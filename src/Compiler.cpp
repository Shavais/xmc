// Compiler.cpp
//
// Drives one CompileJob end-to-end: triage, load unmodified .xmo headers,
// per-file Parser tasks (one pool task per modified .xm), barrier and
// break-out recovery, Reviewer, Coder, Emitter.
//
// The Parser now owns the full lex/parse/morph loop for a single file.
// It maps no files itself; the Compiler maps each .xm and passes a
// string_view so arena-allocated token offsets remain valid throughout.
// The Parser submits Morpher tasks to the shared pool as it finishes
// noun nodes; those tasks run concurrently with the Parser's continued
// descent into later nodes.
//
#include "pch/pch.h"
#include "Compiler.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/Coder.h"
#include "compiler/Emitter.h"
#include "compiler/Morpher.h"
#include "compiler/Parser.h"
#include "compiler/Reviewer.h"
#include "compiler/SymbolTable.h"
#include "compiler/Xmo.h"
#include "tool/BS_thread_pool.hpp"
#include "tool/FileMapping.h"
#include "tool/Logger.h"

namespace fs = std::filesystem;

namespace xmc
{
    namespace
    {
        // -----------------------------------------------------------------
        // CompilerState
        //
        // Per-call state. One instance lives on the stack of Compile() and
        // is threaded through the helper functions below by reference.
        // -----------------------------------------------------------------
        struct CompilerState
        {
            const CompileJob& job;
            SymbolTable       symbols;

            std::vector<std::unique_ptr<Xmo>>            xmos;
            std::unordered_map<InternedString, uint32_t,
                InternedStringHasher, InternedStringEqual> xmoByName;

            explicit CompilerState(const CompileJob& j) : job(j) {}

            Xmo* FindXmo(const XmoKey& key)
            {
                auto it = xmoByName.find(key.name);
                return it == xmoByName.end() ? nullptr : xmos[it->second].get();
            }
        };

        // -----------------------------------------------------------------
        // File-system helpers
        // -----------------------------------------------------------------
        fs::path XmoPathFor(const fs::path& xmPath, const fs::path& intDir)
        {
            return intDir / (xmPath.stem().string() + ".xmo");
        }

        bool IsXmModified(const fs::path& xmPath,
                          const fs::path& xmoPath,
                          bool            fullBuild)
        {
            if (fullBuild) return true;
            std::error_code ec;
            if (!fs::exists(xmoPath, ec) || ec) return true;
            const auto xmTime  = fs::last_write_time(xmPath,  ec); if (ec) return true;
            const auto xmoTime = fs::last_write_time(xmoPath, ec); if (ec) return true;
            return xmTime > xmoTime;
        }

        // -----------------------------------------------------------------
        // TriageFiles
        //
        // Reserves one Xmo slot per source file with a stable index.
        // Modified xmos start in Modified; unmodified ones start in Empty
        // until LoadUnmodifiedXmoHeaders advances them to HeadersOnly.
        // -----------------------------------------------------------------
        void TriageFiles(CompilerState& s)
        {
            s.xmos.reserve(s.job.SourceFiles.size());
            const fs::path intDir(s.job.IntDir);

            for (uint32_t i = 0; i < s.job.SourceFiles.size(); ++i) {
                const auto& xmPath = s.job.SourceFiles[i];

                auto xmo          = std::make_unique<Xmo>();
                xmo->name         = xmPath;
                xmo->internedName = s.symbols.InternString(xmPath);
                xmo->index        = i;

                const bool modified = IsXmModified(
                    xmPath, XmoPathFor(xmPath, intDir), s.job.Full);
                xmo->state.store(
                    modified ? XmoState::Modified : XmoState::Empty,
                    std::memory_order_relaxed);

                s.xmoByName.emplace(xmo->internedName, i);
                s.xmos.push_back(std::move(xmo));
            }
        }

        // -----------------------------------------------------------------
        // LoadUnmodifiedXmoHeaders
        // -----------------------------------------------------------------
        void LoadUnmodifiedXmoHeaders(CompilerState& s)
        {
            const fs::path intDir(s.job.IntDir);
            for (auto& xmo : s.xmos) {
                if (xmo->state.load(std::memory_order_relaxed) != XmoState::Empty)
                    continue;
                const fs::path xmoPath = XmoPathFor(xmo->name, intDir);
                xmo->LoadFrom(xmoPath, s.symbols,
                              const_cast<CompileJob&>(s.job),
                              /*loadParseTree=*/false);
            }
        }

        // -----------------------------------------------------------------
        // RunFileParser
        //
        // The per-file pool task. Maps the .xm source, then calls
        // Parser::Parse which owns the full lex/parse loop and submits
        // Morpher tasks to the pool as noun nodes complete.
        //
        // The FileMapping outlives the Parse call (and therefore all
        // Morpher tasks that reference source offsets), because those tasks
        // complete before MorphTree returns at the end of Parse().
        // -----------------------------------------------------------------
        void RunFileParser(CompilerState& s, Xmo& xmo, BS::thread_pool<>& pool)
        {
            if (s.job.ErrorOccurred.load(std::memory_order_relaxed)) return;

            FileMapping xmMap;
            if (!xmMap.Map(xmo.name)) {
                s.job.ErrorOccurred.store(true, std::memory_order_relaxed);
                return;
            }

            std::string_view source;
            if (xmMap.Size() > 0) {
                source = std::string_view(
                    static_cast<const char*>(xmMap.Base()),
                    static_cast<size_t>(xmMap.Size()));
            }

            Parser::Parse(xmo, source, s.symbols, pool, s.job);
        }

