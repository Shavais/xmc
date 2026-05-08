// compiler/Parser.cpp
//
// Recursive-descent parser for .xm sources. See Parser.h for the
// pipeline contract.
//
// The grammar handled here is the subset hello.xm exercises:
//
//   file        := top_decl* EOF
//   top_decl    := extern_decl | func_decl
//
//   extern_decl := 'extern' qual_name '(' param_list? ')'
//                  ('->' type (':' identifier)?)? ';'
//   func_decl   := type identifier '(' param_list? ')' block
//
//   param_list  := param (',' param)*
//   param       := identifier '->' type (':' identifier)?
//
//   type        := '*'* simple_type ('[' ']')?
//   simple_type := primitive_kw | identifier
//
//   block       := '{' stmt* '}'
//   stmt        := var_decl | return_stmt | expr_stmt
//   var_decl    := type identifier ('=' expr)? ';'
//   return_stmt := 'return' expr? ';'
//   expr_stmt   := expr ';'
//
//   expr        := unary postfix_chain
//   unary       := ('@' | '$' | '&' | '-' | '+' | '!' | '~')? primary
//   primary     := literal | identifier | '(' expr ')'
//   postfix_chain := ( '.' identifier
//                    | '[' expr ']'
//                    | '(' arg_list? ')' )*
//   arg_list    := expr (',' expr)*
//
// `&` as a unary prefix is accepted as address-of for compatibility with
// hello.xm even though Section 8b of the language spec uses `@`. The two
// fold into the same ParseKind::AddressOf node.
//
#include "pch/pch.h"
#include "Parser.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tool/Logger.h"

namespace fs = std::filesystem;

