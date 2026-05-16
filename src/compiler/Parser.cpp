// compiler/Parser.cpp
//
// Unified top-down character-driven parser.
//
// The drive loop reads source bytes directly, using kCharClass[] and kStates[]
// from ParseTables.h to decide what to do next. It builds the parse tree in
// xmo.arena as it descends, without a separate lexer thread or token queue.
//
// When a noun node is complete, Morpher::MorphNoun is submitted to the pool
// as a fire-and-forget task. After the full tree is built, Morpher::MorphTree
// runs a whole-tree semantic pass.
//
// Each ParseTreeNode's children are accumulated in a temporary std::vector
// (per frame) during parsing, then committed to the arena when the node is
// completed. This avoids O(n²) arena re-allocations while growing child lists.
//
#include "pch/pch.h"
#include "Parser.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "Morpher.h"
#include "ParseTables.h"
#include "tool/Logger.h"

namespace fs = std::filesystem;

namespace xmc
{

// =============================================================================
// ParseKindName
// =============================================================================
const char* ParseKindName(ParseKind k)
{
    switch (k) {
    case ParseKind::Unknown:      return "Unknown";
    case ParseKind::File:         return "File";
    case ParseKind::ExternDecl:   return "ExternDecl";
    case ParseKind::FuncDecl:     return "FuncDecl";
    case ParseKind::ParamList:    return "ParamList";
    case ParseKind::Param:        return "Param";
    case ParseKind::ReturnSpec:   return "ReturnSpec";
    case ParseKind::Type:         return "Type";
    case ParseKind::Block:        return "Block";
    case ParseKind::VarDecl:      return "VarDecl";
    case ParseKind::ExprStmt:     return "ExprStmt";
    case ParseKind::Return:       return "Return";
    case ParseKind::ArgList:      return "ArgList";
    case ParseKind::Assign:       return "Assign";
    case ParseKind::Call:         return "Call";
    case ParseKind::MemberAccess: return "MemberAccess";
    case ParseKind::Subscript:    return "Subscript";
    case ParseKind::AddressOf:    return "AddressOf";
    case ParseKind::Deref:        return "Deref";
    case ParseKind::Negate:       return "Negate";
    case ParseKind::UnaryPlus:    return "UnaryPlus";
    case ParseKind::Not:          return "Not";
    case ParseKind::BitNot:       return "BitNot";
    case ParseKind::BinOp:        return "BinOp";
    case ParseKind::Ident:        return "Ident";
    case ParseKind::IntLit:       return "IntLit";
    case ParseKind::FloatLit:     return "FloatLit";
    case ParseKind::StringLit:    return "StringLit";
    case ParseKind::CharLit:      return "CharLit";
    case ParseKind::BoolLit:      return "BoolLit";
    case ParseKind::NullLit:      return "NullLit";
    case ParseKind::Stub:         return "Stub";
    default:                      return "?";
    }
}

// =============================================================================
// Internal helpers
// =============================================================================
namespace
{

// -----------------------------------------------------------------------------
// Frame
//
// One entry on the parse stack. Each pushed ParseTreeNode has a parallel Frame
// that accumulates its children during parsing. On pop, children are committed
// to the arena as a flat array.
// -----------------------------------------------------------------------------
struct Frame {
    ParseTreeNode*              node;
    std::vector<ParseTreeNode*> children; // pending; committed on pop
};

// -----------------------------------------------------------------------------
// ParseCtx
//
// All mutable state for one file parse. Lives on the stack of Drive().
// -----------------------------------------------------------------------------
struct ParseCtx {
    Xmo&               xmo;
    std::string_view   source;
    SymbolTable&       symbols;
    BS::thread_pool<>& pool;
    const CompileJob&  job;

    // Source cursor
    uint32_t pos  = 0;
    uint32_t line = 1;
    uint32_t col  = 1;

    // Current token window (start of the identifier being accumulated)
    uint32_t tokenStart = 0;
    uint32_t tokenLine  = 1;
    uint32_t tokenCol   = 1;

    // Current parser state (index into kStates[])
    uint16_t state = S_FILE;

    // Scratch storage for the ambiguous first identifier after "extern".
    // Set by DispatchIdent when the role of IDENT1 cannot yet be determined
    // (the ':' or space trigger is ambiguous). Cleared once the role resolves.
    InternedString pending{};

    // Brace depth counter for the S_BLOCK scanner.
    // Starts at 1 when the opening '{' is consumed; decremented on '}';
    // reaching 0 means the block is closed and we return to S_FILE.
    uint32_t braceDepth = 0;

    // Node stack
    std::vector<Frame> stack;

    // Accessors
    ParseTreeNode* current() const
    {
        return stack.empty() ? nullptr : stack.back().node;
    }

    // Advance one byte (updating line/col).
    void advance()
    {
        if (pos < source.size()) {
            if (source[pos] == '\n') { ++line; col = 1; }
            else                     { ++col; }
            ++pos;
        }
    }

    // Current char class (CC_NULL when past end).
    CharClass cls() const
    {
        return (pos < source.size())
               ? CharClass(kCharClass[uint8_t(source[pos])])
               : CC_NULL;
    }

    // Mark the start of a token at the current position.
    void startToken()
    {
        tokenStart = pos;
        tokenLine  = line;
        tokenCol   = col;
    }

    // Text of the identifier accumulated since startToken().
    std::string_view token() const
    {
        return source.substr(tokenStart, pos - tokenStart);
    }

    // Intern the current token text.
    InternedString internToken() const
    {
        return symbols.InternString(token());
    }

    // Push a new node (doesn't add it as child yet — caller does that via
    // addChild or by being the File root).
    void pushNode(ParseTreeNode* n)
    {
        stack.push_back({ n, {} });
    }

    // Add child to the current top frame and set child->parent.
    void addChild(ParseTreeNode* child)
    {
        if (!stack.empty()) {
            child->parent = stack.back().node;
            stack.back().children.push_back(child);
        }
    }

    // Commit pending children to the arena, then pop.
    // Returns the completed node.
    ParseTreeNode* popNode()
    {
        assert(!stack.empty());
        Frame& f = stack.back();
        ParseTreeNode* n = f.node;
        if (!f.children.empty()) {
            n->childCount = uint32_t(f.children.size());
            n->children   = xmo.arena.NewArray<ParseTreeNode*>(n->childCount);
            std::memcpy(n->children, f.children.data(),
                        n->childCount * sizeof(ParseTreeNode*));
        }
        stack.pop_back();
        return n;
    }

