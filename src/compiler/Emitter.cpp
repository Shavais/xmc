// compiler/Emitter.cpp
//
// Phase 1: post-order tree walk that fills xmo.codeBuffer with raw x64
//          machine code.  Patches immediate values in-place; records
//          relocations (calls, RIP-relative string refs) in xmo.relocs
//          and xmo.relocTargetNames.
//
// Phase 2: single-threaded COFF assembly.  Creates .text and .rdata
//          sections, defines symbols, applies COFF IMAGE_REL_AMD64_REL32
//          relocations, and calls Coff::WriteTo.
//
// FuncDecl special case: block[0] (prologue) is emitted BEFORE children;
// blocks[1..N-1] (xor_eax_eax + epilogue) are emitted AFTER children.
// All other nodes use strict post-order (children before blocks).
//
#include "pch/pch.h"
#include "Emitter.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "CodeBlock.h"
#include "Parser.h"
#include "../tool/Coff.h"

namespace fs = std::filesystem;

namespace xmc
{
    namespace
    {
        // ----------------------------------------------------------------
        // Phase 1 helpers
        // ----------------------------------------------------------------

        // Record a relocation for a just-emitted block.
        // kind 0 = external call; kind 1 = .rdata string reference.
        static void PushReloc(Xmo& xmo, uint32_t offsetInBuffer,
                              uint32_t kind,
                              const char* name, uint32_t nameLen)
        {
            uint32_t nameIdx = (uint32_t)xmo.relocTargetNames.size();
            xmo.relocTargetNames.emplace_back(name, nameLen);

            XmoRelocation r{};
            r.offset           = offsetInBuffer;
            r.targetNameOffset = nameIdx;   // index into relocTargetNames
            r.targetNameLen    = 0;         // unused in-memory; get from vector
            r.kind             = kind;
            xmo.relocs.push_back(r);
        }

        // Emit one CodeBlock and apply its patch / relocation.
        // n   = the parse-tree node that owns this blockId.
        // func = the enclosing FuncDecl (for totalStackSize patches).
        static void EmitBlockId(uint16_t id, ParseTreeNode* n,
                                Xmo& xmo, ParseTreeNode* func)
        {
            const CodeBlock& cb = CodeBlocks[id];
            uint32_t start = (uint32_t)xmo.codeBuffer.size();

            for (uint16_t i = 0; i < cb.codeSize; ++i)
                xmo.codeBuffer.push_back(cb.code[i]);

            if (cb.patchOffset == 0xFF) return;

            uint8_t* patch = xmo.codeBuffer.data() + start + cb.patchOffset;

            if (id == bid.prologue || id == bid.epilogue) {
                // 1-byte immediate = frame size
                *patch = (uint8_t)(func ? func->totalStackSize : 0);

            } else if (id == bid.call_rel32) {
                // Relocation to callee; callee Ident is children[0]
                if (n->childCount > 0) {
                    ParseTreeNode* callee = n->children[0];
                    if (callee->name.str)
                        PushReloc(xmo, start + cb.patchOffset, 0,
                                  callee->name.str, callee->name.len);
                }

            } else if (id == bid.mov_ecx_imm32 || id == bid.mov_r8d_imm32) {
                uint32_t v = (uint32_t)n->intValue;
                memcpy(patch, &v, 4);

            } else if (id == bid.mov_rcx_rsp_off8) {
                if (n->declaredSymbol)
                    *patch = (uint8_t)n->declaredSymbol->offset;

            } else if (id == bid.lea_rdx_rip32) {
                // Relocation to string in .rdata; target name = variable name.
                // Resolved against the per-xmo localStrMap in EmitPhase2,
                // so same-named locals in different xmos never collide.
                if (n->declaredSymbol && n->declaredSymbol->name.str)
                    PushReloc(xmo, start + cb.patchOffset, 1,
                              n->declaredSymbol->name.str,
                              n->declaredSymbol->name.len);

            } else if (id == bid.lea_r9_rsp_off8) {
                *patch = (uint8_t)n->intValue;

            } else if (id == bid.mov_rsp_off8_rax) {
                if (n->declaredSymbol)
                    *patch = (uint8_t)n->declaredSymbol->offset;
            }
            // mov_rsp32_imm0 (id 10): patchOffset=0xFF already handled above
        }

