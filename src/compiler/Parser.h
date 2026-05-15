// compiler/Parser.h
//
// Parse-tree node definitions and the unified per-file Parser entry point.
//
// The Parser is a top-down, character-driven state machine. It does not use
// a separate lexer thread or a pipeline queue. It reads source bytes directly
// and builds the parse tree in xmo.arena as it descends.
//
// When the Parser finishes a noun node it submits a Morpher task to the
// shared thread pool. The parser and morpher therefore overlap in time, but
// the parser never blocks on the morpher. See Morpher.h for the handoff
// contract.
//
// Parse-tree nodes (ParseTreeNode) are the same structure used by all later
// stages (Reviewer, Coder, Emitter). New kinds are appended; never reorder.
//
#pragma once

#include <cstdint>
#include <string_view>

#include "SymbolTable.h"
#include "Xmo.h"
#include "../Compiler.h"
#include "../tool/BS_thread_pool.hpp"

namespace xmc
{
    // =========================================================================
    // ParseKind
    //
    // Tag identifying the construct each ParseTreeNode represents. Three shape
    // families:
    //
    //   nouns        — leaves; hold values (literals, identifiers).
    //   verbs        — produce values (calls, member access, operators, etc.).
    //   conjunctions — structure flow (functions, blocks, if/while/return, …).
    //
    // New kinds are appended; do not reorder existing values.
    // =========================================================================
    enum class ParseKind : uint16_t
    {
        Unknown = 0,

        // ----- Top-level / structural ----------------------------------------
        File,           // root of an .xm; children = top-level decls
        ExternDecl,     // extern qual:name(params) -> retType : retReg ;
        FuncDecl,       // retType name(params) block
        ParamList,      // children = Param nodes
        Param,          // name -> type [: register]
        ReturnSpec,     // -> type [: register]   (a Param without a name)
        Type,           // type expression: u8, *u8, **u8, u8[], etc.
        Block,          // { stmt* }
        VarDecl,        // type name [= expr] ;
        ExprStmt,       // expr ;
        Return,         // return [expr] ;
        ArgList,        // children = expression nodes

        // ----- Verbs ---------------------------------------------------------
        Assign,         // a = b
        Call,           // child[0] = callee, then ArgList
        MemberAccess,   // obj.member
        Subscript,      // a[i]
        AddressOf,      // @x or &x
        Deref,          // $x
        Negate,         // -x
        UnaryPlus,      // +x
        Not,            // !x
        BitNot,         // ~x
        BinOp,          // child[0] op child[1]; opToken identifies operator

        // ----- Nouns ---------------------------------------------------------
        Ident,          // identifier reference
        IntLit,         // integer literal
        FloatLit,       // floating-point literal
        StringLit,      // "…"  — span via srcStart/srcLen
        CharLit,        // '.'  — charValue in intValue
        BoolLit,        // true | false
        NullLit,        // null

        // ----- Placeholder ---------------------------------------------------
        Stub,           // forward-reference placeholder (Under_The_Hood §2d)
    };

    // Human-readable name for parser-log output.
    const char* ParseKindName(ParseKind k);

    // =========================================================================
    // ParseTreeNode
    //
    // One node in the parse tree. Allocated from Xmo::arena; never freed
    // individually. children is an arena-allocated array of childCount
    // pointers. parent points up the tree.
    //
    // Fields that the Morpher writes (baseType, minrmask, maxrmask,
    // declaredSymbol) must be treated as owned by the Morpher once the node
    // has been handed off via the thread pool. The Parser must not touch them
    // after submission.
    // =========================================================================
    struct ParseTreeNode
    {
        ParseKind       kind     = ParseKind::Unknown;
        uint16_t        opToken  = 0;       // for BinOp / Assign: which operator

        ParseTreeNode*  parent      = nullptr;
        ParseTreeNode** children    = nullptr;
        uint32_t        childCount  = 0;

        // Morpher-owned fields. Parser leaves these at zero/nullptr.
        uint32_t        baseType    = 0;
        uint8_t         minrmask    = 0;
        uint8_t         maxrmask    = 0xFF;
        Symbol*         declaredSymbol = nullptr;

        // Source location of the principal token (for diagnostics and logs).
        uint32_t        srcStart = 0;
        uint32_t        srcLen   = 0;
        uint32_t        line     = 0;
        uint32_t        col      = 0;

        // ---- Payload --------------------------------------------------------
        // Interpreted per ParseKind; see Parser.h prologue for the mapping.
        InternedString  name;           // primary identifier
        InternedString  qualifier;      // namespace prefix (extern decls)
        InternedString  regHint;        // register clause (Param / ReturnSpec)

        uint64_t        intValue   = 0; // IntLit / CharLit
        double          floatValue = 0; // FloatLit

        uint8_t         pointerDepth = 0; // Type: leading '*' count
        bool            isArray      = false; // Type: trailing "[]"

        // Coder fills these; Emitter consumes. Empty until the Coder runs.
        uint16_t*       codeBlocks     = nullptr;
        uint32_t        codeBlockCount = 0;
    };

    // =========================================================================
    // Parser
    //
    // Entry point: one call per source file, from a pool worker thread.
    //
    // The Parser maps no files itself — the caller (Compiler) maps the source
    // and passes a string_view. The Parser owns all parse-tree allocation
    // (in xmo.arena) and all child-count bookkeeping.
    //
    // When a noun node is complete the Parser submits a Morpher task to pool.
    // The pool reference is non-owning; the pool outlives the Parser call.
    //
    // On parse error the Parser sets job.ErrorOccurred, emits a diagnostic
    // (file:line:col: expected one of [...], got 'x'), and attempts to
    // recover so later files still compile.
    // =========================================================================
    class Parser
    {
    public:
        static void Parse(
            Xmo&                              xmo,
            std::string_view                  source,
            SymbolTable&                      symbols,
            BS::thread_pool<>&                pool,
            const CompileJob&                 job);
    };

} // namespace xmc
