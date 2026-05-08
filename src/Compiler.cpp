// compiler/Compiler.cpp
#include "pch/pch.h"
#include "Compiler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

//#include "compiler/Coder.h"
#include "compiler/Emitter.h"
#include "compiler/Morpher.h"
#include "compiler/Parser.h"
#include "compiler/PipelineQueue.h"
#include "compiler/Reviewer.h"
#include "compiler/SymbolTable.h"
#include "compiler/Xmo.h"
#include "tool/bs_thread_pool.hpp"   // or "tool/bs_thread_pool.hpp" depending on how you've laid it out#include "compiler/Lexer.h"
#include "tool/FileMapping.h"
#include "tool/Logger.h"

namespace fs = std::filesystem;

namespace xmc
{
	namespace
	{
		// -----------------------------------------------------------------
		// Per-file pipeline queue metrics, captured at end of file.
		// -----------------------------------------------------------------
		struct FileMetrics
		{
			std::string fileName;
			uint64_t    tokQueueMaxDepth = 0;
			uint64_t    tokQueueMaxBlockNs = 0;
			uint64_t    tokQueueEnq = 0;
			uint64_t    tokQueueDeq = 0;
			uint64_t    nodeQueueMaxDepth = 0;
			uint64_t    nodeQueueMaxBlockNs = 0;
			uint64_t    nodeQueueEnq = 0;
			uint64_t    nodeQueueDeq = 0;
		};

		// -----------------------------------------------------------------
		// CompilerState
		//
		// Per-call state. One instance lives on the stack of Compile() and
		// is threaded through the helper functions below by reference.
		// -----------------------------------------------------------------
		struct CompilerState
		{
			const CompileJob& job;
			SymbolTable                          symbols;
			std::vector<std::unique_ptr<Xmo>>    xmos;
			std::unordered_map<InternedString, uint32_t,
				InternedStringHasher, InternedStringEqual> xmoByName;

			// Captured per-file metrics, in completion order.
			std::mutex                logMetricsMtx;
			std::vector<FileMetrics>  fileMetrics;

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

		bool IsXmModified(const fs::path& xmPath, const fs::path& xmoPath, bool fullBuild)
		{
			if (fullBuild) return true;
			std::error_code ec;
			if (!fs::exists(xmoPath, ec) || ec) return true;
			const auto xmTime = fs::last_write_time(xmPath, ec);
			if (ec) return true;
			const auto xmoTime = fs::last_write_time(xmoPath, ec);
			if (ec) return true;
			return xmTime > xmoTime;
		}