        // Post-order tree walk that emits all code blocks.
        static void EmitNode(ParseTreeNode* n, Xmo& xmo, ParseTreeNode* func)
        {
            if (!n) return;

            if (n->kind == ParseKind::FuncDecl) {
                // Record offset before emitting prologue
                uint32_t funcOffset = (uint32_t)xmo.codeBuffer.size();
                if (n->name.str)
                    xmo.funcSymbols.emplace_back(
                        std::string(n->name.str, n->name.len), funcOffset);

                // Emit prologue (block[0]) BEFORE children
                if (n->codeBlockCount > 0)
                    EmitBlockId(n->codeBlocks[0], n, xmo, n);

                for (uint32_t i = 0; i < n->childCount; ++i)
                    EmitNode(n->children[i], xmo, n);

                // Emit remaining blocks (xor_eax_eax, epilogue) AFTER children
                for (uint32_t b = 1; b < n->codeBlockCount; ++b)
                    EmitBlockId(n->codeBlocks[b], n, xmo, n);

                return;
            }

            // All other nodes: post-order
            for (uint32_t i = 0; i < n->childCount; ++i)
                EmitNode(n->children[i], xmo, func);

            for (uint32_t b = 0; b < n->codeBlockCount; ++b)
                EmitBlockId(n->codeBlocks[b], n, xmo, func);
        }

        // ----------------------------------------------------------------
        // Phase 1 log
        // ----------------------------------------------------------------

        static void WriteEmitterLog(const Xmo& xmo)
        {
            fs::path src(xmo.name);
            fs::path logPath = src.parent_path() /
                               (src.stem().string() + ".emitter.txt");
            std::ofstream out(logPath);
            if (!out) return;

            out << "Code buffer: " << xmo.codeBuffer.size() << " bytes\n\n";

            for (size_t i = 0; i < xmo.codeBuffer.size(); ++i) {
                char hex[4];
                snprintf(hex, sizeof(hex), "%02X ", (unsigned)xmo.codeBuffer[i]);
                out.write(hex, 3);
                if ((i + 1) % 16 == 0) out << '\n';
            }
            if (xmo.codeBuffer.size() % 16 != 0) out << '\n';

            out << "\nRelocations: " << xmo.relocs.size() << '\n';
            for (size_t i = 0; i < xmo.relocs.size(); ++i) {
                const auto& r = xmo.relocs[i];
                const std::string& name =
                    (r.targetNameOffset < (uint32_t)xmo.relocTargetNames.size())
                    ? xmo.relocTargetNames[r.targetNameOffset] : "?";
                out << "  [" << i << "] offset=" << r.offset
                    << "  kind=" << r.kind
                    << "  target=" << name << '\n';
            }

            out << "\nFunctions:\n";
            for (const auto& [name, off] : xmo.funcSymbols)
                out << "  " << name << " @ " << off << '\n';
        }

        // ----------------------------------------------------------------
        // Phase 2 helpers
        // ----------------------------------------------------------------

        // Walk the tree collecting array VarDecl nodes whose StringLit child
        // holds the escape-resolved bytes; emit each into .rdata and record
        // the COFF symbol index in localStrMap.
        // localStrMap is per-xmo and never shared across xmos, so the bare
        // variable name is a sufficient key — same-named locals in different
        // xmos each get their own symbol in .rdata.
        static void CollectStringLits(
            ParseTreeNode* n, Coff& coff, uint16_t rdataIdx,
            std::unordered_map<std::string, uint32_t>& localStrMap)
        {
            if (!n) return;

            if (n->kind == ParseKind::VarDecl && n->isArray && n->name.str) {
                for (uint32_t i = 0; i < n->childCount; ++i) {
                    if (n->children[i]->kind == ParseKind::StringLit) {
                        ParseTreeNode* lit = n->children[i];
                        std::string varName(n->name.str, n->name.len);
                        if (!localStrMap.count(varName) && lit->name.str) {
                            std::vector<uint8_t> data(
                                reinterpret_cast<const uint8_t*>(lit->name.str),
                                reinterpret_cast<const uint8_t*>(lit->name.str) + lit->name.len);
                            localStrMap[varName] = coff.AddDataSymbol(varName, rdataIdx, data,
                                                                      IMAGE_SYM_CLASS_STATIC);
                        }
                        break;
                    }
                }
            }

            for (uint32_t i = 0; i < n->childCount; ++i)
                CollectStringLits(n->children[i], coff, rdataIdx, localStrMap);
        }

    } // namespace

