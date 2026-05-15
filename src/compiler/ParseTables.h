// compiler/ParseTables.h
//
// Character-class table and NestState table for the unified top-down parser.
// Everything here is constexpr / header-only; Parser.cpp is the only consumer.
//
// Character classes (CharClass enum) partition the 256-byte input space into
// ~25 logical groups. The NestState table has one entry per parser state; each
// entry maps CharClass → next state (or a special action like SN_SELF /
// SN_DISPATCH / SN_ERROR / SN_ACCEPT).
//
// The state machine covers extern declarations in full under the new syntax:
//
//   extern [rettype[:retreg]] [qualifier:]funcname(type[:reg] name, ...) ;
//
// Return spec comes BEFORE the function name (no trailing "->"). Parameters
// use C-style type-before-name order with no "->" arrows.
//
// Disambiguation for the first identifier after "extern":
//   - Leading "*"             → definitely return type (pointer)
//   - IDENT "("               → IDENT is funcname, no return type
//   - IDENT " " "("           → IDENT is funcname, no return type
//   - IDENT " " IDENT         → first IDENT is return type, second is funcname
//   - IDENT ":" IDENT "("     → IDENT1 is qualifier, IDENT2 is funcname
//   - IDENT ":" IDENT " "     → IDENT1 is return type, IDENT2 is return reg
//
// DispatchIdent() does the semantic work when SN_DISPATCH fires; it may
// consume additional characters (e.g. a ":" separator) via ctx.advance() so
// the next state sees a clean stream.
//
// Additional grammar regions (functions, blocks, expressions) will be added
// state-by-state as tests require them.
//
#pragma once

#include <array>
#include <cstdint>

namespace xmc
{

// ---------------------------------------------------------------------------
// CharClass
// ---------------------------------------------------------------------------
enum CharClass : uint8_t {
    CC_OTHER   = 0,   // unrecognised — error in most states
    CC_SPACE   = 1,   // ' '  '\t'  '\r'  '\n'
    CC_LETTER  = 2,   // [a-zA-Z_]
    CC_DIGIT   = 3,   // [0-9]
    CC_SLASH   = 4,   // /
    CC_DASH    = 5,   // -
    CC_GT      = 6,   // >
    CC_STAR    = 7,   // *
    CC_LPAREN  = 8,   // (
    CC_RPAREN  = 9,   // )
    CC_LBRACE  = 10,  // {
    CC_RBRACE  = 11,  // }
    CC_LBRACK  = 12,  // [
    CC_RBRACK  = 13,  // ]
    CC_COLON   = 14,  // :
    CC_SEMI    = 15,  // ;
    CC_COMMA   = 16,  // ,
    CC_EQUALS  = 17,  // =
    CC_AMP     = 18,  // &
    CC_DQUOTE  = 19,  // "
    CC_SQUOTE  = 20,  // '
    CC_AT      = 21,  // @
    CC_DOLLAR  = 22,  // $
    CC_DOT     = 23,  // .
    CC_NULL    = 24,  // '\0'  (end of source)
    NUM_CC     = 25,
};

// ---------------------------------------------------------------------------
// Special values for NestState::next[]
//
// Real state indices occupy [0, 0xFFFB].  Values above that are actions.
// ---------------------------------------------------------------------------
constexpr uint16_t SN_ERROR    = 0xFFFF; // unexpected char → emit diagnostic
constexpr uint16_t SN_SELF     = 0xFFFE; // accumulate char, stay in same state
constexpr uint16_t SN_DISPATCH = 0xFFFD; // identifier token done → DispatchIdent()
constexpr uint16_t SN_ACCEPT   = 0xFFFC; // end of source, parse successful

// ---------------------------------------------------------------------------
// NestState flags
// ---------------------------------------------------------------------------
constexpr uint8_t NS_SKIP_WS       = 0x01; // transparent whitespace skip
constexpr uint8_t NS_SKIP_COMMENTS = 0x02; // transparent // … newline skip

// ---------------------------------------------------------------------------
// NestState
// ---------------------------------------------------------------------------
struct NestState {
    uint16_t    next[NUM_CC]; // transition per char class
    uint8_t     flags;        // NS_SKIP_WS | NS_SKIP_COMMENTS
    uint8_t     _pad;
    const char* expected;     // used verbatim: "expected <expected>, got 'x'"
};

// ---------------------------------------------------------------------------
// StateIdx  — parser state indices, matching kStates[] order below
// ---------------------------------------------------------------------------
enum StateIdx : uint16_t {

