// compiler/Coder.cpp
//
// Walks the parse tree and assigns codeBlocks indices to nodes.
//
// For hello.xm only these node kinds produce code:
//
//   FuncDecl        [prologue, xor_eax_eax, epilogue]
//                   Emitter emits prologue BEFORE walking children,
//                   then {xor_eax_eax, epilogue} AFTER (implicit return 0).
//
//   Call            [call_rel32]  — after ArgList children set up registers
//
//   VarDecl         [mov_rsp_off8_rax]  — if non-array with initializer;
//                   stores rax (callee return) to the local's stack slot
//
//   IntLit          (paramSlot 1) [mov_ecx_imm32]
//   Ident (stack)   (paramSlot 1) [mov_rcx_rsp_off8]
//   Ident (array)   (paramSlot 2) [lea_rdx_rip32]
//   MemberAccess    (paramSlot 3) [mov_r8d_imm32]  — string .length
//   AddressOf       (paramSlot 4) [lea_r9_rsp_off8]
//   NullLit         (paramSlot 5) [mov_rsp32_imm0]
//
// The Coder also pre-computes patch values that the Emitter cannot derive
// cheaply from the node alone:
//   MemberAccess.intValue  ← escape-resolved byte length of the string
//   AddressOf.intValue     ← Symbol::offset of the addressed local
//
#include "pch/pch.h"
#include "Coder.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "CodeBlock.h"
#include "Parser.h"
#include "Types.h"

namespace fs = std::filesystem;

namespace xmc
{
    namespace
    {
        // Commit a codeBlocks vector into the arena and store on the node.
        static void SetBlocks(ParseTreeNode* n, Xmo& xmo,
                              std::initializer_list<uint16_t> ids)
        {
            uint32_t count = uint32_t(ids.size());
            if (count == 0) return;
            uint16_t* arr = xmo.arena.NewArray<uint16_t>(count);
            uint32_t  i   = 0;
            for (uint16_t id : ids) arr[i++] = id;
            n->codeBlocks     = arr;
            n->codeBlockCount = count;
        }

        static ParseTreeNode* FirstChildOfKind(ParseTreeNode* n, ParseKind k)
        {
            for (uint32_t i = 0; i < n->childCount; ++i)
                if (n->children[i]->kind == k) return n->children[i];
            return nullptr;
        }

        // Resolve string byte-length for a MemberAccess ".length" on a u8[] local.
        // Navigates: obj Ident → VarDecl → StringLit child → intValue.
        static uint64_t StringLengthOf(ParseTreeNode* memberAccess)
        {
            if (memberAccess->childCount == 0) return 0;
            ParseTreeNode* obj = memberAccess->children[0]; // Ident of the array
            Symbol*        sym = obj->declaredSymbol;
            ParseTreeNode* decl = sym ? sym->declNode : nullptr;
            if (!decl) return 0;
            ParseTreeNode* lit = FirstChildOfKind(decl, ParseKind::StringLit);
            return lit ? lit->intValue : 0;
        }

