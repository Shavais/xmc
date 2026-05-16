// compiler/Reviewer.cpp
//
// Post-Morpher codegen-prep pass (Windows x64 ABI).
//
// For each FuncDecl in the parse tree:
//   - Assigns paramSlot on every arg expression under each Call node.
//     Slots 1-4 map to rcx/rdx/r8/r9 (from the callee's Param regHint);
//     slot 5+ are stack positions [rsp+32], [rsp+40], …
//   - Computes totalStackSize: shadow(32) + extra arg slots(8 each) + locals(8 each),
//     rounded to the next value ≡ 8 (mod 16) so rsp stays 16-byte aligned at call sites.
//   - Assigns Symbol::offset for each stack local: rsp-relative address within the frame.
//     Layout: locals begin at rsp+(32 + extraArgSlots*8).
//   - Sets funcFlags = 0 (isFallible / listening / discarding flavors not needed yet).
//
// Array-typed locals (e.g. u8[] string literals) are stored in .rdata, not on the
// stack, so they are excluded from the local-count and offset assignment.
//
#include "pch/pch.h"
#include "Reviewer.h"

#include "Parser.h"

namespace fs = std::filesystem;

namespace xmc
{
    namespace
    {
        // -----------------------------------------------------------------
        // Helpers shared across review and log passes
        // -----------------------------------------------------------------

        static ParseTreeNode* FirstChildOfKind(ParseTreeNode* parent, ParseKind k)
        {
            for (uint32_t i = 0; i < parent->childCount; ++i)
                if (parent->children[i]->kind == k) return parent->children[i];
            return nullptr;
        }

        // Map a Param regHint to a paramSlot (1-4). Returns 0 if absent/unknown.
        static uint8_t RegHintToSlot(const InternedString& hint)
        {
            if (!hint.str || hint.len == 0) return 0;
            std::string_view h(hint.str, hint.len);
            if (h == "rcx") return 1;
            if (h == "rdx") return 2;
            if (h == "r8")  return 3;
            if (h == "r9")  return 4;
            return 0;
        }

        // Walk subtree of n, assign paramSlots on all ArgList children of Call
        // nodes, and return the maximum arg count seen in any single call.
        // Does not recurse into child FuncDecl nodes (those are separate frames).
        static uint32_t AssignParamSlots(ParseTreeNode* n)
        {
            uint32_t maxArgs = 0;

            if (n->kind == ParseKind::Call) {
                ParseTreeNode* argList = FirstChildOfKind(n, ParseKind::ArgList);
                if (argList && argList->childCount > 0) {
                    ParseTreeNode* calleeIdent = n->childCount > 0 ? n->children[0] : nullptr;
                    Symbol*        sym         = calleeIdent ? calleeIdent->declaredSymbol : nullptr;
                    ParseTreeNode* decl        = sym ? sym->declNode : nullptr;
                    ParseTreeNode* paramList   = decl ? FirstChildOfKind(decl, ParseKind::ParamList) : nullptr;

                    uint8_t nextStack = 5;
                    for (uint32_t i = 0; i < argList->childCount; ++i) {
                        uint8_t slot = 0;
                        if (paramList && i < paramList->childCount)
                            slot = RegHintToSlot(paramList->children[i]->regHint);
                        if (slot == 0) slot = nextStack++;
                        argList->children[i]->paramSlot = slot;
                    }
                    maxArgs = argList->childCount;
                }
            }

            for (uint32_t i = 0; i < n->childCount; ++i) {
                if (n->children[i]->kind == ParseKind::FuncDecl) continue;
                maxArgs = std::max(maxArgs, AssignParamSlots(n->children[i]));
            }

            return maxArgs;
        }

        // Collect stack-allocated VarDecl nodes in the subtree of n.
        // Excludes array-typed locals (those live in .rdata, not on the stack).
        // Does not recurse into child FuncDecl nodes.
        static void CollectLocals(ParseTreeNode* n, std::vector<ParseTreeNode*>& out)
        {
            if (n->kind == ParseKind::VarDecl && n->declaredSymbol && !n->isArray)
                out.push_back(n);
            for (uint32_t i = 0; i < n->childCount; ++i) {
                if (n->children[i]->kind == ParseKind::FuncDecl) continue;
                CollectLocals(n->children[i], out);
            }
        }

        // Collect all Call nodes in the subtree of n.
        // Does not recurse into child FuncDecl nodes.
        static void CollectCalls(ParseTreeNode* n, std::vector<ParseTreeNode*>& out)
        {
            if (n->kind == ParseKind::Call) out.push_back(n);
            for (uint32_t i = 0; i < n->childCount; ++i) {
                if (n->children[i]->kind == ParseKind::FuncDecl) continue;
                CollectCalls(n->children[i], out);
            }
        }