        // -----------------------------------------------------------------
        // RunModifiedPipeline
        //
        // Submits one pool task per Modified xmo and waits for all of them.
        // Parser tasks submit leaf Morpher tasks during Drive(), so after the
        // file futures resolve there may still be Morpher tasks in flight.
        // pool.wait_for_tasks() drains those before the morpher log is written.
        // -----------------------------------------------------------------
        void RunModifiedPipeline(CompilerState& s, BS::thread_pool<>& pool)
        {
            BS::multi_future<void> fileFutures;

            for (auto& xmo : s.xmos) {
                if (xmo->state.load(std::memory_order_relaxed) != XmoState::Modified)
                    continue;

                Xmo* x = xmo.get();
                fileFutures.push_back(pool.submit_task(
                    [&s, x, &pool] {
                        RunFileParser(s, *x, pool);
                    }
                ));
            }

            fileFutures.wait();

            // Drain any Morpher leaf tasks that are still running or queued.
            // Safe to call from the main thread (not a pool worker).
            pool.wait();

            // Write per-file morpher logs now that all morphing is complete.
            if (s.job.MorpherLog) {
                for (auto& xmo : s.xmos) {
                    if (xmo->state.load(std::memory_order_relaxed) != XmoState::Modified)
                        continue;
                    Morpher::MorphTree(*xmo, s.symbols, s.job);
                }
            }
        }

        // -----------------------------------------------------------------
        // HandleBreakOuts
        //
        // Single-shot recovery when a structural or scan break-out was
        // signalled by the Morpher.
        // -----------------------------------------------------------------
        void HandleBreakOuts(CompilerState& s, BS::thread_pool<>& pool)
        {
            if (s.job.ReparseRequired.exchange(false, std::memory_order_acq_rel)) {
                for (auto& xmo : s.xmos) {
                    const auto state = xmo->state.load(std::memory_order_relaxed);
                    if (state == XmoState::HeadersOnly || state == XmoState::Empty) {
                        xmo->ResetForRegeneration();
                        xmo->state.store(XmoState::Modified, std::memory_order_relaxed);
                    }
                }
                RunModifiedPipeline(s, pool);
            }

            if (s.job.ParseTreeScanRequired.exchange(false, std::memory_order_acq_rel)) {
                const fs::path intDir(s.job.IntDir);
                for (auto& xmo : s.xmos) {
                    if (xmo->state.load(std::memory_order_relaxed) == XmoState::HeadersOnly) {
                        const fs::path xmoPath = XmoPathFor(xmo->name, intDir);
                        xmo->LoadParseTree(xmoPath, s.symbols,
                                           const_cast<CompileJob&>(s.job));
                    }
                }
                for (auto& xmo : s.xmos) {
                    if (xmo->state.load(std::memory_order_relaxed) == XmoState::Full) {
                        // TODO: submit leaf Morpher tasks for the loaded parse tree,
                        // then call pool.wait_for_tasks() before MorphTree.
                        Morpher::MorphTree(*xmo, s.symbols, s.job);
                    }
                }
            }
        }

        // -----------------------------------------------------------------
        // Per-xmo passes after the pipeline has converged.
        // -----------------------------------------------------------------
        void RunReviewer(CompilerState& s, BS::thread_pool<>& pool)
        {
            BS::multi_future<void> futures;
            for (auto& xmo : s.xmos) {
                if (xmo->state.load(std::memory_order_relaxed) != XmoState::Modified)
                    continue;
                Xmo* x = xmo.get();
                futures.push_back(pool.submit_task([&s, x] {
                    if (!s.job.ErrorOccurred.load(std::memory_order_relaxed))
                        Reviewer::Review(*x, s.symbols, s.job);
                }));
            }
            futures.wait();
        }

        void RunCoder(CompilerState& s, BS::thread_pool<>& pool)
        {
            BS::multi_future<void> futures;
            for (auto& xmo : s.xmos) {
                if (xmo->state.load(std::memory_order_relaxed) != XmoState::Modified)
                    continue;
                Xmo* x = xmo.get();
                futures.push_back(pool.submit_task([&s, x] {
                    if (!s.job.ErrorOccurred.load(std::memory_order_relaxed))
                        Coder::Code(*x, s.job);
                }));
            }
            futures.wait();
        }

        void RunEmitterPhase1(CompilerState& s)
        {
            for (auto& xmo : s.xmos) {
                if (s.job.ErrorOccurred.load(std::memory_order_relaxed)) break;
                Emitter::EmitPhase1(*xmo, s.job);
            }
        }

    } // namespace

    // -------------------------------------------------------------------------
    // Compiler::Compile
    // -------------------------------------------------------------------------
    void Compiler::Compile(const CompileJob& job)
    {
        CompilerState s(job);

        TriageFiles(s);
        LoadUnmodifiedXmoHeaders(s);
        if (job.ErrorOccurred.load(std::memory_order_relaxed)) return;

        // Pool is sized by max concurrent files. Morpher tasks for a file
        // also run inside this pool, so size >= 2 allows parse/morph overlap.
        const uint32_t poolSize = job.MaxConcurrentFiles == 0
                                    ? 2u : job.MaxConcurrentFiles;
        BS::thread_pool<> pool(poolSize);

        RunModifiedPipeline(s, pool);
        HandleBreakOuts(s, pool);
        if (job.ErrorOccurred.load(std::memory_order_relaxed)) return;

        RunReviewer(s, pool);
        if (job.ErrorOccurred.load(std::memory_order_relaxed)) return;

        RunCoder(s, pool);
        if (job.ErrorOccurred.load(std::memory_order_relaxed)) return;

        RunEmitterPhase1(s);
        if (job.ErrorOccurred.load(std::memory_order_relaxed)) return;

        Emitter::EmitPhase2(s.xmos, job);
    }

} // namespace xmc