    // ---- Top-level ----------------------------------------------------------
    S_FILE                   = 0,
    S_FILE_IDENT             = 1,

    // ---- Extern — return-spec / name disambiguation -------------------------
    //
    // After "extern", the first non-WS token is ambiguous.  The states below
    // resolve it via DispatchIdent as described in the file header.

    S_EXTERN_AFTER           = 2,  // skip WS; '*' (rettype ptr) or letter
    S_EXTERN_RETTYPE_STAR    = 3,  // accumulate '*'s; letter → RETTYPE_NAME
    S_EXTERN_RETTYPE_NAME    = 4,  // scan rettype ident; dispatch ':' or ' '
    S_EXTERN_RETREG_WAIT     = 5,  // skip WS; letter → RETREG_NAME
    S_EXTERN_RETREG_NAME     = 6,  // scan retreg ident; dispatch ' '

    S_EXTERN_FIRST_IDENT     = 7,  // scan first ambiguous ident; dispatch ':' '(' ' '
    S_EXTERN_SECOND_IDENT_PRE= 8,  // skip WS; letter → SECOND_IDENT
    S_EXTERN_SECOND_IDENT    = 9,  // scan IDENT2 of "IDENT1:IDENT2"; dispatch '(' ' '

    S_EXTERN_AFTER_FIRST_SPACE=10, // skip WS after IDENT1 space; letter or '('
    S_EXTERN_QNAME           = 11, // skip WS before [qualifier:]funcname
    S_EXTERN_QNAME_IDENT     = 12, // scan qualifier-or-funcname; dispatch ':' '(' ' '
    S_EXTERN_FNAME_WAIT      = 13, // skip WS; letter → FNAME_IDENT
    S_EXTERN_FNAME_IDENT     = 14, // scan funcname (known); dispatch '(' ' '
    S_EXTERN_LPAREN          = 15, // skip WS; '(' → EXTERN_PARAMS

    // ---- Extern parameters — type[:reg] name --------------------------------
    S_EXTERN_PARAMS          = 16, // inside param list; '*' letter or ')'
    S_PARAM_TYPE_STAR        = 17, // accumulate '*'s; letter → PARAM_TYPE_NAME
    S_PARAM_TYPE_NAME        = 18, // scan param type ident; dispatch ':' ' '
    S_PARAM_REG_WAIT         = 19, // skip WS; letter → PARAM_REG_NAME
    S_PARAM_REG_NAME         = 20, // scan param reg ident; dispatch ' '
    S_PARAM_NAME_WAIT        = 21, // skip WS; letter → PARAM_NAME
    S_PARAM_NAME             = 22, // scan param name; dispatch ',' ')' ' '
    S_PARAM_AFTER_NAME       = 23, // skip WS after param name; ',' or ')'

    // ---- After param list ---------------------------------------------------
    S_AFTER_PARAMS           = 24, // skip WS; ';' → FILE (ExternDecl done) or '{' → BLOCK

    // ---- Function declaration -----------------------------------------------
    //
    // Top-level function declarations:  rettype funcname(params) { body }
    // The return type name is scanned in S_FILE_IDENT (same as any top-level
    // identifier) and dispatched retroactively in DispatchIdent.

    S_FUNC_NAME_WAIT         = 25, // skip WS; letter → FUNC_NAME
    S_FUNC_NAME              = 26, // scan funcname; dispatch '(' ' '
    S_FUNC_LPAREN            = 27, // skip WS; '(' → S_EXTERN_PARAMS (param chain reused)

    // ---- Block body (brace-depth scanner) -----------------------------------
    //
    // Minimal scanner: all characters are SN_SELF. OnEnterState tracks brace
    // depth and transitions back to S_FILE when the matching '}' is consumed.
    // Full statement parsing will be added incrementally.