		// -----------------------------------------------------------------
		// TriageFiles
		//
		// Reserves one Xmo slot per source file with stable index. Modified
		// xmos start in Modified; unmodified ones start in Empty until
		// LoadUnmodifiedXmoHeaders advances them to HeadersOnly (or
		// LoadFailed). xmoIdx is stable for the entire job; Symbol::xmoIdx
		// values written later refer back to these indices.
		// -----------------------------------------------------------------
		void TriageFiles(CompilerState& s)
		{
			s.xmos.reserve(s.job.SourceFiles.size());
			const fs::path intDir(s.job.IntDir);

			for (uint32_t i = 0; i < s.job.SourceFiles.size(); ++i) {
				const auto& xmPath = s.job.SourceFiles[i];

				auto xmo = std::make_unique<Xmo>();
				xmo->name = xmPath;
				xmo->internedName = s.symbols.InternString(xmPath);
				xmo->index = i;

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
		//
		// Each Empty xmo gets its .xmo file mapped, headers (exports,
		// scopes, dep-graph edges) deserialized into Xmo's owned containers,
		// and unmapped before LoadFrom returns. Sequential here pending the
		// pool; LoadFrom is independent across xmos and parallelizes when
		// we want.
		// -----------------------------------------------------------------
		void LoadUnmodifiedXmoHeaders(CompilerState& s)
		{
			const fs::path intDir(s.job.IntDir);
			for (auto& xmo : s.xmos) {
				if (xmo->state.load(std::memory_order_relaxed) != XmoState::Empty) continue;
				const fs::path xmoPath = XmoPathFor(xmo->name, intDir);
				xmo->LoadFrom(xmoPath, s.symbols,
					const_cast<CompileJob&>(s.job),
					/*loadParseTree=*/false);
			}
		}

		// -----------------------------------------------------------------
		// RunFilePipeline
		//
		// The per-file work item: maps the .xm, spawns three jthreads
		// (Lex, Parse, Morph) linked by two blocking pipeline queues, and
		// joins them. Runs on a pool worker. The .xm mapping is local to
		// this function and outlives all three jthreads (the joins return
		// before the function exits), so token srcStart/srcLen offsets
		// remain valid for the entire pipeline.
		//
		// Returns when all three pipeline stages have finished. The return
		// path captures the queues' metrics into s.fileMetrics.
		// -----------------------------------------------------------------
		void RunFilePipeline(CompilerState& s, Xmo& xmo)
		{
			if (s.job.ErrorOccurred.load(std::memory_order_relaxed)) return;

			// 1. Map the source. The mapping lives until the end of this
			//    function, after the three jthreads have joined; tokens
			//    indexing into `source` therefore remain valid for the
			//    whole pipeline.
			FileMapping xmMap;
			if (!xmMap.Map(xmo.name)) {
				// FileMapping::Map already logged a diagnostic to oserror.
				s.job.ErrorOccurred.store(true, std::memory_order_relaxed);
				return;
			}

			std::string_view source;
			if (xmMap.Size() > 0) {
				source = std::string_view(
					static_cast<const char*>(xmMap.Base()),
					static_cast<size_t>(xmMap.Size()));
			}

			// 2. Pipeline queues. Stack-local; outlive their consumers
			//    because the jthreads join before this scope exits.
			PipelineQueue<Lexer::Token>     tokQueue;
			PipelineQueue<ParseTreeNode*> nodeQueue;

			// 3. Pipeline stages. Each runs as a jthread; ~jthread joins
			//    on scope exit, which is exactly what we want at the end
			//    of this function.
			//
			//    End-of-stream sentinels:
			//      - tokQueue:   a Token with type == TOK_EOF
			//      - nodeQueue:  a nullptr ParseTreeNode*
			{
				std::jthread lexer([&] {
					Lexer::Lex(xmo.name, source, tokQueue, s.job.LexerLog);
					});

				std::jthread parser([&] {
					Parser::Parse(xmo, source, tokQueue, nodeQueue, s.symbols, s.job);
					});

				std::jthread morpher([&] {
					Morpher::Morph(xmo, nodeQueue, s.symbols, s.job);
					});

				// Joins happen here as the jthreads leave scope.
			}

			// 4. Capture per-file pipeline metrics for end-of-job summary.
			FileMetrics m;
			m.fileName = xmo.name;
			m.tokQueueMaxDepth = tokQueue.MaxDepth();
			m.tokQueueMaxBlockNs = tokQueue.MaxBlockNs();
			m.tokQueueEnq = tokQueue.TotalEnq();
			m.tokQueueDeq = tokQueue.TotalDeq();
			m.nodeQueueMaxDepth = nodeQueue.MaxDepth();
			m.nodeQueueMaxBlockNs = nodeQueue.MaxBlockNs();
			m.nodeQueueEnq = nodeQueue.TotalEnq();
			m.nodeQueueDeq = nodeQueue.TotalDeq();
			{
				std::lock_guard lk(s.logMetricsMtx);
				s.fileMetrics.push_back(std::move(m));
			}
		}

		// -----------------------------------------------------------------
		// RunModifiedPipeline
		//
		// Submits one pool task per Modified xmo and waits for all to
		// finish. Pool size = job.MaxConcurrentFiles, so at most that many
		// .xm files are mapped and in-flight simultaneously; each in-flight
		// file occupies one pool worker plus its own three pipeline
		// jthreads.
		// -----------------------------------------------------------------
		void RunModifiedPipeline(CompilerState& s, BS::thread_pool<>& pool)
		{
			BS::multi_future<void> fileFutures;

			for (auto& xmo : s.xmos) {
				if (xmo->state.load(std::memory_order_relaxed) != XmoState::Modified) continue;

				Xmo* x = xmo.get();
				fileFutures.push_back(pool.submit_task(
					[&s, x] {
						RunFilePipeline(s, *x);
					}
				));
			}

			fileFutures.wait();
		}

		// -----------------------------------------------------------------
		// HandleBreakOuts
		//
		// Single-shot recovery. Each flag fires at most once per job; we
		// honor that by exchanging the flag to false before restarting and
		// not re-checking. The closure used here is conservative -- "every
		// xmo at HeadersOnly/Empty" for a reparse, "every HeadersOnly xmo
		// upgraded to Full" for a rescan. A tighter, observer-frontier
		// closure becomes possible once Morpher records originating xmos
		// via a side channel on CompileJob.
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
				// Re-Morph every Full xmo. This stage doesn't yet stream
				// through pipeline queues -- we re-Morph an already-built
				// tree -- so there's no per-stage threading to set up.
				for (auto& xmo : s.xmos) {
					if (xmo->state.load(std::memory_order_relaxed) == XmoState::Full) {
						Morpher::MorphTree(*xmo, s.symbols, s.job);
					}
				}
			}
		}