namespace xmc
{
	// =================================================================
	// ParseKindName
	// =================================================================
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
		}
		return "<bad-kind>";
	}

	namespace
	{
		using T = Lexer::TokenType;

		// =============================================================
		// Token classification helpers
		// =============================================================

		// True if `tt` is one of the primitive type keywords that can
		// start a type expression on its own (b, i8..i64, u8..u64, etc.).
		bool IsPrimitiveTypeKw(T tt)
		{
			switch (tt) {
			case T::KW_B:
			case T::KW_I8: case T::KW_I16: case T::KW_I32: case T::KW_I64:
			case T::KW_U8: case T::KW_U16: case T::KW_U32: case T::KW_U64:
			case T::KW_F32: case T::KW_F64:
			case T::KW_D16: case T::KW_D32: case T::KW_D64:
			case T::KW_C8: case T::KW_C16:
			case T::KW_ZS8: case T::KW_ZS16: case T::KW_ZS32:
			case T::KW_S8: case T::KW_S16:
			case T::KW_UTF8: case T::KW_US:
				return true;
			default:
				return false;
			}
		}

		// True if `tt` can start a type expression (primitive keyword,
		// container keyword, identifier, or a leading `*` for a pointer).
		bool CanStartType(T tt)
		{
			if (IsPrimitiveTypeKw(tt)) return true;
			switch (tt) {
			case T::OP_STAR:
			case T::IDENTIFIER:
			case T::KW_LIST:    case T::KW_QUEUE:    case T::KW_STACK:
			case T::KW_BTREE:   case T::KW_HASHSET:  case T::KW_HASHMAP:
			case T::KW_FUNCTION:
				return true;
			default:
				return false;
			}
		}

		// Decode a numeric literal from its source slice into a u64.
		// Recognizes 0x.. and 0b.. prefixes; falls back to base 10.
		// Floats fall through this and end up as FloatLit elsewhere.
		uint64_t ParseIntLiteral(std::string_view text)
		{
			if (text.size() >= 2 && text[0] == '0' &&
				(text[1] == 'x' || text[1] == 'X'))
			{
				uint64_t v = 0;
				for (size_t i = 2; i < text.size(); ++i) {
					char c = text[i];
					uint64_t d = 0;
					if (c >= '0' && c <= '9') d = uint64_t(c - '0');
					else if (c >= 'a' && c <= 'f') d = 10 + uint64_t(c - 'a');
					else if (c >= 'A' && c <= 'F') d = 10 + uint64_t(c - 'A');
					else continue; // skip _ separators if the language allows
					v = (v << 4) | d;
				}
				return v;
			}
			if (text.size() >= 2 && text[0] == '0' &&
				(text[1] == 'b' || text[1] == 'B'))
			{
				uint64_t v = 0;
				for (size_t i = 2; i < text.size(); ++i) {
					char c = text[i];
					if (c == '0' || c == '1') {
						v = (v << 1) | uint64_t(c - '0');
					}
				}
				return v;
			}
			uint64_t v = 0;
			for (char c : text) {
				if (c >= '0' && c <= '9') {
					v = v * 10 + uint64_t(c - '0');
				}
			}
			return v;
		}

		// =============================================================
		// ParserState
		//
		// Per-Parse state. One instance per file, stack-allocated in
		// Parser::Parse. Holds the token cursor with a small lookahead
		// buffer, the source view, and arena/symbols/job references.
		// =============================================================
		struct ParserState
		{
			Xmo& xmo;
			std::string_view                source;
			PipelineQueue<Lexer::Token>& tokQueue;
			PipelineQueue<ParseTreeNode*>& nodeQueue;
			SymbolTable& symbols;
			const CompileJob& job;

			// 2-token lookahead: enough to disambiguate `type identifier (`
			// (function decl) from `type identifier =` / `type identifier ;`
			// (variable decl). The Lexer enqueues TOK_EOF as the stream
			// terminator; once we've seen it, we keep it parked at lookahead[0]
			// so repeated peeks return TOK_EOF cleanly.
			Lexer::Token lookahead[2];
			bool         haveLook[2] = { false, false };

			// Tree-order list of every node we've allocated, root first.
			// Used both to write the parser log and to verify we haven't
			// orphaned anyone.
			std::vector<ParseTreeNode*> allNodes;

			ParserState(Xmo& x, std::string_view src,
				PipelineQueue<Lexer::Token>& tq,
				PipelineQueue<ParseTreeNode*>& nq,
				SymbolTable& syms, const CompileJob& j)
				: xmo(x), source(src), tokQueue(tq), nodeQueue(nq),
				symbols(syms), job(j) {
			}

			// ---- Token cursor ---------------------------------------

			// Logical cursor over the token stream.
			//
			// PipelineQueue is a single-consumer pipe: the only way to
			// see a token is to physically dequeue it. Lookahead is
			// implemented by parking dequeued-but-not-yet-consumed
			// tokens in `lookahead[]`. From any caller's perspective
			// Peek is non-destructive -- it can be called repeatedly
			// and a later Eat returns the same token -- because
			// `lookahead[]` is queried before the queue.

			// Peek at the i'th token ahead (i in [0, 1]) without
			// consuming it from the parser's cursor.
			const Lexer::Token& Peek(int i = 0)
			{
				assert(i >= 0 && i < 2);
				for (int j = 0; j <= i; ++j) {
					if (!haveLook[j]) {
						tokQueue.WaitDequeue(lookahead[j]);
						haveLook[j] = true;
					}
				}
				return lookahead[i];
			}

			// Consume the next token from the cursor and return it.
			// Drains `lookahead[]` first; only hits the queue if the
			// buffer is empty (i.e. nothing has been Peek'd yet).
			Lexer::Token Eat()
			{
				Lexer::Token t;
				if (haveLook[0]) {
					t = lookahead[0];
					if (haveLook[1]) {
						lookahead[0] = lookahead[1];
						haveLook[1] = false;
					}
					else {
						haveLook[0] = false;
					}
					return t;
				}
				tokQueue.WaitDequeue(t);
				return t;
			}

			// Consume the next token if it matches `tt`; return whether it did.
			bool EatIf(T tt)
			{
				if (Peek().type == tt) { Eat(); return true; }
				return false;
			}

			// Consume `tt` or emit a diagnostic and don't consume.
			Lexer::Token Expect(T tt, const char* what)
			{
				const Lexer::Token& nxt = Peek();
				if (nxt.type == tt) return Eat();
				ReportError(nxt, std::string("expected ") + what +
					", got " + Lexer::TokenTypeName(nxt.type));
				// Synthesize a placeholder token so callers can keep
				// going. srcStart/srcLen of zero means "no source span".
				Lexer::Token fake{};
				fake.type = tt;
				fake.line = nxt.line;
				fake.col = nxt.col;
				return fake;
			}

			std::string_view TokenText(const Lexer::Token& t) const
			{
				return source.substr(t.srcStart, t.srcLen);
			}

			// ---- Diagnostics ----------------------------------------

			void ReportError(const Lexer::Token& at, const std::string& msg)
			{
				oserror << xmo.name << ":" << at.line << ":" << at.col
					<< ": parse error: " << msg << std::endl;
				job.ErrorOccurred.store(true, std::memory_order_relaxed);
			}

			// ---- Node construction ----------------------------------

			// Allocate a node into the xmo's arena; record it in allNodes
			// so we can log and so the post-order push has a deterministic
			// list to walk if we ever batch the queue handoff.
			ParseTreeNode* NewNode(ParseKind k, const Lexer::Token& at)
			{
				ParseTreeNode* n = xmo.arena.Construct<ParseTreeNode>();
				n->kind = k;
				n->srcStart = at.srcStart;
				n->srcLen = at.srcLen;
				n->line = at.line;
				n->col = at.col;
				allNodes.push_back(n);
				return n;
			}

			// Materialize an arena-owned children array from a temporary
			// vector and wire parent pointers. Each child whose own
			// childCount is zero is enqueued to the Morpher here, with
			// its parent pointer now set -- the Morpher reaches every
			// non-leaf ancestor by walking up from a leaf, so enqueueing
			// the non-leaves separately would be redundant.
			//
			// A child's childCount is meaningful at the moment we read
			// it because children are constructed bottom-up: by the time
			// AttachChildren runs on `parent`, every child has already
			// finished its own AttachChildren call and its descendant
			// shape is final.
			void AttachChildren(ParseTreeNode* parent,
				const std::vector<ParseTreeNode*>& kids)
			{
				if (kids.empty()) {
					parent->children = nullptr;
					parent->childCount = 0;
					return;
				}
				ParseTreeNode** arr = static_cast<ParseTreeNode**>(
					xmo.arena.Allocate(sizeof(ParseTreeNode*) * kids.size()));
				for (size_t i = 0; i < kids.size(); ++i) {
					arr[i] = kids[i];
					kids[i]->parent = parent;
					if (kids[i]->childCount == 0) {
						ParseTreeNode* leaf = kids[i];
						nodeQueue.Enqueue(std::move(leaf));
					}
				}
				parent->children = arr;
				parent->childCount = static_cast<uint32_t>(kids.size());
			}

			// (No per-node Finalize: a leaf is enqueued by its parent's
			// AttachChildren, so the leaf has a wired parent at queue
			// time. Non-leaf nodes are reachable from any of their leaf
			// descendants via parent walks; enqueueing them too would be
			// redundant work for the Morpher.)

			// =========================================================
			// Grammar productions
			// =========================================================

			ParseTreeNode* ParseFile();

			ParseTreeNode* ParseExternDecl();
			ParseTreeNode* ParseFuncDecl(ParseTreeNode* returnType,
				const Lexer::Token& nameTok);
			ParseTreeNode* ParseParam();
			ParseTreeNode* ParseReturnSpec();   // for extern: -> type [: reg]
			ParseTreeNode* ParseType();
			ParseTreeNode* ParseBlock();
			ParseTreeNode* ParseStmt();
			ParseTreeNode* ParseVarDecl(ParseTreeNode* type,
				const Lexer::Token& nameTok);
			ParseTreeNode* ParseReturnStmt();
			ParseTreeNode* ParseExprStmt();

			ParseTreeNode* ParseExpr();
			ParseTreeNode* ParseUnary();
			ParseTreeNode* ParsePrimary();
			ParseTreeNode* ParsePostfixChain(ParseTreeNode* lhs);

			void Skip(T tt) { while (Peek().type == tt) Eat(); }

			void RecoverToStmtBoundary();

			void WriteParserLog(ParseTreeNode* root);
			void DumpNode(std::ofstream& out, const ParseTreeNode* n, int depth);
		};

		// =============================================================
		// File / top-level decls
		// =============================================================

		ParseTreeNode* ParserState::ParseFile()
		{
			Lexer::Token start = Peek();
			ParseTreeNode* file = NewNode(ParseKind::File, start);

			std::vector<ParseTreeNode*> decls;
			while (Peek().type != T::TOK_EOF) {
				ParseTreeNode* decl = nullptr;

				if (Peek().type == T::KW_EXTERN) {
					decl = ParseExternDecl();
				}
				else if (CanStartType(Peek().type)) {
					// A top-level type-prefixed item is a function decl
					// (we don't support top-level vars here -- the
					// language doesn't strictly forbid them but hello.xm
					// doesn't exercise it and a function is the natural
					// reading of `i32 main()`).
					ParseTreeNode* type = ParseType();
					Lexer::Token nameTok = Expect(T::IDENTIFIER,
						"function name after return type");
					if (Peek().type == T::PUNCT_LPAREN) {
						decl = ParseFuncDecl(type, nameTok);
					}
					else {
						ReportError(Peek(),
							"expected '(' to begin function parameter list");
						RecoverToStmtBoundary();
						continue;
					}
				}
				else {
					ReportError(Peek(), std::string(
						"unexpected token at top level: ") +
						Lexer::TokenTypeName(Peek().type));
					RecoverToStmtBoundary();
					continue;
				}

				if (decl) decls.push_back(decl);
			}

			AttachChildren(file, decls);
			return file;
		}

		// =============================================================
		// extern <qual>:<name> ( params ) -> type [: reg] ;
		// extern <name>        ( params ) ;
		//
		// hello.xm uses the qualified-namespace form for kernel32:*.
		// =============================================================
		ParseTreeNode* ParserState::ParseExternDecl()
		{
			Lexer::Token kw = Eat(); // KW_EXTERN
			ParseTreeNode* node = NewNode(ParseKind::ExternDecl, kw);

			// First identifier may be either the function name (no
			// qualifier) or the qualifier (with `:` and a second
			// identifier following).
			Lexer::Token first = Expect(T::IDENTIFIER,
				"identifier after 'extern'");
			if (Peek().type == T::PUNCT_COLON) {
				Eat(); // ':'
				node->qualifier = symbols.InternString(TokenText(first));
				Lexer::Token nameTok = Expect(T::IDENTIFIER,
					"identifier after ':'");
				node->name = symbols.InternString(TokenText(nameTok));
			}
			else {
				node->name = symbols.InternString(TokenText(first));
			}

			Expect(T::PUNCT_LPAREN, "'(' to begin parameter list");

			std::vector<ParseTreeNode*> kids;
			ParseTreeNode* paramList = NewNode(ParseKind::ParamList, Peek());
			std::vector<ParseTreeNode*> params;
			if (Peek().type != T::PUNCT_RPAREN) {
				while (true) {
					ParseTreeNode* p = ParseParam();
					if (p) params.push_back(p);
					if (Peek().type == T::PUNCT_COMMA) { Eat(); continue; }
					break;
				}
			}
			AttachChildren(paramList, params);
			kids.push_back(paramList);

			Expect(T::PUNCT_RPAREN, "')' to end parameter list");

			// Optional return spec: -> type [: reg]
			if (Peek().type == T::OP_ARROW) {
				ParseTreeNode* ret = ParseReturnSpec();
				if (ret) kids.push_back(ret);
			}

			Expect(T::PUNCT_SEMICOLON, "';' to end extern declaration");

			AttachChildren(node, kids);
			return node;
		}

		// =============================================================
		// param := identifier '->' type (':' identifier)?
		// =============================================================
		ParseTreeNode* ParserState::ParseParam()
		{
			Lexer::Token nameTok = Expect(T::IDENTIFIER, "parameter name");
			ParseTreeNode* p = NewNode(ParseKind::Param, nameTok);
			p->name = symbols.InternString(TokenText(nameTok));

			Expect(T::OP_ARROW, "'->' after parameter name");

			ParseTreeNode* type = ParseType();
			std::vector<ParseTreeNode*> kids;
			if (type) kids.push_back(type);

			// Optional register clause: ':' identifier
			if (Peek().type == T::PUNCT_COLON) {
				Eat();
				Lexer::Token reg = Expect(T::IDENTIFIER,
					"register name after ':'");
				p->regHint = symbols.InternString(TokenText(reg));
			}

			AttachChildren(p, kids);
			return p;
		}

		// =============================================================
		// return_spec := '->' type (':' identifier)?
		//
		// Used for the post-')' return descriptor of an extern decl.
		// Same shape as Param but no name; we mark this with kind
		// ReturnSpec for log clarity.
		// =============================================================
		ParseTreeNode* ParserState::ParseReturnSpec()
		{
			Lexer::Token arrow = Eat(); // OP_ARROW
			ParseTreeNode* node = NewNode(ParseKind::ReturnSpec, arrow);

			ParseTreeNode* type = ParseType();
			std::vector<ParseTreeNode*> kids;
			if (type) kids.push_back(type);

			if (Peek().type == T::PUNCT_COLON) {
				Eat();
				Lexer::Token reg = Expect(T::IDENTIFIER,
					"register name after ':'");
				node->regHint = symbols.InternString(TokenText(reg));
			}

			AttachChildren(node, kids);
			return node;
		}

		// =============================================================
		// type := '*'* simple_type ('[' ']')?
		// =============================================================
		ParseTreeNode* ParserState::ParseType()
		{
			// Count leading stars.
			Lexer::Token first = Peek();
			uint8_t depth = 0;
			while (Peek().type == T::OP_STAR) {
				Eat();
				if (depth < 255) depth++;
			}

			Lexer::Token core = Peek();
			ParseTreeNode* node = NewNode(ParseKind::Type, depth ? first : core);
			node->pointerDepth = depth;

			if (IsPrimitiveTypeKw(core.type)) {
				Eat();
				node->name = symbols.InternString(TokenText(core));
				// We could also set node->baseType to a known id here.
				// Deferred until BaseTypeIds is finalized; the Morpher
				// will resolve from `name` for now.
			}
			else if (core.type == T::IDENTIFIER) {
				Eat();
				node->name = symbols.InternString(TokenText(core));
			}
			else if (core.type == T::KW_LIST || core.type == T::KW_QUEUE ||
				core.type == T::KW_STACK || core.type == T::KW_BTREE ||
				core.type == T::KW_HASHSET || core.type == T::KW_HASHMAP ||
				core.type == T::KW_FUNCTION)
			{
				Eat();
				node->name = symbols.InternString(TokenText(core));
				// Generic argument lists (list<T> etc.) are not handled
				// here; hello.xm doesn't use them. Adding `<...>` parsing
				// is the natural follow-on.
			}
			else {
				ReportError(core, std::string("expected a type, got ") +
					Lexer::TokenTypeName(core.type));
			}

			// Trailing "[]" makes it an array.
			if (Peek().type == T::PUNCT_LBRACKET) {
				Lexer::Token lb = Peek();
				Eat();
				if (Peek().type == T::PUNCT_RBRACKET) {
					Eat();
					node->isArray = true;
				}
				else {
					ReportError(lb, "expected ']' after '[' in type");
				}
			}

			return node;
		}

		// =============================================================
		// func_decl := type identifier '(' params? ')' block
		//
		// The caller has already consumed `type identifier`; we get the
		// '(' here.
		// =============================================================
		ParseTreeNode* ParserState::ParseFuncDecl(ParseTreeNode* returnType,
			const Lexer::Token& nameTok)
		{
			ParseTreeNode* fn = NewNode(ParseKind::FuncDecl, nameTok);
			fn->name = symbols.InternString(TokenText(nameTok));

			std::vector<ParseTreeNode*> kids;
			if (returnType) kids.push_back(returnType);

			Expect(T::PUNCT_LPAREN, "'(' to begin parameter list");

			ParseTreeNode* paramList = NewNode(ParseKind::ParamList, Peek());
			std::vector<ParseTreeNode*> params;
			if (Peek().type != T::PUNCT_RPAREN) {
				while (true) {
					ParseTreeNode* p = ParseParam();
					if (p) params.push_back(p);
					if (Peek().type == T::PUNCT_COMMA) { Eat(); continue; }
					break;
				}
			}
			AttachChildren(paramList, params);
			kids.push_back(paramList);

			Expect(T::PUNCT_RPAREN, "')' to end parameter list");

			ParseTreeNode* body = ParseBlock();
			if (body) kids.push_back(body);

			AttachChildren(fn, kids);
			return fn;
		}

		// =============================================================
		// block := '{' stmt* '}'
		// =============================================================
		ParseTreeNode* ParserState::ParseBlock()
		{
			Lexer::Token lb = Expect(T::PUNCT_LBRACE,
				"'{' to begin block");
			ParseTreeNode* block = NewNode(ParseKind::Block, lb);

			std::vector<ParseTreeNode*> stmts;
			while (Peek().type != T::PUNCT_RBRACE &&
				Peek().type != T::TOK_EOF)
			{
				ParseTreeNode* s = ParseStmt();
				if (s) stmts.push_back(s);
			}
			Expect(T::PUNCT_RBRACE, "'}' to close block");

			AttachChildren(block, stmts);
			return block;
		}

		// =============================================================
		// stmt := var_decl | return_stmt | expr_stmt
		//
		// We disambiguate var-decl from expr-stmt with one token of
		// lookahead beyond the type: `type identifier (` is a function
		// call (expr stmt); `type identifier ; | =` is a var decl.
		// At statement level there's no function-decl form, so the
		// `(` case never produces a func decl here.
		// =============================================================
		ParseTreeNode* ParserState::ParseStmt()
		{
			if (Peek().type == T::KW_RETURN) return ParseReturnStmt();

			if (CanStartType(Peek().type)) {
				// Tentatively parse a type, but only if what follows
				// ultimately matches the var-decl shape. We commit
				// after consuming the type because all the "begins a
				// type" tokens (primitive keywords, '*', container
				// keywords) cannot start a non-declaration statement
				// in our subset; identifiers are the one ambiguity.
				if (Peek().type == T::IDENTIFIER) {
					// `Identifier ...`: could be var decl `T x;` or an
					// expression `T(...)` / `T.field` / `T = ...`.
					// Check Peek(1): if it's another identifier, it's a
					// var decl with a user-named type. Otherwise it's
					// an expression.
					if (Peek(1).type == T::IDENTIFIER) {
						ParseTreeNode* type = ParseType();
						Lexer::Token nameTok = Expect(T::IDENTIFIER,
							"variable name");
						return ParseVarDecl(type, nameTok);
					}
					return ParseExprStmt();
				}
				// `*`-prefixed or primitive-keyword start: must be a
				// declaration (no statement-level expression starts
				// with these tokens in our subset).
				ParseTreeNode* type = ParseType();
				Lexer::Token nameTok = Expect(T::IDENTIFIER,
					"variable name after type");
				return ParseVarDecl(type, nameTok);
			}

			return ParseExprStmt();
		}

		// =============================================================
		// var_decl := type identifier ('=' expr)? ';'
		//
		// The caller has already consumed `type identifier`; we handle
		// the optional initializer and the terminator.
		// =============================================================
		ParseTreeNode* ParserState::ParseVarDecl(ParseTreeNode* type,
			const Lexer::Token& nameTok)
		{
			ParseTreeNode* decl = NewNode(ParseKind::VarDecl, nameTok);
			decl->name = symbols.InternString(TokenText(nameTok));

			std::vector<ParseTreeNode*> kids;
			if (type) kids.push_back(type);

			if (Peek().type == T::OP_ASSIGN) {
				Eat();
				ParseTreeNode* init = ParseExpr();
				if (init) kids.push_back(init);
			}

			Expect(T::PUNCT_SEMICOLON, "';' to end variable declaration");

			AttachChildren(decl, kids);
			return decl;
		}

		// =============================================================
		// return_stmt := 'return' expr? ';'
		// =============================================================
		ParseTreeNode* ParserState::ParseReturnStmt()
		{
			Lexer::Token kw = Eat(); // KW_RETURN
			ParseTreeNode* ret = NewNode(ParseKind::Return, kw);

			std::vector<ParseTreeNode*> kids;
			if (Peek().type != T::PUNCT_SEMICOLON) {
				ParseTreeNode* e = ParseExpr();
				if (e) kids.push_back(e);
			}
			Expect(T::PUNCT_SEMICOLON, "';' after return");
			AttachChildren(ret, kids);
			return ret;
		}

		// =============================================================
		// expr_stmt := expr ';'
		// Also handles top-level assignment as `lhs '=' expr`.
		// =============================================================
		ParseTreeNode* ParserState::ParseExprStmt()
		{
			Lexer::Token start = Peek();
			ParseTreeNode* lhs = ParseExpr();

			ParseTreeNode* node = nullptr;
			if (Peek().type == T::OP_ASSIGN) {
				Lexer::Token eq = Eat();
				ParseTreeNode* rhs = ParseExpr();
				ParseTreeNode* assign = NewNode(ParseKind::Assign, eq);
				assign->opToken = uint16_t(T::OP_ASSIGN);
				std::vector<ParseTreeNode*> kids;
				if (lhs) kids.push_back(lhs);
				if (rhs) kids.push_back(rhs);
				AttachChildren(assign, kids);
				node = assign;
			}
			else {
				node = lhs;
			}

			Expect(T::PUNCT_SEMICOLON, "';' to end statement");

			ParseTreeNode* stmt = NewNode(ParseKind::ExprStmt, start);
			std::vector<ParseTreeNode*> kids;
			if (node) kids.push_back(node);
			AttachChildren(stmt, kids);
			return stmt;
		}

		// =============================================================
		// expr := unary postfix_chain
		//
		// Binary operators are not exercised by hello.xm; we keep the
		// hook (BinOp) but parse only single-operand expressions for
		// now. Adding precedence climbing is the right next step.
		// =============================================================
		ParseTreeNode* ParserState::ParseExpr()
		{
			return ParseUnary();
		}

		ParseTreeNode* ParserState::ParseUnary()
		{
			T tt = Peek().type;
			if (tt == T::OP_AT || tt == T::OP_DOLLAR ||
				tt == T::OP_AMP || tt == T::OP_MINUS ||
				tt == T::OP_PLUS || tt == T::OP_BANG ||
				tt == T::OP_TILDE)
			{
				Lexer::Token op = Eat();
				ParseTreeNode* operand = ParseUnary();
				ParseKind k =
					tt == T::OP_AT ? ParseKind::AddressOf :
					tt == T::OP_DOLLAR ? ParseKind::Deref :
					tt == T::OP_AMP ? ParseKind::AddressOf :
					tt == T::OP_MINUS ? ParseKind::Negate :
					tt == T::OP_PLUS ? ParseKind::UnaryPlus :
					tt == T::OP_BANG ? ParseKind::Not :
					ParseKind::BitNot;
				ParseTreeNode* node = NewNode(k, op);
				node->opToken = uint16_t(tt);
				std::vector<ParseTreeNode*> kids;
				if (operand) kids.push_back(operand);
				AttachChildren(node, kids);
				return node;
			}

			ParseTreeNode* p = ParsePrimary();
			return ParsePostfixChain(p);
		}

		ParseTreeNode* ParserState::ParsePrimary()
		{
			Lexer::Token tk = Peek();
			switch (tk.type) {
			case T::LIT_INTEGER: {
				Eat();
				ParseTreeNode* n = NewNode(ParseKind::IntLit, tk);
				n->intValue = ParseIntLiteral(TokenText(tk));
				return n;
			}
			case T::LIT_FLOAT:
			case T::LIT_DECIMAL: {
				Eat();
				ParseTreeNode* n = NewNode(ParseKind::FloatLit, tk);
				// Conservative: don't attempt to parse the full xmc
				// decimal grammar here; the Morpher / a future
				// numeric helper will. Keep raw bytes via srcStart/srcLen.
				return n;
			}
			case T::LIT_STRING: {
				Eat();
				ParseTreeNode* n = NewNode(ParseKind::StringLit, tk);
				return n;
			}
			case T::LIT_CHAR: {
				Eat();
				ParseTreeNode* n = NewNode(ParseKind::CharLit, tk);
				// First character of the unquoted slice; escape
				// handling is the Morpher's job. Hello.xm has no
				// char literals, so this stays intentionally simple.
				std::string_view raw = TokenText(tk);
				if (raw.size() >= 3) n->intValue = uint8_t(raw[1]);
				return n;
			}
			case T::KW_TRUE:
			case T::KW_FALSE: {
				Eat();
				ParseTreeNode* n = NewNode(ParseKind::BoolLit, tk);
				n->intValue = (tk.type == T::KW_TRUE) ? 1 : 0;
				return n;
			}
			case T::KW_NULL: {
				Eat();
				ParseTreeNode* n = NewNode(ParseKind::NullLit, tk);
				return n;
			}
			case T::IDENTIFIER: {
				Eat();
				ParseTreeNode* n = NewNode(ParseKind::Ident, tk);
				n->name = symbols.InternString(TokenText(tk));
				return n;
			}
			case T::PUNCT_LPAREN: {
				Eat();
				ParseTreeNode* inner = ParseExpr();
				Expect(T::PUNCT_RPAREN, "')' to close parenthesized expression");
				return inner;
			}
			default: {
				ReportError(tk, std::string(
					"expected expression, got ") +
					Lexer::TokenTypeName(tk.type));
				// Synthesize a NullLit so the caller has something
				// to attach. The error has been recorded.
				Lexer::Token fake = tk;
				Eat();
				ParseTreeNode* n = NewNode(ParseKind::NullLit, fake);
				return n;
			}
			}
		}

		// =============================================================
		// postfix_chain := ( '.' ident | '[' expr ']' | '(' args? ')' )*
		// =============================================================
		ParseTreeNode* ParserState::ParsePostfixChain(ParseTreeNode* lhs)
		{
			while (true) {
				T tt = Peek().type;
				if (tt == T::PUNCT_DOT) {
					Lexer::Token dot = Eat();
					Lexer::Token mem = Expect(T::IDENTIFIER,
						"member name after '.'");
					ParseTreeNode* m = NewNode(ParseKind::MemberAccess, dot);
					m->name = symbols.InternString(TokenText(mem));
					std::vector<ParseTreeNode*> kids;
					if (lhs) kids.push_back(lhs);
					AttachChildren(m, kids);
					lhs = m;
				}
				else if (tt == T::PUNCT_LBRACKET) {
					Lexer::Token lb = Eat();
					ParseTreeNode* idx = ParseExpr();
					Expect(T::PUNCT_RBRACKET, "']' to close subscript");
					ParseTreeNode* s = NewNode(ParseKind::Subscript, lb);
					std::vector<ParseTreeNode*> kids;
					if (lhs) kids.push_back(lhs);
					if (idx) kids.push_back(idx);
					AttachChildren(s, kids);
					lhs = s;
				}
				else if (tt == T::PUNCT_LPAREN) {
					Lexer::Token lp = Eat();
					ParseTreeNode* argList = NewNode(ParseKind::ArgList, lp);
					std::vector<ParseTreeNode*> args;
					if (Peek().type != T::PUNCT_RPAREN) {
						while (true) {
							ParseTreeNode* a = ParseExpr();
							if (a) args.push_back(a);
							if (Peek().type == T::PUNCT_COMMA) {
								Eat();
								continue;
							}
							break;
						}
					}
					Expect(T::PUNCT_RPAREN, "')' to close argument list");
					AttachChildren(argList, args);

					ParseTreeNode* call = NewNode(ParseKind::Call, lp);
					std::vector<ParseTreeNode*> kids;
					if (lhs) kids.push_back(lhs);
					kids.push_back(argList);
					AttachChildren(call, kids);
					lhs = call;
				}
				else {
					return lhs;
				}
			}
		}

		// =============================================================
		// Error recovery
		//
		// Skip tokens until the next reasonable resync point. We use
		// `;`, `}`, or top-level keywords. The synchronization choice
		// here is deliberately permissive: getting back into the
		// productive grammar quickly matters more than emitting the
		// "ideal" set of secondary diagnostics.
		// =============================================================
		void ParserState::RecoverToStmtBoundary()
		{
			while (true) {
				T tt = Peek().type;
				if (tt == T::TOK_EOF) return;
				if (tt == T::PUNCT_SEMICOLON) { Eat(); return; }
				if (tt == T::PUNCT_RBRACE)    return;
				if (tt == T::KW_EXTERN)       return;
				Eat();
			}
		}

		// =============================================================
		// Parser log
		//
		// Tree-walk, indented one tab per level. Mirrors the lexer log
		// shape (sibling file alongside the source) so the two can be
		// diffed against goldens with the same machinery. Format:
		//
		//   <filename>:<line>:<col>  <indent><Kind>  <interesting fields>
		// =============================================================
		void ParserState::WriteParserLog(ParseTreeNode* root)
		{
			fs::path src(xmo.name);
			fs::path logPath = src.parent_path() /
				(src.stem().string() + ".parser.txt");
			std::ofstream out(logPath);
			if (!out) return;
			DumpNode(out, root, 0);
		}

		// Returns up to `len` bytes from the source starting at `start`,
		// with newlines and tabs collapsed to single spaces. Used for
		// the "raw text" suffix of literal entries in the parser log.
		std::string Excerpt(std::string_view src, uint32_t start, uint32_t len)
		{
			if (start >= src.size()) return {};
			uint32_t end = start + len;
			if (end > src.size()) end = uint32_t(src.size());
			std::string out;
			out.reserve(end - start);
			for (uint32_t i = start; i < end; ++i) {
				char c = src[i];
				if (c == '\n' || c == '\r' || c == '\t') c = ' ';
				out += c;
			}
			return out;
		}

		void ParserState::DumpNode(std::ofstream& out,
			const ParseTreeNode* n, int depth)
		{
			if (!n) return;

			// Position prefix, padded to width that matches the lexer log.
			std::string pos = xmo.name + ":" +
				std::to_string(n->line) + ":" + std::to_string(n->col);
			out.write(pos.data(), std::streamsize(pos.size()));
			for (size_t pad = pos.size(); pad < 24; ++pad) out.put(' ');

			// Indent.
			for (int i = 0; i < depth; ++i) out.put(' '), out.put(' ');

			// Kind.
			out << ParseKindName(n->kind);

			// Interesting per-kind fields.
			switch (n->kind) {
			case ParseKind::ExternDecl:
			case ParseKind::FuncDecl:
			case ParseKind::Param:
			case ParseKind::VarDecl:
			case ParseKind::Ident:
			case ParseKind::MemberAccess:
			{
				out << "  ";
				if (n->qualifier.str && n->qualifier.len) {
					out.write(n->qualifier.str, n->qualifier.len);
					out << ":";
				}
				if (n->name.str && n->name.len) {
					out.write(n->name.str, n->name.len);
				}
				if (n->kind == ParseKind::Param &&
					n->regHint.str && n->regHint.len)
				{
					out << " : ";
					out.write(n->regHint.str, n->regHint.len);
				}
				break;
			}
			case ParseKind::ReturnSpec:
			{
				if (n->regHint.str && n->regHint.len) {
					out << "  : ";
					out.write(n->regHint.str, n->regHint.len);
				}
				break;
			}
			case ParseKind::Type:
			{
				out << "  ";
				for (uint8_t i = 0; i < n->pointerDepth; ++i) out << '*';
				if (n->name.str && n->name.len) {
					out.write(n->name.str, n->name.len);
				}
				if (n->isArray) out << "[]";
				break;
			}
			case ParseKind::IntLit:
				out << "  " << Excerpt(source, n->srcStart, n->srcLen)
					<< "  (=" << n->intValue << ")";
				break;
			case ParseKind::FloatLit:
				out << "  " << Excerpt(source, n->srcStart, n->srcLen);
				break;
			case ParseKind::StringLit:
			case ParseKind::CharLit:
				out << "  " << Excerpt(source, n->srcStart, n->srcLen);
				break;
			case ParseKind::BoolLit:
				out << "  " << (n->intValue ? "true" : "false");
				break;
			case ParseKind::BinOp:
			case ParseKind::Assign:
			case ParseKind::AddressOf:
			case ParseKind::Deref:
			case ParseKind::Negate:
			case ParseKind::UnaryPlus:
			case ParseKind::Not:
			case ParseKind::BitNot:
				if (n->opToken) {
					out << "  "
						<< Lexer::TokenTypeName(T(n->opToken));
				}
				break;
			default:
				break;
			}

			out << '\n';

			for (uint32_t i = 0; i < n->childCount; ++i) {
				DumpNode(out, n->children[i], depth + 1);
			}
		}

	} // namespace

	// =====================================================================
	// Parser::Parse
	// =====================================================================
	void Parser::Parse(
		Xmo& xmo,
		std::string_view                source,
		PipelineQueue<Lexer::Token>& tokQueue,
		PipelineQueue<ParseTreeNode*>& nodeQueue,
		SymbolTable& symbols,
		const CompileJob& job)
	{
		ParserState st(xmo, source, tokQueue, nodeQueue, symbols, job);

		ParseTreeNode* root = st.ParseFile();
		xmo.parseTree = root;

		// Drain any tokens remaining after the root parse (would only
		// happen on a recovery path that left tokens in front of EOF).
		while (st.Peek().type != Lexer::TokenType::TOK_EOF) {
			st.Eat();
		}
		// Consume the EOF itself.
		st.Eat();

		// Write the parser log if requested.
		if (job.ParserLog) {
			st.WriteParserLog(root);
		}

		// End-of-stream sentinel for the Morpher. Required: dropping
		// this would deadlock the Morpher's join in RunFilePipeline.
		nodeQueue.Enqueue(nullptr);
	}

} // namespace xmc