    S_BLOCK                  = 28, // brace-depth scanner; CC_NULL → error

    NUM_STATES               = 29,
};

// ---------------------------------------------------------------------------
// kCharClass[256]  — constexpr byte-to-CharClass mapping
// ---------------------------------------------------------------------------
inline constexpr auto kCharClass = []() constexpr
{
    std::array<uint8_t, 256> t{};
    // Default is CC_OTHER = 0.

    t['\0'] = CC_NULL;
    t[' ']  = CC_SPACE;  t['\t'] = CC_SPACE;
    t['\r'] = CC_SPACE;  t['\n'] = CC_SPACE;

    for (int c = 'a'; c <= 'z'; ++c) t[uint8_t(c)] = CC_LETTER;
    for (int c = 'A'; c <= 'Z'; ++c) t[uint8_t(c)] = CC_LETTER;
    t['_'] = CC_LETTER;

    for (int c = '0'; c <= '9'; ++c) t[uint8_t(c)] = CC_DIGIT;

    t['/']  = CC_SLASH;  t['-']  = CC_DASH;   t['>'] = CC_GT;
    t['*']  = CC_STAR;   t['(']  = CC_LPAREN; t[')'] = CC_RPAREN;
    t['{']  = CC_LBRACE; t['}']  = CC_RBRACE;
    t['[']  = CC_LBRACK; t[']']  = CC_RBRACK;
    t[':']  = CC_COLON;  t[';']  = CC_SEMI;   t[','] = CC_COMMA;
    t['=']  = CC_EQUALS; t['&']  = CC_AMP;
    t['"']  = CC_DQUOTE; t['\''] = CC_SQUOTE;
    t['@']  = CC_AT;     t['$']  = CC_DOLLAR; t['.'] = CC_DOT;

    return t;
}();

// ---------------------------------------------------------------------------
// kStates[NUM_STATES]  — the NestState table
//
// MkState: helper that returns a NestState with all next[] = SN_ERROR.
// Individual entries are then set by the lambda body before returning.
// ---------------------------------------------------------------------------
inline constexpr NestState MkState(uint8_t flags, const char* expected)
{
    NestState s{};
    for (auto& n : s.next) n = SN_ERROR;
    s.flags    = flags;
    s._pad     = 0;
    s.expected = expected;
    return s;
}

inline constexpr NestState kStates[NUM_STATES] = {

    // -----------------------------------------------------------------------
    // S_FILE (0)
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS | NS_SKIP_COMMENTS,
                         "top-level declaration (extern, type, or identifier)");
        s.next[CC_LETTER] = S_FILE_IDENT;
        s.next[CC_NULL]   = SN_ACCEPT;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_FILE_IDENT (1) — scanning top-level keyword / identifier
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "identifier character or end of keyword");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_SPACE]  = SN_DISPATCH;
        s.next[CC_LPAREN] = SN_DISPATCH;
        s.next[CC_LBRACE] = SN_DISPATCH;
        s.next[CC_COLON]  = SN_DISPATCH;
        s.next[CC_DASH]   = SN_DISPATCH;
        s.next[CC_SEMI]   = SN_DISPATCH;
        s.next[CC_NULL]   = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_AFTER (2) — after "extern"; skip WS, await '*' or first ident
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS | NS_SKIP_COMMENTS,
                         "return type, qualifier, or function name after 'extern'");
        s.next[CC_STAR]   = S_EXTERN_RETTYPE_STAR;  // unambiguously a return type
        s.next[CC_LETTER] = S_EXTERN_FIRST_IDENT;   // ambiguous until dispatch
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_RETTYPE_STAR (3) — accumulate '*' pointer stars; letter → name
    //
    // SN_SELF on '*' is handled by OnEnterState to increment pointerDepth.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "return type name or additional '*'");
        s.next[CC_STAR]   = SN_SELF;
        s.next[CC_LETTER] = S_EXTERN_RETTYPE_NAME;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_RETTYPE_NAME (4) — scanning return type identifier (unambiguous)
    //
    // Dispatch on ':' → retreg follows (DispatchIdent consumes the ':').
    // Dispatch on ' ' → no retreg; function name follows.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "return type identifier, ':' (register), or space");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_COLON]  = SN_DISPATCH;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_RETREG_WAIT (5) — skip WS before return register name
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS, "return register name");
        s.next[CC_LETTER] = S_EXTERN_RETREG_NAME;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_RETREG_NAME (6) — scanning return register identifier
    //
    // Dispatch on ' ' → retreg done; function qualified-name follows.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "return register identifier or space before function name");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_FIRST_IDENT (7) — scanning first identifier after "extern"
    //
    // Dispatch on '(' → this ident is the function name (no return type, no qualifier).
    // Dispatch on ' ' → ambiguous: could be rettype or funcname; check what follows.
    // Dispatch on ':' → ambiguous: could be "rettype:retreg" or "qualifier:funcname";
    //                   DispatchIdent saves token as pending and consumes ':'.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "identifier, ':', '(', or space");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_COLON]  = SN_DISPATCH;
        s.next[CC_LPAREN] = SN_DISPATCH;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_SECOND_IDENT_PRE (8) — skip WS; await second identifier
    // (Reached after DispatchIdent consumed the ':' following IDENT1.)
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS, "second identifier after ':'");
        s.next[CC_LETTER] = S_EXTERN_SECOND_IDENT;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_SECOND_IDENT (9) — scanning IDENT2 in "IDENT1:IDENT2"
    //
    // Dispatch on '(' → IDENT1=qualifier, IDENT2=funcname, open params.
    // Dispatch on ' ' → IDENT1=rettype, IDENT2=retreg, qname follows.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "identifier, '(' (qualifier:funcname), or space (rettype:retreg)");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_LPAREN] = SN_DISPATCH;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_AFTER_FIRST_SPACE (10) — skip WS; resolve the IDENT1-space ambiguity
    //
    // '('    → IDENT1 was the function name (void return); consume '(' open params.
    // letter → IDENT1 was the return type; scan qualified function name.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS,
                         "function name (after return type) or '(' (function has no return type)");
        s.next[CC_LETTER] = S_EXTERN_QNAME_IDENT;  // IDENT1 was rettype (no retreg)
        s.next[CC_LPAREN] = S_EXTERN_PARAMS;        // IDENT1 was funcname; '(' consumed here
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_QNAME (11) — skip WS before [qualifier:]funcname
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS | NS_SKIP_COMMENTS,
                         "function name or qualifier");
        s.next[CC_LETTER] = S_EXTERN_QNAME_IDENT;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_QNAME_IDENT (12) — scanning qualifier-or-funcname identifier
    //
    // Dispatch on ':' → this ident is a qualifier; DispatchIdent consumes ':'.
    // Dispatch on '(' → this ident is the function name; '(' follows immediately.
    // Dispatch on ' ' → this ident is the function name; space before '('.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "function name identifier, ':', '(', or space");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_COLON]  = SN_DISPATCH;
        s.next[CC_LPAREN] = SN_DISPATCH;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_FNAME_WAIT (13) — skip WS before function name
    // (Reached after qualifier ":" was consumed by DispatchIdent.)
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS, "function name");
        s.next[CC_LETTER] = S_EXTERN_FNAME_IDENT;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_FNAME_IDENT (14) — scanning function name identifier (unambiguous)
    //
    // Dispatch on '(' → funcname done; '(' follows immediately.
    // Dispatch on ' ' → funcname done; space before '('.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "function name identifier, '(' or space");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_LPAREN] = SN_DISPATCH;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_LPAREN (15) — skip WS; '(' opens param list
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS | NS_SKIP_COMMENTS,
                         "'(' to begin parameter list");
        s.next[CC_LPAREN] = S_EXTERN_PARAMS;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_EXTERN_PARAMS (16) — inside param list; await param type or ')'
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS | NS_SKIP_COMMENTS,
                         "parameter type or ')'");
        s.next[CC_STAR]   = S_PARAM_TYPE_STAR;
        s.next[CC_LETTER] = S_PARAM_TYPE_NAME;
        s.next[CC_RPAREN] = S_AFTER_PARAMS;     // empty or end of last param
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_PARAM_TYPE_STAR (17) — accumulate '*' stars for param type
    //
    // SN_SELF on '*' increments the Type node's pointerDepth.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "type name or additional '*'");
        s.next[CC_STAR]   = SN_SELF;
        s.next[CC_LETTER] = S_PARAM_TYPE_NAME;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_PARAM_TYPE_NAME (18) — scanning param type identifier
    //
    // Dispatch on ':' → register follows; DispatchIdent pops Type and consumes ':'.
    // Dispatch on ' ' → no register; param name follows; DispatchIdent pops Type.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "type identifier, ':' (register), or space before parameter name");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_COLON]  = SN_DISPATCH;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_PARAM_REG_WAIT (19) — skip WS before param register name
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS, "register name");
        s.next[CC_LETTER] = S_PARAM_REG_NAME;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_PARAM_REG_NAME (20) — scanning param register identifier
    //
    // Dispatch on ' ' → reg done; param name follows.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "register name or space before parameter name");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_PARAM_NAME_WAIT (21) — skip WS before parameter name identifier
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS, "parameter name");
        s.next[CC_LETTER] = S_PARAM_NAME;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_PARAM_NAME (22) — scanning parameter name identifier
    //
    // Dispatch on ',' → param done, next param follows; DispatchIdent pops Param.
    // Dispatch on ')' → param done, list closes; DispatchIdent pops Param.
    // Dispatch on ' ' → param name done, ','/')' follows after WS.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "parameter name, ',', ')', or space");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_COMMA]  = SN_DISPATCH;
        s.next[CC_RPAREN] = SN_DISPATCH;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_PARAM_AFTER_NAME (23) — skip WS after param name; await ',' or ')'
    //
    // ',' → next param; OnLeaveState handles nothing (Param already popped).
    // ')' → end of param list; OnLeaveState pops ParamList.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS | NS_SKIP_COMMENTS,
                         "',' (next parameter) or ')' (end of parameter list)");
        s.next[CC_COMMA]  = S_EXTERN_PARAMS;
        s.next[CC_RPAREN] = S_AFTER_PARAMS;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_AFTER_PARAMS (24) — after ')'; skip WS
    //
    // ';' → ExternDecl done → S_FILE
    // '{' → function body block → S_BLOCK
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS | NS_SKIP_COMMENTS,
                         "';' (extern) or '{' (function body)");
        s.next[CC_SEMI]   = S_FILE;
        s.next[CC_LBRACE] = S_BLOCK;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_FUNC_NAME_WAIT (25) — skip WS; await function name identifier
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS, "function name");
        s.next[CC_LETTER] = S_FUNC_NAME;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_FUNC_NAME (26) — scanning function name identifier
    //
    // Dispatch on '(' → funcname done; '(' follows immediately.
    // Dispatch on ' ' → funcname done; space before '('.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "function name, '(' or space");
        s.next[CC_LETTER] = SN_SELF;
        s.next[CC_DIGIT]  = SN_SELF;
        s.next[CC_LPAREN] = SN_DISPATCH;
        s.next[CC_SPACE]  = SN_DISPATCH;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_FUNC_LPAREN (27) — skip WS; '(' opens parameter list
    //
    // Reuses the extern parameter chain (S_EXTERN_PARAMS through
    // S_PARAM_AFTER_NAME) since the syntax is identical.
    // After ')' the chain arrives at S_AFTER_PARAMS, which then accepts '{'.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(NS_SKIP_WS | NS_SKIP_COMMENTS,
                         "'(' to begin parameter list");
        s.next[CC_LPAREN] = S_EXTERN_PARAMS;
        return s;
    }(),

    // -----------------------------------------------------------------------
    // S_BLOCK (28) — brace-depth scanner for function bodies
    //
    // All character classes map to SN_SELF; OnEnterState tracks brace depth
    // and switches ctx.state to S_FILE when the matching '}' is consumed.
    // CC_NULL stays SN_ERROR — EOF inside a block is an error.
    // -----------------------------------------------------------------------
    []() constexpr {
        auto s = MkState(0, "block body (unexpected end of file inside block)");
        for (auto& n : s.next) n = SN_SELF;
        s.next[CC_NULL] = SN_ERROR;
        return s;
    }(),
};

} // namespace xmc