    // ----------------------------------------------------------------
    // Public entry points
    // ----------------------------------------------------------------

    void Emitter::EmitPhase1(Xmo& xmo, const CompileJob& job)
    {
        if (!xmo.parseTree) return;

        EmitNode(xmo.parseTree, xmo, nullptr);

        if (job.EmitterLog)
            WriteEmitterLog(xmo);
    }

    void Emitter::EmitPhase2(
        const std::vector<std::unique_ptr<Xmo>>& xmos,
        const CompileJob&                        job)
    {
        Coff     coff;
        uint16_t textIdx  = coff.CreateSection(".text",  SectionType::Code);
        uint16_t rdataIdx = coff.CreateSection(".rdata", SectionType::ReadOnly);

        // Unified symbol map: name → COFF symbol table index.
        // A name is added as external (undefined) on first use; upgraded to
        // defined in-place when its definition is encountered later.
        std::unordered_map<std::string, uint32_t> symbolMap;

        auto getOrAddExternal = [&](const std::string& name) -> uint32_t {
            auto [it, inserted] = symbolMap.emplace(name, 0u);
            if (inserted) it->second = coff.AddExternalSymbol(name);
            return it->second;
        };

        for (const auto& xmo : xmos) {
            // Per-xmo map for string literal .rdata symbols. These are never
            // cross-xmo references, so a fresh map per xmo avoids collisions
            // between same-named locals in different files.
            std::unordered_map<std::string, uint32_t> localStrMap;
            if (xmo->parseTree)
                CollectStringLits(xmo->parseTree, coff, rdataIdx, localStrMap);

            if (xmo->codeBuffer.empty()) continue;

            // Snapshot the current .text write cursor — this is the base
            // offset for all symbols and relocations in this xmo.
            uint32_t baseOff = coff.GetSectionBufferSize(textIdx);

            // Define or upgrade each function exported by this xmo.
            for (const auto& [name, off] : xmo->funcSymbols) {
                auto [it, inserted] = symbolMap.emplace(name, 0u);
                if (inserted)
                    it->second = coff.DefineSymbol(name, baseOff + off,
                                                   textIdx, IMAGE_SYM_CLASS_EXTERNAL);
                else
                    coff.UpgradeSymbol(it->second, baseOff + off, textIdx);
            }

            coff.AddRawBytes(textIdx, xmo->codeBuffer);

            for (const auto& r : xmo->relocs) {
                if (r.targetNameOffset >= (uint32_t)xmo->relocTargetNames.size()) continue;
                const std::string& name = xmo->relocTargetNames[r.targetNameOffset];
                uint32_t absOff = baseOff + r.offset;

                if (r.kind == 0) {
                    // Function call: get-or-create (as external), then relocate.
                    coff.AddRelocation(textIdx, absOff,
                                       getOrAddExternal(name),
                                       IMAGE_REL_AMD64_REL32);
                } else if (r.kind == 1) {
                    // String ref: look up in this xmo's local string map.
                    auto it = localStrMap.find(name);
                    if (it != localStrMap.end())
                        coff.AddRelocation(textIdx, absOff, it->second,
                                           IMAGE_REL_AMD64_REL32);
                }
            }
        }

        coff.WriteTo(job.ObjPath);
    }

} // namespace xmc