        // Walk the tree and assign code blocks. Pre-order is fine: block
        // assignments don't depend on children's assignments.
        static void CodeNode(ParseTreeNode* n, Xmo& xmo)
        {
            if (!n) return;

            switch (n->kind) {

            case ParseKind::FuncDecl:
                // [prologue] before body; [xor_eax_eax, epilogue] after.
                // The Emitter distinguishes them by position in the vector:
                // index 0 = pre-children, indices 1..N-1 = post-children.
                SetBlocks(n, xmo, {bid.prologue, bid.xor_eax_eax, bid.epilogue});
                break;

            case ParseKind::Call:
                SetBlocks(n, xmo, {bid.call_rel32});
                break;

            case ParseKind::VarDecl:
                // Non-array locals with an initializer: store rax after call.
                if (!n->isArray && n->declaredSymbol && n->childCount > 1)
                    SetBlocks(n, xmo, {bid.mov_rsp_off8_rax});
                break;

            case ParseKind::IntLit:
                if (n->paramSlot == 1)
                    SetBlocks(n, xmo, {bid.mov_ecx_imm32});
                break;

            case ParseKind::Ident:
                if (n->paramSlot != 0 && n->declaredSymbol) {
                    if (!n->isArray) {
                        // Stack local: load into register.
                        if      (n->paramSlot == 1) SetBlocks(n, xmo, {bid.mov_rcx_rsp_off8});
                    } else {
                        // Array (string literal in .rdata): RIP-relative address.
                        if      (n->paramSlot == 2) SetBlocks(n, xmo, {bid.lea_rdx_rip32});
                    }
                }
                break;

            case ParseKind::MemberAccess:
                if (n->paramSlot == 3) {
                    // Pre-compute string length so the Emitter can patch it directly.
                    n->intValue = StringLengthOf(n);
                    SetBlocks(n, xmo, {bid.mov_r8d_imm32});
                }
                break;

            case ParseKind::AddressOf:
                if (n->paramSlot == 4 && n->childCount > 0) {
                    ParseTreeNode* operand = n->children[0]; // Ident of the local
                    Symbol*        sym     = operand->declaredSymbol;
                    if (sym) n->intValue = sym->offset;
                    SetBlocks(n, xmo, {bid.lea_r9_rsp_off8});
                }
                break;

            case ParseKind::NullLit:
                if (n->paramSlot == 5)
                    SetBlocks(n, xmo, {bid.mov_rsp32_imm0});
                break;

            default:
                break;
            }

            // Recurse into children.
            for (uint32_t i = 0; i < n->childCount; ++i)
                CodeNode(n->children[i], xmo);
        }

        // ----------------------------------------------------------------
        // Log writing
        // ----------------------------------------------------------------

        static const char* BlockName(uint16_t id)
        {
            switch (id) {
            case 0:  return "prologue";
            case 1:  return "epilogue";
            case 2:  return "lea_rcx_rel";
            case 3:  return "call_rel32";
            case 4:  return "xor_eax_eax";
            case 5:  return "mov_ecx_imm32";
            case 6:  return "mov_rcx_rsp_off8";
            case 7:  return "lea_rdx_rip32";
            case 8:  return "mov_r8d_imm32";
            case 9:  return "lea_r9_rsp_off8";
            case 10: return "mov_rsp32_imm0";
            case 11: return "mov_rsp_off8_rax";
            default: return "?";
            }
        }

        static void LogNode(std::ofstream& out, const ParseTreeNode* n,
                            const std::string& xmoName, int depth)
        {
            if (!n) return;
            if (n->codeBlockCount == 0 && n->kind == ParseKind::File) {
                // Still recurse into the tree even for nodes with no blocks.
            }

            // Print location prefix.
            std::string pos = xmoName + ":" + std::to_string(n->line) +
                              ":" + std::to_string(n->col);
            out.write(pos.data(), std::streamsize(pos.size()));
            for (size_t p = pos.size(); p < 24; ++p) out.put(' ');

            for (int i = 0; i < depth; ++i) out << "  ";
            out << ParseKindName(n->kind);

            if (n->name.str && n->name.len &&
                n->kind != ParseKind::StringLit) {
                out << "  ";
                out.write(n->name.str, n->name.len);
            }

            if (n->codeBlockCount > 0) {
                out << "  →  [";
                for (uint32_t i = 0; i < n->codeBlockCount; ++i) {
                    if (i) out << ", ";
                    uint16_t id = n->codeBlocks[i];
                    out << id << "(" << BlockName(id) << ")";
                }
                out << "]";
                if (n->paramSlot)        out << "  slot=" << int(n->paramSlot);
                if (n->intValue && n->kind != ParseKind::Ident)
                    out << "  val=" << n->intValue;
            }
            out << '\n';

            for (uint32_t i = 0; i < n->childCount; ++i)
                LogNode(out, n->children[i], xmoName, depth + 1);
        }

        static void WriteCoderLog(const Xmo& xmo)
        {
            fs::path src(xmo.name);
            fs::path logPath = src.parent_path() /
                               (src.stem().string() + ".coder.txt");
            std::ofstream out(logPath);
            if (!out) return;
            LogNode(out, xmo.parseTree, xmo.name, 0);
        }

    } // namespace

    // ----------------------------------------------------------------
    // Public entry point
    // ----------------------------------------------------------------

    void Coder::Code(Xmo& xmo, const CompileJob& job)
    {
        CodeNode(xmo.parseTree, xmo);

        if (job.CoderLog)
            WriteCoderLog(xmo);
    }

} // namespace xmc