		// -----------------------------------------------------------------
		// Per-xmo passes after the pipeline has converged. Sequential for
		// now; each is independent across xmos and parallelizes cleanly
		// through the same pool.
		// -----------------------------------------------------------------
		void RunReviewer(CompilerState& s)
		{
			for (auto& xmo : s.xmos) {
				if (s.job.ErrorOccurred.load(std::memory_order_relaxed)) break;
				Reviewer::Review(*xmo, s.symbols, s.job);
			}
		}

		void RunCoder(CompilerState& s)
		{
			for (auto& xmo : s.xmos) {
				if (s.job.ErrorOccurred.load(std::memory_order_relaxed)) break;
				// Coder::Code(*xmo, s.job);
			}
		}

		void RunEmitterPhase1(CompilerState& s)
		{
			for (auto& xmo : s.xmos) {
				if (s.job.ErrorOccurred.load(std::memory_order_relaxed)) break;
				Emitter::EmitPhase1(*xmo, s.job);
			}
		}

		// -----------------------------------------------------------------
		// LogMetrics
		//
		// Writes per-file pipeline-queue metrics to osdebug. Helpful for
		// tuning queue sizes, deciding whether stages need more or less
		// overlap, and understanding back-pressure under load.
		// -----------------------------------------------------------------
		void LogMetrics(CompilerState& s)
		{
			if (s.fileMetrics.empty()) return;

			osdebug << "Pipeline queue metrics:" << std::endl;
			osdebug << "  file"
				<< "  tok(maxDepth, maxBlockUs, enq, deq)"
				<< "  node(maxDepth, maxBlockUs, enq, deq)"
				<< std::endl;

			for (const auto& m : s.fileMetrics) {
				osdebug << "  " << m.fileName
					<< "  tok(" << m.tokQueueMaxDepth
					<< ", " << (m.tokQueueMaxBlockNs / 1000)
					<< ", " << m.tokQueueEnq
					<< ", " << m.tokQueueDeq << ")"
					<< "  node(" << m.nodeQueueMaxDepth
					<< ", " << (m.nodeQueueMaxBlockNs / 1000)
					<< ", " << m.nodeQueueEnq
					<< ", " << m.nodeQueueDeq << ")"
					<< std::endl;
			}
		}
	} // namespace

	// ---------------------------------------------------------------------
	// Compiler::Compile
	// ---------------------------------------------------------------------
	void Compiler::Compile(const CompileJob& job)
	{
		CompilerState s(job);

		TriageFiles(s);
		LoadUnmodifiedXmoHeaders(s);
		if (job.ErrorOccurred.load(std::memory_order_relaxed)) {
			LogMetrics(s);
			return;
		}

		// Pool is sized by max concurrent files, not by total stage threads
		// -- the three pipeline jthreads per file run *outside* the pool.
		BS::thread_pool<> pool(job.MaxConcurrentFiles == 0 ? 1 : job.MaxConcurrentFiles);

		RunModifiedPipeline(s, pool);
		HandleBreakOuts(s, pool);
		if (job.ErrorOccurred.load(std::memory_order_relaxed)) {
			LogMetrics(s);
			return;
		}

		RunReviewer(s);
		if (job.ErrorOccurred.load(std::memory_order_relaxed)) {
			LogMetrics(s);
			return;
		}

		RunCoder(s);
		if (job.ErrorOccurred.load(std::memory_order_relaxed)) {
			LogMetrics(s);
			return;
		}

		RunEmitterPhase1(s);
		if (job.ErrorOccurred.load(std::memory_order_relaxed)) {
			LogMetrics(s);
			return;
		}

		Emitter::EmitPhase2(s.xmos, job);

		LogMetrics(s);
	}

} // namespace xmc