    // Convenience: allocate and zero-initialise a ParseTreeNode from the arena.
    ParseTreeNode* allocNode(ParseKind kind)
    {
        auto* n    = xmo.arena.Construct<ParseTreeNode>();
        n->kind    = kind;
        n->line    = tokenLine;
        n->col     = tokenCol;
        n->srcStart = tokenStart;
        return n;
    }

    // Emit a syntax error and mark job failed.
    void error(const char* expected, char got)
    {
        if (got == '\0') {
            oserror << xmo.name << ":" << line << ":" << col
                    << ": expected " << expected
                    << ", got end of file\n";
        } else {
            oserror << xmo.name << ":" << line << ":" << col
                    << ": expected " << expected
                    << ", got '" << got << "'\n";
        }
        job.ErrorOccurred.store(true, std::memory_order_relaxed);
    }
};

// -----------------------------------------------------------------------------
// WriteParserLog
//
// Writes an indented tree dump to <stem>.parser.txt next to the source file.
// Called after a successful or error parse (partial tree on error).
// -----------------------------------------------------------------------------
static std::string Excerpt(std::string_view src, uint32_t start, uint32_t len)
{
    if (start >= src.size()) return {};
    uint32_t end = std::min(start + len, uint32_t(src.size()));
    return std::string(src.substr(start, end - start));
}

static void DumpNode(std::ofstream& out, const ParseTreeNode* n,
                     const std::string& fileName,
                     std::string_view   source,
                     int                depth)
{
    if (!n) return;

    // Position prefix, padded to column 26.
    std::string pos = fileName + ":" +
                      std::to_string(n->line) + ":" +
                      std::to_string(n->col);
    out << pos;
    for (size_t i = pos.size(); i < 26; ++i) out << ' ';

    // Indent.
    for (int i = 0; i < depth; ++i) out << "  ";

    out << ParseKindName(n->kind);

    // Per-kind payload.
    switch (n->kind) {
    case ParseKind::ExternDecl:
    case ParseKind::FuncDecl:
    case ParseKind::Param:
    case ParseKind::ReturnSpec:
    case ParseKind::VarDecl:
    case ParseKind::Ident:
    case ParseKind::MemberAccess:
        out << "  ";
        if (n->qualifier.str && n->qualifier.len)
            out.write(n->qualifier.str, n->qualifier.len) << ":";
        if (n->name.str && n->name.len)
            out.write(n->name.str, n->name.len);
        if ((n->kind == ParseKind::Param || n->kind == ParseKind::ReturnSpec)
            && n->regHint.str && n->regHint.len)
        {
            out << " : ";
            out.write(n->regHint.str, n->regHint.len);
        }
        break;
    case ParseKind::Type:
        out << "  ";
        for (uint8_t i = 0; i < n->pointerDepth; ++i) out << '*';
        if (n->name.str && n->name.len)
            out.write(n->name.str, n->name.len);
        if (n->isArray) out << "[]";
        break;
    case ParseKind::IntLit:
        out << "  " << n->intValue;
        break;
    case ParseKind::StringLit:
        out << "  \"" << Excerpt(source, n->srcStart, n->srcLen) << "\"";
        break;
    default:
        break;
    }

    out << '\n';

    for (uint32_t i = 0; i < n->childCount; ++i)
        DumpNode(out, n->children[i], fileName, source, depth + 1);
}

static void WriteParserLog(ParseTreeNode*      root,
                           const std::vector<Frame>& partialStack,
                           const std::string&  fileName,
                           std::string_view    source)
{
    fs::path logPath = fs::path(fileName).parent_path() /
                       (fs::path(fileName).stem().string() + ".parser.txt");
    std::ofstream out(logPath);
    if (!out) return;

    if (root) {
        DumpNode(out, root, fileName, source, 0);
    } else {
        // Partial parse: dump whatever nodes are on the stack.
        for (const Frame& f : partialStack)
            if (f.node) DumpNode(out, f.node, fileName, source, 0);
    }
}

// =============================================================================
// Statement/expression helpers
// =============================================================================

// Parse a decimal or 0x-hex integer token text into a uint64_t.
static uint64_t ParseIntTok(std::string_view tok)
{
    if (tok.size() >= 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        uint64_t v = 0;
        for (char c : tok.substr(2)) {
            v *= 16;
            if (c >= '0' && c <= '9')      v += uint64_t(c - '0');
            else if (c >= 'a' && c <= 'f') v += uint64_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v += uint64_t(c - 'A' + 10);
        }
        return v;
    }
    uint64_t v = 0;
    for (char c : tok) v = v * 10 + uint64_t(c - '0');
    return v;
}

// Allocate an Ident, NullLit, or BoolLit node for `tok`.
static ParseTreeNode* MakeIdentOrLit(ParseCtx& ctx, std::string_view tok)
{
    if (tok == "null") {
        return ctx.allocNode(ParseKind::NullLit);
    }
    if (tok == "true") {
        auto* n = ctx.allocNode(ParseKind::BoolLit);
        n->intValue = 1;
        return n;
    }
    if (tok == "false") {
        auto* n = ctx.allocNode(ParseKind::BoolLit);
        n->intValue = 0;
        return n;
    }
    auto* n = ctx.allocNode(ParseKind::Ident);
    n->name = ctx.symbols.InternString(tok);
    return n;
}

// =============================================================================
// DispatchIdent
//
// Called when an identifier token is complete (the SN_DISPATCH action fires).
// `ctx.token()` is the accumulated text. `ctx.pos` points at the character
// that triggered dispatch (not yet consumed, unless we advance explicitly).
//
// Returns the new state to transition to. Sets ctx.state before returning
// only when multiple state hops happen inside this function.
// =============================================================================
// Helper used by DispatchIdent to build a complete ReturnSpec+Type pair
// and attach it directly to the ExternDecl without going through the stack.
// Used for the "retroactive" cases where we discover the return type only
// after seeing the identifier that follows it.
static void AttachReturnSpec(ParseCtx&      ctx,
                             InternedString typeName,
                             uint8_t        pointerDepth,
                             InternedString regHint)
{
    auto* type = ctx.xmo.arena.Construct<ParseTreeNode>();
    type->kind         = ParseKind::Type;
    type->name         = typeName;
    type->pointerDepth = pointerDepth;
    type->line         = ctx.tokenLine;
    type->col          = ctx.tokenCol;
    type->srcStart     = ctx.tokenStart;

    auto* ret = ctx.xmo.arena.Construct<ParseTreeNode>();
    ret->kind    = ParseKind::ReturnSpec;
    ret->regHint = regHint;
    ret->line    = ctx.tokenLine;
    ret->col     = ctx.tokenCol;
    ret->srcStart= ctx.tokenStart;

    // Wire type as sole child of ret.
    ret->childCount = 1;
    ret->children   = ctx.xmo.arena.NewArray<ParseTreeNode*>(1);
    ret->children[0]= type;
    type->parent    = ret;

    // Attach ret to ExternDecl (current stack top).
    ctx.addChild(ret);
}

static uint16_t DispatchIdent(ParseCtx& ctx)
{
    const uint16_t   from = ctx.state;
    std::string_view tok  = ctx.token();

    switch (from) {

    // -----------------------------------------------------------------------
    // Top-level keyword dispatch
    // -----------------------------------------------------------------------
    case S_FILE_IDENT: {
        // Ensure the File root exists.
        if (!ctx.xmo.parseTree) {
            ctx.startToken();
            auto* file        = ctx.allocNode(ParseKind::File);
            ctx.xmo.parseTree = file;
            ctx.pushNode(file);
        }

        if (tok == "extern") {
            auto* decl = ctx.allocNode(ParseKind::ExternDecl);
            ctx.addChild(decl);
            ctx.pushNode(decl);
            return S_EXTERN_AFTER;
        }

        // Any other identifier at the top level is the return type of a
        // function declaration.  Pointer return types (*rettype) and return
        // register hints (rettype:retreg) are TODO; this handles the common
        // plain-type case (e.g. "i32 main() { ... }").
        {
            auto* decl = ctx.allocNode(ParseKind::FuncDecl);
            ctx.addChild(decl);
            ctx.pushNode(decl);
            // Retroactively attach ReturnSpec+Type for the just-scanned ident.
            AttachReturnSpec(ctx, ctx.symbols.InternString(tok), /*depth=*/0, /*reg=*/{});
            return S_FUNC_NAME_WAIT;
        }
    }

    // -----------------------------------------------------------------------
    // Function name dispatch
    // -----------------------------------------------------------------------
    case S_FUNC_NAME: {
        ParseTreeNode* decl = ctx.current(); // FuncDecl
        decl->name = ctx.symbols.InternString(tok);
        // '(' or ' ': either way, S_FUNC_LPAREN skips WS then processes '('.
        return S_FUNC_LPAREN;
    }

    // -----------------------------------------------------------------------
    // First ambiguous identifier after "extern"
    //
    //   trigger '(' → funcname, no return type, no qualifier
    //   trigger ' ' → save as pending; IDENT_AFTER_FIRST_SPACE resolves later
    //   trigger ':' → save as pending; consume ':'; SECOND_IDENT resolves later
    // -----------------------------------------------------------------------
    case S_EXTERN_FIRST_IDENT: {
        ParseTreeNode* decl = ctx.current(); // ExternDecl
        char trigger = (ctx.pos < ctx.source.size()) ? ctx.source[ctx.pos] : '\0';

        if (trigger == '(') {
            // Unambiguous: this ident is the function name.
            decl->name = ctx.symbols.InternString(tok);
            return S_EXTERN_LPAREN; // '(' re-processed there → creates ParamList
        }
        if (trigger == ' ' || trigger == '\t' || trigger == '\r' || trigger == '\n') {
            // Save; check what follows after WS.
            ctx.pending = ctx.symbols.InternString(tok);
            return S_EXTERN_AFTER_FIRST_SPACE;
        }
        if (trigger == ':') {
            // Could be "rettype:retreg" or "qualifier:funcname".
            ctx.pending = ctx.symbols.InternString(tok);
            ctx.advance(); // consume ':'
            return S_EXTERN_SECOND_IDENT_PRE;
        }
        {
            char got = trigger ? trigger : '\0';
            ctx.error("':', '(', or space after extern identifier", got);
        }
        return S_FILE;
    }

    // -----------------------------------------------------------------------
    // Second identifier of "IDENT1:IDENT2" pair
    //
    //   trigger '(' → IDENT1=qualifier, IDENT2=funcname; open params
    //   trigger ' ' → IDENT1=rettype,   IDENT2=retreg;   qname follows
    // -----------------------------------------------------------------------
    case S_EXTERN_SECOND_IDENT: {
        ParseTreeNode* decl = ctx.current(); // ExternDecl
        char trigger = (ctx.pos < ctx.source.size()) ? ctx.source[ctx.pos] : '\0';

        if (trigger == '(') {
            // IDENT1 = qualifier, IDENT2 = funcname.
            decl->qualifier = ctx.pending;
            decl->name      = ctx.symbols.InternString(tok);
            ctx.pending     = {};
            return S_EXTERN_LPAREN;
        }
        {
            // IDENT1 = rettype, IDENT2 = retreg.
            AttachReturnSpec(ctx,
                /*typeName=*/  ctx.pending,
                /*depth=*/     0,
                /*regHint=*/   ctx.symbols.InternString(tok));
            ctx.pending = {};
            return S_EXTERN_QNAME;
        }
    }

    // -----------------------------------------------------------------------
    // Return type name — unambiguous (preceded by '*' or entered from
    // S_EXTERN_AFTER_FIRST_SPACE where IDENT1 was established as rettype)
    //
    //   trigger ':' → retreg follows; pop Type, consume ':'
    //   trigger ' ' → no retreg;      pop Type and ReturnSpec
    // -----------------------------------------------------------------------
    case S_EXTERN_RETTYPE_NAME: {
        char trigger = (ctx.pos < ctx.source.size()) ? ctx.source[ctx.pos] : '\0';

        // Current stack: ... ExternDecl / ReturnSpec / Type
        ParseTreeNode* type = ctx.current();
        type->name = ctx.symbols.InternString(tok);
        ctx.popNode(); // Type → committed as child of ReturnSpec

        if (trigger == ':') {
            ctx.advance(); // consume ':'
            return S_EXTERN_RETREG_WAIT;
        }
        // ' ' → no retreg; pop ReturnSpec too.
        ctx.popNode(); // ReturnSpec → committed as child of ExternDecl
        return S_EXTERN_QNAME;
    }

    // -----------------------------------------------------------------------
    // Return register name  (stack: ... ExternDecl / ReturnSpec)
    // -----------------------------------------------------------------------
    case S_EXTERN_RETREG_NAME: {
        ParseTreeNode* ret = ctx.current(); // ReturnSpec
        ret->regHint = ctx.symbols.InternString(tok);
        ctx.popNode(); // ReturnSpec → committed as child of ExternDecl
        return S_EXTERN_QNAME;
    }

    // -----------------------------------------------------------------------
    // Qualified name — qualifier or function name
    //
    //   trigger ':' → this ident is a qualifier; consume ':'
    //   trigger '(' → this ident is the function name; '(' re-processed in LPAREN
    //   trigger ' ' → this ident is the function name; LPAREN skips WS
    // -----------------------------------------------------------------------
    case S_EXTERN_QNAME_IDENT: {
        ParseTreeNode* decl = ctx.current(); // ExternDecl
        char trigger = (ctx.pos < ctx.source.size()) ? ctx.source[ctx.pos] : '\0';

        if (trigger == ':') {
            decl->qualifier = ctx.symbols.InternString(tok);
            ctx.advance(); // consume ':'
            return S_EXTERN_FNAME_WAIT;
        }
        // '(' or ' ': this ident is the function name.
        decl->name = ctx.symbols.InternString(tok);
        return S_EXTERN_LPAREN;
    }

    // -----------------------------------------------------------------------
    // Function name (unambiguous — reached after qualifier ":")
    // -----------------------------------------------------------------------
    case S_EXTERN_FNAME_IDENT: {
        ParseTreeNode* decl = ctx.current(); // ExternDecl
        decl->name = ctx.symbols.InternString(tok);
        return S_EXTERN_LPAREN;
    }

    // -----------------------------------------------------------------------
    // Parameter type name
    //
    //   trigger ':' → register follows; pop Type, consume ':'
    //   trigger ' ' → no register;      pop Type; name follows
    // -----------------------------------------------------------------------
    case S_PARAM_TYPE_NAME: {
        char trigger = (ctx.pos < ctx.source.size()) ? ctx.source[ctx.pos] : '\0';

        // Stack: ... ParamList / Param / Type
        ParseTreeNode* type = ctx.current();
        type->name = ctx.symbols.InternString(tok);
        ctx.popNode(); // Type → committed as child of Param

        if (trigger == ':') {
            ctx.advance(); // consume ':'
            return S_PARAM_REG_WAIT;
        }
        // ' ' → no register; param name follows.
        return S_PARAM_NAME_WAIT;
    }

    // -----------------------------------------------------------------------
    // Parameter register name  (stack: ... ParamList / Param)
    // -----------------------------------------------------------------------
    case S_PARAM_REG_NAME: {
        ParseTreeNode* param = ctx.current(); // Param
        param->regHint = ctx.symbols.InternString(tok);
        return S_PARAM_NAME_WAIT;
    }

    // -----------------------------------------------------------------------
    // Parameter name — completes the Param node
    //
    //   trigger ','  → pop Param; next param follows
    //   trigger ')'  → pop Param; param list closes
    //   trigger ' '  → pop Param; S_PARAM_AFTER_NAME handles ',' or ')'
    // -----------------------------------------------------------------------
    case S_PARAM_NAME: {
        ParseTreeNode* param = ctx.current(); // Param
        param->name = ctx.symbols.InternString(tok);
        ctx.popNode(); // Param → committed as child of ParamList
        return S_PARAM_AFTER_NAME;
    }

    // -----------------------------------------------------------------------
    // S_STMT_IDENT — first identifier of a block statement
    //
    //   trigger '(' → ExprStmt / Call
    //   trigger '[' → VarDecl with array type
    //   trigger ' ' → save pending; S_STMT_AFTER_SPACE resolves later
    // -----------------------------------------------------------------------
    case S_STMT_IDENT: {
        char trigger = ctx.pos < ctx.source.size() ? ctx.source[ctx.pos] : '\0';

        if (trigger == '(') {
            ctx.advance(); // consume '(' (Drive won't since this is dispatch)
            auto* es = ctx.allocNode(ParseKind::ExprStmt);
            ctx.addChild(es);
            ctx.pushNode(es);
            auto* call = ctx.allocNode(ParseKind::Call);
            ctx.addChild(call);
            ctx.pushNode(call);
            auto* callee = ctx.allocNode(ParseKind::Ident);
            callee->name = ctx.symbols.InternString(tok);
            ctx.addChild(callee);
            auto* args = ctx.allocNode(ParseKind::ArgList);
            ctx.addChild(args);
            ctx.pushNode(args);
            return S_CALL_ARGS;
        }
        if (trigger == '[') {
            // Array-type VarDecl: type name is tok, isArray = true.
            // Do NOT advance; S_VARDECL_LBRACK will consume '['.
            auto* vd = ctx.allocNode(ParseKind::VarDecl);
            ctx.addChild(vd);
            ctx.pushNode(vd);
            auto* ty = ctx.allocNode(ParseKind::Type);
            ty->name   = ctx.symbols.InternString(tok);
            ty->isArray = true;
            ctx.addChild(ty); // added to VarDecl's pending children
            return S_VARDECL_LBRACK;
        }
        if (trigger == ' ' || trigger == '\t' || trigger == '\r' || trigger == '\n') {
            ctx.pending = ctx.symbols.InternString(tok);
            return S_STMT_AFTER_SPACE;
        }
        ctx.error("'(', '[', or space after statement identifier", trigger);
        return S_BLOCK;
    }

    // -----------------------------------------------------------------------
    // S_VARDECL_NAME — variable name complete
    //
    // The VarDecl is always already on the stack (created in OnEnterState or
    // in DispatchIdent above). Just record the name and go to AFTER_NAME.
    // -----------------------------------------------------------------------
    case S_VARDECL_NAME: {
        if (ctx.current() && ctx.current()->kind == ParseKind::VarDecl)
            ctx.current()->name = ctx.symbols.InternString(tok);
        return S_VARDECL_AFTER_NAME;
    }

    // -----------------------------------------------------------------------
    // S_INIT_IDENT — identifier/keyword in the initializer expression
    //
    //   trigger '(' → init is a function call
    //   trigger ' ', ';' → init is a bare identifier or keyword literal
    // -----------------------------------------------------------------------
    case S_INIT_IDENT: {
        char trigger = ctx.pos < ctx.source.size() ? ctx.source[ctx.pos] : '\0';

        if (trigger == '(') {
            ctx.advance(); // consume '('
            auto* call = ctx.allocNode(ParseKind::Call);
            ctx.addChild(call); // adds to VarDecl's pending children
            ctx.pushNode(call);
            auto* callee = ctx.allocNode(ParseKind::Ident);
            callee->name = ctx.symbols.InternString(tok);
            ctx.addChild(callee);
            auto* args = ctx.allocNode(ParseKind::ArgList);
            ctx.addChild(args);
            ctx.pushNode(args);
            return S_CALL_ARGS;
        }
        // Bare ident or keyword as init value.
        ctx.addChild(MakeIdentOrLit(ctx, tok));
        return S_AFTER_CALL; // skip WS then ';' → pop VarDecl → S_BLOCK
    }

    // -----------------------------------------------------------------------
    // S_ARG_IDENT — argument identifier complete
    //
    //   trigger '.' → member access (obj ident already scanned)
    //   trigger ',' → arg done; more args follow
    //   trigger ')' → arg done; arg list closes
    //   trigger ' ' → arg done; skip WS for ',' or ')'
    // -----------------------------------------------------------------------
    case S_ARG_IDENT: {
        char trigger = ctx.pos < ctx.source.size() ? ctx.source[ctx.pos] : '\0';

        if (trigger == '.') {
            // Build MemberAccess: push it, add the object Ident as its child.
            auto* ma = ctx.allocNode(ParseKind::MemberAccess);
            ctx.addChild(ma);  // adds to ArgList frame
            ctx.pushNode(ma);
            auto* obj = ctx.allocNode(ParseKind::Ident);
            obj->name = ctx.symbols.InternString(tok);
            ctx.addChild(obj); // adds to MemberAccess frame
            ctx.advance();     // consume '.'
            return S_ARG_MEMBER_WAIT;
        }

        ctx.addChild(MakeIdentOrLit(ctx, tok));

        if (trigger == ',') {
            ctx.advance(); // consume ','
            return S_CALL_ARGS;
        }
        if (trigger == ')') {
            ctx.advance(); // consume ')'
            if (ctx.current() && ctx.current()->kind == ParseKind::ArgList)
                ctx.popNode();
            if (ctx.current() && ctx.current()->kind == ParseKind::Call)
                ctx.popNode();
            return S_AFTER_CALL;
        }
        // Space: arg done; skip WS then ',' or ')'.
        return S_ARG_AFTER_IDENT;
    }

    // -----------------------------------------------------------------------
    // S_ARG_MEMBER_IDENT — member name complete; pop MemberAccess
    // -----------------------------------------------------------------------
    case S_ARG_MEMBER_IDENT: {
        char trigger = ctx.pos < ctx.source.size() ? ctx.source[ctx.pos] : '\0';

        if (ctx.current() && ctx.current()->kind == ParseKind::MemberAccess) {
            ctx.current()->name = ctx.symbols.InternString(tok);
            ctx.popNode(); // MemberAccess → committed to ArgList
        }

        if (trigger == ',') {
            ctx.advance();
            return S_CALL_ARGS;
        }
        if (trigger == ')') {
            ctx.advance();
            if (ctx.current() && ctx.current()->kind == ParseKind::ArgList)
                ctx.popNode();
            if (ctx.current() && ctx.current()->kind == ParseKind::Call)
                ctx.popNode();
            return S_AFTER_CALL;
        }
        // Space: skip WS then ',' or ')'.
        return S_ARG_AFTER_IDENT;
    }

    // -----------------------------------------------------------------------
    // S_ARG_ADDR_IDENT — ident after '&'; pop AddressOf
    // -----------------------------------------------------------------------
    case S_ARG_ADDR_IDENT: {
        char trigger = ctx.pos < ctx.source.size() ? ctx.source[ctx.pos] : '\0';

        auto* operand = ctx.allocNode(ParseKind::Ident);
        operand->name = ctx.symbols.InternString(tok);
        ctx.addChild(operand);
        if (ctx.current() && ctx.current()->kind == ParseKind::AddressOf)
            ctx.popNode(); // AddressOf → committed to ArgList or VarDecl

        if (trigger == ',') {
            ctx.advance();
            return S_CALL_ARGS;
        }
        if (trigger == ')') {
            ctx.advance();
            if (ctx.current() && ctx.current()->kind == ParseKind::ArgList)
                ctx.popNode();
            if (ctx.current() && ctx.current()->kind == ParseKind::Call)
                ctx.popNode();
            return S_AFTER_CALL;
        }
        // Space or ';': S_AFTER_CALL handles the rest.
        return S_AFTER_CALL;
    }

    // -----------------------------------------------------------------------
    // S_ARG_NUMLIT — numeric literal complete
    // -----------------------------------------------------------------------
    case S_ARG_NUMLIT: {
        char trigger = ctx.pos < ctx.source.size() ? ctx.source[ctx.pos] : '\0';

        auto* lit = ctx.allocNode(ParseKind::IntLit);
        lit->intValue = ParseIntTok(tok);
        ctx.addChild(lit);

        if (trigger == ',') {
            ctx.advance();
            return S_CALL_ARGS;
        }
        if (trigger == ')') {
            ctx.advance();
            if (ctx.current() && ctx.current()->kind == ParseKind::ArgList)
                ctx.popNode();
            if (ctx.current() && ctx.current()->kind == ParseKind::Call)
                ctx.popNode();
            return S_AFTER_CALL;
        }
        // Space or ';': S_AFTER_CALL handles the rest.
        return S_AFTER_CALL;
    }

    default:
        break;
    }

    {
        char got = ctx.pos < ctx.source.size() ? ctx.source[ctx.pos] : '\0';
        ctx.error(kStates[from].expected, got);
    }
    return from; // stay put (error recovery)
}

// =============================================================================
// OnEnterState
//
// Fired immediately after the state changes (before advancing past the
// triggering character). Handles node creation, token marking, and the
// SN_SELF pointer-depth side effect.
//
// `fromState`: state we just left.
// `toState`:   state we just entered (== ctx.state at call time).
// `cc`:        char class that triggered the transition.
// =============================================================================
static void OnEnterState(uint16_t fromState, uint16_t toState,
                         CharClass cc, ParseCtx& ctx)
{
    // SN_SELF self-loops: pointer-depth side effect and block brace tracking.
    if (toState == fromState) {
        if (fromState == S_PARAM_TYPE_STAR || fromState == S_EXTERN_RETTYPE_STAR) {
            if (cc == CC_STAR && ctx.current() &&
                ctx.current()->kind == ParseKind::Type)
            {
                ++ctx.current()->pointerDepth;
            }
        }
        if (fromState == S_BLOCK) {
            if (cc == CC_LBRACE) {
                ++ctx.braceDepth;
            } else if (cc == CC_RBRACE) {
                if (ctx.braceDepth > 0) --ctx.braceDepth;
                if (ctx.braceDepth == 0) {
                    // Matching '}' found — close the Block and FuncDecl.
                    if (ctx.current() && ctx.current()->kind == ParseKind::Block)
                        ctx.popNode();
                    if (ctx.current() && ctx.current()->kind == ParseKind::FuncDecl)
                        ctx.popNode();
                    // Switch back to file scope; Drive will advance past '}'
                    // and then read kStates[S_FILE] on the next iteration.
                    ctx.state = S_FILE;
                }
            }
        }
        return;
    }

    switch (toState) {

    // ------------------------------------------------------------------
    // Top-level identifier scan.
    // ------------------------------------------------------------------
    case S_FILE_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // First ambiguous identifier scan after "extern".
    // ------------------------------------------------------------------
    case S_EXTERN_FIRST_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Return type — unambiguous because '*' preceded it.
    // Create ReturnSpec + Type and push both.
    // ------------------------------------------------------------------
    case S_EXTERN_RETTYPE_STAR:
        if (fromState == S_EXTERN_AFTER) {
            auto* ret      = ctx.allocNode(ParseKind::ReturnSpec);
            ret->srcStart  = ctx.pos;
            ctx.addChild(ret);
            ctx.pushNode(ret);
            auto* type         = ctx.allocNode(ParseKind::Type);
            type->srcStart     = ctx.pos;
            type->pointerDepth = 1; // this first '*'
            ctx.addChild(type);
            ctx.pushNode(type);
        }
        // else SN_SELF: pointerDepth already incremented above.
        break;

    // ------------------------------------------------------------------
    // Return type name scan (after '*'s or after S_EXTERN_AFTER_FIRST_SPACE
    // where pending was established as rettype with no pointer depth).
    // ------------------------------------------------------------------
    case S_EXTERN_RETTYPE_NAME:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Return register name scan.
    // ------------------------------------------------------------------
    case S_EXTERN_RETREG_NAME:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Second identifier in "IDENT1:IDENT2" (after consumed ':').
    // ------------------------------------------------------------------
    case S_EXTERN_SECOND_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Qualified-name identifier scan (qualifier or funcname).
    //
    // When entered from S_EXTERN_AFTER_FIRST_SPACE via CC_LETTER, the
    // pending ident is the return type (no pointer depth, no retreg).
    // Build and attach the ReturnSpec retroactively before starting the
    // token scan for the function's qualified name.
    // ------------------------------------------------------------------
    case S_EXTERN_QNAME_IDENT:
        if (fromState == S_EXTERN_AFTER_FIRST_SPACE) {
            // pending holds the return type name.
            AttachReturnSpec(ctx, ctx.pending, /*depth=*/0, /*regHint=*/{});
            ctx.pending = {};
        }
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Function name scan (unambiguous — after qualifier ":").
    // ------------------------------------------------------------------
    case S_EXTERN_FNAME_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Open param list.
    //
    // Entered from S_EXTERN_LPAREN on '(' (normal path) or from
    // S_EXTERN_AFTER_FIRST_SPACE on '(' (IDENT1 was funcname, no rettype).
    // ------------------------------------------------------------------
    case S_EXTERN_PARAMS:
        if (fromState == S_EXTERN_LPAREN      ||
            fromState == S_EXTERN_AFTER_FIRST_SPACE ||
            fromState == S_FUNC_LPAREN)
        {
            if (fromState == S_EXTERN_AFTER_FIRST_SPACE) {
                // IDENT1 (pending) was the function name.
                ctx.current()->name = ctx.pending; // ExternDecl.name
                ctx.pending = {};
            }
            auto* pl     = ctx.allocNode(ParseKind::ParamList);
            pl->line     = ctx.line;
            pl->col      = ctx.col;
            pl->srcStart = ctx.pos;
            ctx.addChild(pl);
            ctx.pushNode(pl);
        }
        // Re-entering after ',' from S_PARAM_AFTER_NAME: Param already popped.
        break;

    // ------------------------------------------------------------------
    // Param type scan — no leading '*'.
    // Create Param + Type (depth 0) and push both.
    // ------------------------------------------------------------------
    case S_PARAM_TYPE_NAME:
        if (fromState == S_EXTERN_PARAMS) {
            auto* param     = ctx.allocNode(ParseKind::Param);
            param->srcStart = ctx.pos;
            ctx.addChild(param);
            ctx.pushNode(param);
            auto* type      = ctx.allocNode(ParseKind::Type);
            type->srcStart  = ctx.pos;
            ctx.addChild(type);
            ctx.pushNode(type);
        }
        // from S_PARAM_TYPE_STAR: Param + Type already on stack; just start token.
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Param type scan — with leading '*'.
    // Create Param + Type (depth 1) and push both.
    // ------------------------------------------------------------------
    case S_PARAM_TYPE_STAR:
        if (fromState == S_EXTERN_PARAMS) {
            auto* param         = ctx.allocNode(ParseKind::Param);
            param->srcStart     = ctx.pos;
            ctx.addChild(param);
            ctx.pushNode(param);
            auto* type          = ctx.allocNode(ParseKind::Type);
            type->srcStart      = ctx.pos;
            type->pointerDepth  = 1;
            ctx.addChild(type);
            ctx.pushNode(type);
        }
        // else SN_SELF increments pointerDepth (handled at top of function).
        break;

    // ------------------------------------------------------------------
    // Param register name scan.
    // ------------------------------------------------------------------
    case S_PARAM_REG_NAME:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Param name scan.
    // ------------------------------------------------------------------
    case S_PARAM_NAME:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Function name scan.
    // ------------------------------------------------------------------
    case S_FUNC_NAME:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Open param list for a function declaration.
    //
    // S_FUNC_LPAREN transitions directly to S_EXTERN_PARAMS so the shared
    // parameter-parsing chain is reused.  We need to tell S_EXTERN_PARAMS's
    // OnEnterState to create the ParamList — add S_FUNC_LPAREN to its
    // fromState check below.  (S_FUNC_LPAREN itself has nothing to do here.)
    // ------------------------------------------------------------------
    case S_FUNC_LPAREN:
        break;

    // ------------------------------------------------------------------
    // Function body block.
    //
    // Entered from S_AFTER_PARAMS on '{'.  The opening '{' is consumed by
    // the normal transition advance.  We create a Block node and set
    // braceDepth = 1; the SN_SELF loop above handles the rest.
    // ------------------------------------------------------------------
    case S_BLOCK:
        if (fromState == S_AFTER_PARAMS) {
            auto* block     = ctx.allocNode(ParseKind::Block);
            // Override position: block starts at '{', not at the function name.
            block->line     = ctx.line;
            block->col      = ctx.col;
            block->srcStart = ctx.pos;
            ctx.addChild(block);
            ctx.pushNode(block);
            ctx.braceDepth  = 1;
        }
        break;

    // ------------------------------------------------------------------
    // Statement first-ident scan.
    // ------------------------------------------------------------------
    case S_STMT_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Variable name scan.
    //
    // When entered from S_STMT_AFTER_SPACE: pending holds the type name;
    // create VarDecl + Type and push VarDecl.
    // When entered from S_VARDECL_NAME_WAIT: VarDecl already on stack.
    // ------------------------------------------------------------------
    case S_VARDECL_NAME:
        if (fromState == S_STMT_AFTER_SPACE) {
            auto* vd = ctx.allocNode(ParseKind::VarDecl);
            ctx.addChild(vd);
            ctx.pushNode(vd);
            auto* ty = ctx.allocNode(ParseKind::Type);
            ty->name   = ctx.pending;
            ctx.pending = {};
            ctx.addChild(ty); // leaf: added to VarDecl frame, not pushed
        }
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Call arg list.
    //
    // When entered via a normal transition from S_STMT_AFTER_SPACE on '(':
    // pending holds the function name; build ExprStmt > Call > Ident + ArgList.
    // When entered from DispatchIdent: setup was already done there.
    // ------------------------------------------------------------------
    case S_CALL_ARGS:
        if (fromState == S_STMT_AFTER_SPACE) {
            auto* es = ctx.allocNode(ParseKind::ExprStmt);
            ctx.addChild(es);
            ctx.pushNode(es);
            auto* call = ctx.allocNode(ParseKind::Call);
            ctx.addChild(call);
            ctx.pushNode(call);
            auto* callee = ctx.allocNode(ParseKind::Ident);
            callee->name = ctx.pending;
            ctx.pending  = {};
            ctx.addChild(callee);
            auto* args = ctx.allocNode(ParseKind::ArgList);
            ctx.addChild(args);
            ctx.pushNode(args);
        }
        break;

    // ------------------------------------------------------------------
    // Argument ident scan.
    // ------------------------------------------------------------------
    case S_ARG_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Member ident scan.
    // ------------------------------------------------------------------
    case S_ARG_MEMBER_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // AddressOf creation.
    //
    // Entered from S_CALL_ARGS or S_INIT_WAIT on '&'.
    // ctx.addChild() adds AddressOf to the current top (ArgList or VarDecl).
    // ------------------------------------------------------------------
    case S_ARG_ADDR_WAIT: {
        auto* ao = ctx.allocNode(ParseKind::AddressOf);
        ctx.addChild(ao);
        ctx.pushNode(ao);
        break;
    }

    // ------------------------------------------------------------------
    // AddressOf operand ident scan.
    // ------------------------------------------------------------------
    case S_ARG_ADDR_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Numeric literal scan.
    // ------------------------------------------------------------------
    case S_ARG_NUMLIT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // Init-expression ident scan.
    // ------------------------------------------------------------------
    case S_INIT_IDENT:
        ctx.startToken();
        break;

    // ------------------------------------------------------------------
    // String literal: record the position of the opening '"'.
    // ------------------------------------------------------------------
    case S_INIT_STRLIT:
        ctx.startToken(); // tokenStart = position of opening '"'
        break;

    default:
        break;
    }
}

// =============================================================================
// OnLeaveState
//
// Fired when we are about to leave `fromState` for `toState` via character
// `cc`, AFTER normal state transition but BEFORE advance().  Handles node
// completions (pops) that are keyed on the leaving transition.
// =============================================================================
static void OnLeaveState(uint16_t fromState, uint16_t toState,
                         CharClass cc, ParseCtx& ctx)
{
    // ------------------------------------------------------------------
    // ParamList completion on ')'.
    //
    // Triggered from S_PARAM_AFTER_NAME or S_EXTERN_PARAMS (empty list).
    // DispatchIdent already popped the last Param (if any) before we get
    // here; we just need to pop the ParamList itself.
    // ------------------------------------------------------------------
    if (cc == CC_RPAREN &&
        (fromState == S_PARAM_AFTER_NAME || fromState == S_EXTERN_PARAMS))
    {
        if (ctx.current() && ctx.current()->kind == ParseKind::ParamList)
            ctx.popNode();
    }

    // ------------------------------------------------------------------
    // ExternDecl completion on ';'.
    // ------------------------------------------------------------------
    if (cc == CC_SEMI && fromState == S_AFTER_PARAMS) {
        if (ctx.current() && ctx.current()->kind == ParseKind::ExternDecl)
            ctx.popNode();
    }

    // ------------------------------------------------------------------
    // VarDecl completion on ';' (no initializer).
    // ------------------------------------------------------------------
    if (cc == CC_SEMI && fromState == S_VARDECL_AFTER_NAME) {
        if (ctx.current() && ctx.current()->kind == ParseKind::VarDecl)
            ctx.popNode();
    }

    // ------------------------------------------------------------------
    // Statement completion on ';' (after expression or call).
    // Pops ExprStmt or VarDecl (whichever is on top).
    // ------------------------------------------------------------------
    if (cc == CC_SEMI && fromState == S_AFTER_CALL) {
        ParseTreeNode* top = ctx.current();
        if (top && (top->kind == ParseKind::ExprStmt ||
                    top->kind == ParseKind::VarDecl))
            ctx.popNode();
    }

    // ------------------------------------------------------------------
    // Call arg list completion on ')'.
    //
    // Triggered from S_CALL_ARGS (empty arg list) or S_ARG_AFTER_IDENT
    // (last arg had trailing space).  DispatchIdent handles the non-empty
    // no-space case by popping directly and advancing.
    // ------------------------------------------------------------------
    if (cc == CC_RPAREN &&
        (fromState == S_CALL_ARGS || fromState == S_ARG_AFTER_IDENT))
    {
        if (ctx.current() && ctx.current()->kind == ParseKind::ArgList)
            ctx.popNode();
        if (ctx.current() && ctx.current()->kind == ParseKind::Call)
            ctx.popNode();
    }

    // ------------------------------------------------------------------
    // String literal completion on closing '"'.
    //
    // tokenStart was set to the opening '"' when S_INIT_STRLIT was entered.
    // ctx.pos is AT the closing '"' (Drive hasn't advanced yet).
    // ------------------------------------------------------------------
    if (cc == CC_DQUOTE && fromState == S_INIT_STRLIT) {
        auto* lit      = ctx.allocNode(ParseKind::StringLit);
        lit->srcStart  = ctx.tokenStart + 1;                    // skip opening '"'
        lit->srcLen    = ctx.pos - ctx.tokenStart - 1;          // content length
        ctx.addChild(lit);
    }
}

// =============================================================================
// Drive
//
// Main parse loop. Runs until SN_ACCEPT (clean EOF) or error.
// =============================================================================
static void Drive(ParseCtx& ctx)
{
    while (true) {
        const NestState& ns = kStates[ctx.state];
        CharClass cc = ctx.cls();

        // ------------------------------------------------------------------
        // Transparent whitespace skip.
        // ------------------------------------------------------------------
        if ((ns.flags & NS_SKIP_WS) && cc == CC_SPACE) {
            ctx.advance();
            continue;
        }

        // ------------------------------------------------------------------
        // Transparent line-comment skip  (// … \n).
        // ------------------------------------------------------------------
        if ((ns.flags & NS_SKIP_COMMENTS) && cc == CC_SLASH) {
            size_t p = ctx.pos + 1;
            if (p < ctx.source.size() && ctx.source[p] == '/') {
                while (ctx.pos < ctx.source.size() &&
                       ctx.source[ctx.pos] != '\n')
                    ctx.advance();
                continue;
            }
        }

        // ------------------------------------------------------------------
        // Look up the transition.
        // ------------------------------------------------------------------
        uint16_t next = ns.next[cc];

        // ------------------------------------------------------------------
        // Accept: end of source, parse succeeded.
        // ------------------------------------------------------------------
        if (next == SN_ACCEPT) {
            break;
        }

        // ------------------------------------------------------------------
        // Error: unexpected character.
        // ------------------------------------------------------------------
        if (next == SN_ERROR) {
            char got = (ctx.pos < ctx.source.size()) ? ctx.source[ctx.pos] : '\0';
            ctx.error(ns.expected, got);
            break; // no recovery yet
        }

        // ------------------------------------------------------------------
        // Dispatch: identifier token is complete.
        // ------------------------------------------------------------------
        if (next == SN_DISPATCH) {
            uint16_t newState = DispatchIdent(ctx);
            OnEnterState(ctx.state, newState, cc, ctx);
            ctx.state = newState;
            // Do NOT advance — the triggering char is re-processed.
            continue;
        }

        // ------------------------------------------------------------------
        // Self-loop: stay in current state (accumulate the current char).
        // ------------------------------------------------------------------
        if (next == SN_SELF) {
            OnEnterState(ctx.state, ctx.state, cc, ctx); // for pointer-depth
            ctx.advance();
            continue;
        }

        // ------------------------------------------------------------------
        // Normal transition to a new state.
        // ------------------------------------------------------------------
        uint16_t fromState = ctx.state;
        OnLeaveState(fromState, next, cc, ctx);
        OnEnterState(fromState, next, cc, ctx);
        ctx.state = uint16_t(next);
        ctx.advance();
    }

    // ------------------------------------------------------------------
    // Close the File node (if we opened one).
    // ------------------------------------------------------------------
    if (ctx.xmo.parseTree && !ctx.stack.empty() &&
        ctx.stack.front().node == ctx.xmo.parseTree)
    {
        // Unwind any remaining open nodes (e.g. on error).
        while (ctx.stack.size() > 1) ctx.popNode();
        ctx.popNode(); // File
    }
}

// =============================================================================
// WriteParserLog (driver glue)
// =============================================================================
static void EmitParserLog(ParseTreeNode* root,
                          const std::vector<Frame>& stack,
                          const std::string& fileName,
                          std::string_view   source)
{
    WriteParserLog(root, stack, fileName, source);
}

} // namespace (anonymous)

// =============================================================================
// Parser::Parse  — public entry point
// =============================================================================
void Parser::Parse(
    Xmo&               xmo,
    std::string_view   source,
    SymbolTable&       symbols,
    BS::thread_pool<>& pool,
    const CompileJob&  job)
{
    ParseCtx ctx{ xmo, source, symbols, pool, job };

    Drive(ctx);

    if (job.ParserLog) {
        EmitParserLog(xmo.parseTree, ctx.stack, xmo.name, source);
    }

    // Run the whole-tree morpher pass (waits for any outstanding MorphNoun
    // tasks to finish internally via pool.wait() before returning).
    if (!job.ErrorOccurred.load(std::memory_order_relaxed)) {
        Morpher::MorphTree(xmo, symbols, job);
    }
}

} // namespace xmc