        // -----------------------------------------------------------------
        // Core review logic
        // -----------------------------------------------------------------

        static void ReviewFuncDecl(ParseTreeNode* funcDecl)
        {
            // Pass 1: assign paramSlots and find the deepest call.
            uint32_t maxArgs = AssignParamSlots(funcDecl);

            // Pass 2: collect stack locals.
            std::vector<ParseTreeNode*> locals;
            CollectLocals(funcDecl, locals);

            // Compute frame size.
            // Windows x64 layout after `sub rsp, totalStackSize`:
            //   rsp+0  .. rsp+31               shadow space (always)
            //   rsp+32 .. rsp+32+extra*8-1     extra arg slots (args beyond 4)
            //   rsp+prefix .. rsp+prefix+locals*8-1  locals
            uint32_t extraArgSlots = maxArgs > 4 ? maxArgs - 4 : 0;
            uint32_t prefix        = 32 + extraArgSlots * 8;
            uint32_t size          = prefix + uint32_t(locals.size()) * 8;

            // Round up to the nearest N ≡ 8 (mod 16) so rsp is 16-byte
            // aligned at every call site inside the function.
            uint32_t rem = size % 16;
            if (rem != 8)
                size += (8 + 16 - rem) % 16;

            funcDecl->totalStackSize = size;
            funcDecl->funcFlags      = 0;

            // Assign rsp-relative offset to each stack local.
            for (uint32_t i = 0; i < uint32_t(locals.size()); ++i)
                locals[i]->declaredSymbol->offset = prefix + i * 8;
        }

        // Walk the full parse tree, reviewing every FuncDecl.
        static void ReviewTree(ParseTreeNode* n)
        {
            if (!n) return;
            if (n->kind == ParseKind::FuncDecl) ReviewFuncDecl(n);
            for (uint32_t i = 0; i < n->childCount; ++i)
                ReviewTree(n->children[i]);
        }

        // -----------------------------------------------------------------
        // Log writing
        // -----------------------------------------------------------------

        static void LogFuncDecl(std::ofstream& out, ParseTreeNode* funcDecl)
        {
            out << "FuncDecl ";
            if (funcDecl->name.str && funcDecl->name.len)
                out.write(funcDecl->name.str, funcDecl->name.len);
            out << " (line " << funcDecl->line << ")\n";
            out << "  totalStackSize = " << funcDecl->totalStackSize << "\n";
            out << "  funcFlags      = " << funcDecl->funcFlags << "\n";

            std::vector<ParseTreeNode*> locals;
            CollectLocals(funcDecl, locals);
            if (!locals.empty()) {
                out << "  locals:\n";
                for (auto* vd : locals) {
                    out << "    ";
                    if (vd->name.str) out.write(vd->name.str, vd->name.len);
                    out << ": rsp+" << vd->declaredSymbol->offset << "\n";
                }
            }

            std::vector<ParseTreeNode*> calls;
            CollectCalls(funcDecl, calls);
            if (!calls.empty()) {
                out << "  calls:\n";
                for (auto* call : calls) {
                    out << "    ";
                    if (call->childCount > 0 && call->children[0]->name.str)
                        out.write(call->children[0]->name.str, call->children[0]->name.len);
                    out << " (line " << call->line << "):";
                    ParseTreeNode* argList = FirstChildOfKind(call, ParseKind::ArgList);
                    if (argList) {
                        for (uint32_t i = 0; i < argList->childCount; ++i)
                            out << " arg[" << i << "]=slot" << int(argList->children[i]->paramSlot);
                    }
                    out << "\n";
                }
            }
        }

        static void LogTree(std::ofstream& out, ParseTreeNode* n)
        {
            if (!n) return;
            if (n->kind == ParseKind::FuncDecl) LogFuncDecl(out, n);
            for (uint32_t i = 0; i < n->childCount; ++i)
                LogTree(out, n->children[i]);
        }

        static void WriteReviewerLog(const Xmo& xmo)
        {
            fs::path src(xmo.name);
            fs::path logPath = src.parent_path() /
                (src.stem().string() + ".reviewer.txt");
            std::ofstream out(logPath);
            if (!out) return;
            LogTree(out, xmo.parseTree);
        }

    } // namespace

    // -----------------------------------------------------------------
    // Public entry point
    // -----------------------------------------------------------------

    void Reviewer::Review(Xmo& xmo, SymbolTable& symbols, const CompileJob& job)
    {
        ReviewTree(xmo.parseTree);

        if (job.ReviewerLog)
            WriteReviewerLog(xmo);

        (void)symbols;
    }

} // namespace xmc
