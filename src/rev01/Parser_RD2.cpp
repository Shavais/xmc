// compiler/Parser.cpp
//
// Pratt (top-down operator precedence) parser for .xm sources.
// See Parser.h for the pipeline contract.
//
// Structural constructs (file, declarations, blocks, statements) use
// conventional recursive-descent functions.  Operator expressions use a
// Pratt loop so precedence levels never need their own call frames.
//
// Grammar handled here is the subset hello.xm exercises:
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
//   expr        := Pratt expression (see ParseExpr / ParseNud / ParseLed)
//   prefix ops  := '@' '&' '$' '-' '+' '!' '~'
//   postfix ops := '.' identifier | '[' expr ']' | '(' arg_list? ')'
//   infix ops   := all binary operators including '=' (right-associative)
//   arg_list    := expr (',' expr)*
//
// `&` as a unary prefix is accepted as address-of for compatibility with
// hello.xm even though Section 8b of the language spec uses `@`.  Both
// fold into the same ParseKind::AddressOf node.
//
#include "pch/pch.h"
#include "Parser.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tool/Logger.h"
#include "tool/StringFunctions.h"

namespace fs = std::filesystem;

namespace xmc
{
	// =====================================================================
	// ParseKindName
	// =====================================================================
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

		// =================================================================
		// Token classification helpers
		// =================================================================

		bool IsPrimitiveTypeKw(T tt)
		{
			switch (tt) {
			case T::KW_B:
			case T::KW_I8:  case T::KW_I16: case T::KW_I32: case T::KW_I64:
			case T::KW_U8:  case T::KW_U16: case T::KW_U32: case T::KW_U64:
			case T::KW_F32: case T::KW_F64:
			case T::KW_D16: case T::KW_D32: case T::KW_D64:
			case T::KW_C8:  case T::KW_C16:
			case T::KW_ZS8: case T::KW_ZS16: case T::KW_ZS32:
			case T::KW_S8:  case T::KW_S16:
			case T::KW_UTF8: case T::KW_US:
				return true;
			default:
				return false;
			}
		}

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
		// Handles 0x (hex), 0b (binary), and base-10.
		uint64_t ParseIntLiteral(std::string_view text)
		{
			if (text.size() >= 2 && text[0] == '0' &&
				(text[1] == 'x' || text[1] == 'X'))
			{
				uint64_t v = 0;
				for (size_t i = 2; i < text.size(); ++i) {
					char c = text[i];
					uint64_t d = 0;
					if      (c >= '0' && c <= '9') d = uint64_t(c - '0');
					else if (c >= 'a' && c <= 'f') d = 10 + uint64_t(c - 'a');
					else if (c >= 'A' && c <= 'F') d = 10 + uint64_t(c - 'A');
					else continue;
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
					if (c == '0' || c == '1')
						v = (v << 1) | uint64_t(c - '0');
				}
				return v;
			}
			uint64_t v = 0;
			for (char c : text) {
				if (c >= '0' && c <= '9')
					v = v * 10 + uint64_t(c - '0');
			}
			return v;
		}

		// =================================================================
		// Pratt expression precedence levels
		//
		// Postfix operators (.  [  () bind tightest and are left-
		// associative.  Assignment operators bind loosest and are right-
		// associative (ParseLed uses PREC_ASSIGNMENT-1 for their RHS).
		// All other binary operators are left-associative (ParseLed uses
		// the operator's own level for its RHS, stopping at equal prec).
		// =================================================================
		enum Prec : int {
			PREC_NONE           = 0,
			PREC_ASSIGNMENT     = 1,   // =  :=  +=  -=  *=  /=  ...
			PREC_LOGICAL_OR     = 2,   // ||
			PREC_LOGICAL_AND    = 3,   // &&
			PREC_BIT_OR         = 4,   // |
			PREC_BIT_XOR        = 5,   // ^
			PREC_BIT_AND        = 6,   // &
			PREC_EQUALITY       = 7,   // ==  !=
			PREC_RELATIONAL     = 8,   // <  >  <=  >=
			PREC_SHIFT          = 9,   // <<  >>
			PREC_ADDITIVE       = 10,  // +  -
			PREC_MULTIPLICATIVE = 11,  // *  /  %
			PREC_POSTFIX        = 12,  // .  [  (
		};

		int ExprPrecedence(T tt)
		{
			switch (tt) {
			case T::OP_ASSIGN:
			case T::OP_ASSIGN_REF:
			case T::OP_PLUS_ASSIGN:
			case T::OP_MINUS_ASSIGN:
			case T::OP_STAR_ASSIGN:
			case T::OP_SLASH_ASSIGN:
			case T::OP_PERCENT_ASSIGN:
			case T::OP_AMP_ASSIGN:
			case T::OP_PIPE_ASSIGN:
			case T::OP_CARET_ASSIGN:
			case T::OP_LSHIFT_ASSIGN:
			case T::OP_RSHIFT_ASSIGN:  return PREC_ASSIGNMENT;
			case T::OP_OR:             return PREC_LOGICAL_OR;
			case T::OP_AND:            return PREC_LOGICAL_AND;
			case T::OP_PIPE:           return PREC_BIT_OR;
			case T::OP_CARET:          return PREC_BIT_XOR;
			case T::OP_AMP:            return PREC_BIT_AND;
			case T::OP_EQ:
			case T::OP_NEQ:            return PREC_EQUALITY;
			case T::OP_LT:
			case T::OP_GT:
			case T::OP_LTE:
			case T::OP_GTE:            return PREC_RELATIONAL;
			case T::OP_LSHIFT:
			case T::OP_RSHIFT:         return PREC_SHIFT;
			case T::OP_PLUS:
			case T::OP_MINUS:          return PREC_ADDITIVE;
			case T::OP_STAR:
			case T::OP_SLASH:
			case T::OP_PERCENT:        return PREC_MULTIPLICATIVE;
			case T::PUNCT_DOT:
			case T::PUNCT_LBRACKET:
			case T::PUNCT_LPAREN:      return PREC_POSTFIX;
			default:                   return PREC_NONE;
			}
		}

		// =================================================================
		// ParserState
		//
		// Per-file parse state, stack-allocated in Parser::Parse.
		// Holds the token cursor (2-token lookahead over the blocking
		// PipelineQueue), source view for lexeme extraction, and
		// references to the arena, symbol table, and job.
		// =================================================================
		struct ParserState
		{
			Xmo&                           xmo;
			std::string_view               source;
			PipelineQueue<Lexer::Token>&   tokQueue;
			PipelineQueue<ParseTreeNode*>& nodeQueue;
			SymbolTable&                   symbols;
			const CompileJob&              job;

			// 2-token lookahead buffer.  PipelineQueue is destructive-dequeue
			// only; parking tokens here gives non-destructive Peek semantics.
			Lexer::Token lookahead[2];
			bool         haveLook[2] = { false, false };

			// Every node allocated this parse, in tree order.  Used for the
			// parser log and future orphan-detection assertions.
			std::vector<ParseTreeNode*> allNodes;

			ParserState(Xmo& x, std::string_view src,
				PipelineQueue<Lexer::Token>& tq,
				PipelineQueue<ParseTreeNode*>& nq,
				SymbolTable& syms, const CompileJob& j)
				: xmo(x), source(src), tokQueue(tq), nodeQueue(nq),
				  symbols(syms), job(j) {}

			// ---- Token cursor -------------------------------------------

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

			Lexer::Token Eat()
			{
				Lexer::Token t;
				if (haveLook[0]) {
					t = lookahead[0];
					if (haveLook[1]) {
						lookahead[0] = lookahead[1];
						haveLook[1] = false;
					} else {
						haveLook[0] = false;
					}
					return t;
				}
				tokQueue.WaitDequeue(t);
				return t;
			}

			bool EatIf(T tt)
			{
				if (Peek().type == tt) { Eat(); return true; }
				return false;
			}

			Lexer::Token Expect(T tt, const char* what)
			{
				const Lexer::Token& nxt = Peek();
				if (nxt.type == tt) return Eat();
				ReportError(nxt, std::string("expected ") + what +
					", got " + Lexer::TokenTypeName(nxt.type));
				Lexer::Token fake{};
				fake.type = tt;
				fake.line = nxt.line;
				fake.col  = nxt.col;
				return fake;
			}

			// ---- Diagnostics --------------------------------------------

			void ReportError(const Lexer::Token& at, const std::string& msg)
			{
				oserror << xmo.name << ":" << at.line << ":" << at.col
				        << ": parse error: " << msg << std::endl;
				job.ErrorOccurred.store(true, std::memory_order_relaxed);
			}

			// ---- Node construction --------------------------------------

			ParseTreeNode* NewNode(ParseKind k, const Lexer::Token& at)
			{
				ParseTreeNode* n = xmo.arena.Construct<ParseTreeNode>();
				n->kind     = k;
				n->srcStart = at.srcStart;
				n->srcLen   = at.srcLen;
				n->line     = at.line;
				n->col      = at.col;
				allNodes.push_back(n);
				return n;
			}

			// Wire the children array, set parent pointers, and enqueue
			// leaf nodes (childCount == 0) to the Morpher's input queue.
			// Leaves are enqueued here -- after their parent pointer is
			// set -- so the Morpher can walk up from any leaf to reach
			// every ancestor without needing inner nodes enqueued separately.
			void AttachChildren(ParseTreeNode* parent,
				const std::vector<ParseTreeNode*>& kids)
			{
				if (kids.empty()) {
					parent->children   = nullptr;
					parent->childCount = 0;
					return;
				}
				ParseTreeNode** arr = static_cast<ParseTreeNode**>(
					xmo.arena.Allocate(sizeof(ParseTreeNode*) * kids.size()));
				for (size_t i = 0; i < kids.size(); ++i) {
					arr[i]         = kids[i];
					kids[i]->parent = parent;
					if (kids[i]->childCount == 0) {
						ParseTreeNode* leaf = kids[i];
						nodeQueue.Enqueue(std::move(leaf));
					}
				}
				parent->children   = arr;
				parent->childCount = static_cast<uint32_t>(kids.size());
			}

			// =============================================================
			// Grammar productions -- structural (recursive descent)
			// =============================================================

			ParseTreeNode* ParseFile();

			ParseTreeNode* ParseExternDecl();
			ParseTreeNode* ParseFuncDecl(ParseTreeNode* returnType,
			                             const Lexer::Token& nameTok);
			ParseTreeNode* ParseParam();
			ParseTreeNode* ParseReturnSpec();
			ParseTreeNode* ParseType();
			ParseTreeNode* ParseBlock();
			ParseTreeNode* ParseStmt();
			ParseTreeNode* ParseVarDecl(ParseTreeNode* type,
			                            const Lexer::Token& nameTok);
			ParseTreeNode* ParseReturnStmt();
			ParseTreeNode* ParseExprStmt();

			// =============================================================
			// Grammar productions -- expressions (Pratt)
			// =============================================================

			ParseTreeNode* ParseExpr(int minPrec = PREC_NONE);
			ParseTreeNode* ParseNud();
			ParseTreeNode* ParseLed(const Lexer::Token& op, ParseTreeNode* left);

			void RecoverToStmtBoundary();

			void WriteParserLog(ParseTreeNode* root);
			void DumpNode(std::ofstream& out, const ParseTreeNode* n, int depth);
		};

		// =================================================================
		// File / top-level declarations
		// =================================================================

		ParseTreeNode* ParserState::ParseFile()
		{
			Lexer::Token start = Peek();
			ParseTreeNode* file = NewNode(ParseKind::File, start);

			std::vector<ParseTreeNode*> decls;
			while (Peek().type != T::TOK_EOF) {
				ParseTreeNode* decl = nullptr;

				if (Peek().type == T::KW_EXTERN) {
					decl = ParseExternDecl();
				} else if (CanStartType(Peek().type)) {
					ParseTreeNode* type = ParseType();
					Lexer::Token nameTok = Expect(T::IDENTIFIER,
						"function name after return type");
					if (Peek().type == T::PUNCT_LPAREN) {
						decl = ParseFuncDecl(type, nameTok);
					} else {
						ReportError(Peek(),
							"expected '(' to begin function parameter list");
						RecoverToStmtBoundary();
						continue;
					}
				} else {
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

		// =================================================================
		// extern qual:name ( params ) -> type [: reg] ;
		// =================================================================
		ParseTreeNode* ParserState::ParseExternDecl()
		{
			Lexer::Token kw = Eat(); // KW_EXTERN
			ParseTreeNode* node = NewNode(ParseKind::ExternDecl, kw);

			Lexer::Token first = Expect(T::IDENTIFIER, "identifier after 'extern'");
			if (Peek().type == T::PUNCT_COLON) {
				Eat();
				node->qualifier = symbols.InternString(first.Text(source));
				Lexer::Token nameTok = Expect(T::IDENTIFIER, "identifier after ':'");
				node->name = symbols.InternString(nameTok.Text(source));
			} else {
				node->name = symbols.InternString(first.Text(source));
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

			if (Peek().type == T::OP_ARROW) {
				ParseTreeNode* ret = ParseReturnSpec();
				if (ret) kids.push_back(ret);
			}

			Expect(T::PUNCT_SEMICOLON, "';' to end extern declaration");
			AttachChildren(node, kids);
			return node;
		}

		// =================================================================
		// param := identifier '->' type (':' identifier)?
		// =================================================================
		ParseTreeNode* ParserState::ParseParam()
		{
			Lexer::Token nameTok = Expect(T::IDENTIFIER, "parameter name");
			ParseTreeNode* p = NewNode(ParseKind::Param, nameTok);
			p->name = symbols.InternString(nameTok.Text(source));

			Expect(T::OP_ARROW, "'->' after parameter name");

			ParseTreeNode* type = ParseType();
			std::vector<ParseTreeNode*> kids;
			if (type) kids.push_back(type);

			if (Peek().type == T::PUNCT_COLON) {
				Eat();
				Lexer::Token reg = Expect(T::IDENTIFIER, "register name after ':'");
				p->regHint = symbols.InternString(reg.Text(source));
			}

			AttachChildren(p, kids);
			return p;
		}

		// =================================================================
		// return_spec := '->' type (':' identifier)?
		// =================================================================
		ParseTreeNode* ParserState::ParseReturnSpec()
		{
			Lexer::Token arrow = Eat(); // OP_ARROW
			ParseTreeNode* node = NewNode(ParseKind::ReturnSpec, arrow);

			ParseTreeNode* type = ParseType();
			std::vector<ParseTreeNode*> kids;
			if (type) kids.push_back(type);

			if (Peek().type == T::PUNCT_COLON) {
				Eat();
				Lexer::Token reg = Expect(T::IDENTIFIER, "register name after ':'");
				node->regHint = symbols.InternString(reg.Text(source));
			}

			AttachChildren(node, kids);
			return node;
		}

		// =================================================================
		// type := '*'* simple_type ('[' ']')?
		// =================================================================
		ParseTreeNode* ParserState::ParseType()
		{
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
				node->name = symbols.InternString(core.Text(source));
			} else if (core.type == T::IDENTIFIER) {
				Eat();
				node->name = symbols.InternString(core.Text(source));
			} else if (core.type == T::KW_LIST   || core.type == T::KW_QUEUE ||
			           core.type == T::KW_STACK   || core.type == T::KW_BTREE ||
			           core.type == T::KW_HASHSET || core.type == T::KW_HASHMAP ||
			           core.type == T::KW_FUNCTION)
			{
				Eat();
				node->name = symbols.InternString(core.Text(source));
			} else {
				ReportError(core, std::string("expected a type, got ") +
					Lexer::TokenTypeName(core.type));
			}

			if (Peek().type == T::PUNCT_LBRACKET) {
				Lexer::Token lb = Peek(); Eat();
				if (Peek().type == T::PUNCT_RBRACKET) {
					Eat();
					node->isArray = true;
				} else {
					ReportError(lb, "expected ']' after '[' in type");
				}
			}

			return node;
		}

		// =================================================================
		// func_decl := type identifier '(' params? ')' block
		// =================================================================
		ParseTreeNode* ParserState::ParseFuncDecl(ParseTreeNode* returnType,
			const Lexer::Token& nameTok)
		{
			ParseTreeNode* fn = NewNode(ParseKind::FuncDecl, nameTok);
			fn->name = symbols.InternString(nameTok.Text(source));

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

		// =================================================================
		// block := '{' stmt* '}'
		// =================================================================
		ParseTreeNode* ParserState::ParseBlock()
		{
			Lexer::Token lb = Expect(T::PUNCT_LBRACE, "'{' to begin block");
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

		// =================================================================
		// stmt := var_decl | return_stmt | expr_stmt
		//
		// Disambiguation: `identifier identifier` => var decl with named
		// type; `primitive_kw identifier` => var decl with primitive type;
		// everything else => expression statement.
		// =================================================================
		ParseTreeNode* ParserState::ParseStmt()
		{
			if (Peek().type == T::KW_RETURN) return ParseReturnStmt();

			if (CanStartType(Peek().type)) {
				if (Peek().type == T::IDENTIFIER) {
					if (Peek(1).type == T::IDENTIFIER) {
						ParseTreeNode* type = ParseType();
						Lexer::Token nameTok = Expect(T::IDENTIFIER, "variable name");
						return ParseVarDecl(type, nameTok);
					}
					return ParseExprStmt();
				}
				ParseTreeNode* type = ParseType();
				Lexer::Token nameTok = Expect(T::IDENTIFIER, "variable name after type");
				return ParseVarDecl(type, nameTok);
			}

			return ParseExprStmt();
		}

		// =================================================================
		// var_decl := type identifier ('=' expr)? ';'
		// =================================================================
		ParseTreeNode* ParserState::ParseVarDecl(ParseTreeNode* type,
			const Lexer::Token& nameTok)
		{
			ParseTreeNode* decl = NewNode(ParseKind::VarDecl, nameTok);
			decl->name = symbols.InternString(nameTok.Text(source));

			std::vector<ParseTreeNode*> kids;
			if (type) kids.push_back(type);

			if (Peek().type == T::OP_ASSIGN) {
				Eat();
				ParseTreeNode* init = ParseExpr(PREC_NONE);
				if (init) kids.push_back(init);
			}

			Expect(T::PUNCT_SEMICOLON, "';' to end variable declaration");
			AttachChildren(decl, kids);
			return decl;
		}

		// =================================================================
		// return_stmt := 'return' expr? ';'
		// =================================================================
		ParseTreeNode* ParserState::ParseReturnStmt()
		{
			Lexer::Token kw = Eat(); // KW_RETURN
			ParseTreeNode* ret = NewNode(ParseKind::Return, kw);

			std::vector<ParseTreeNode*> kids;
			if (Peek().type != T::PUNCT_SEMICOLON) {
				ParseTreeNode* e = ParseExpr(PREC_NONE);
				if (e) kids.push_back(e);
			}
			Expect(T::PUNCT_SEMICOLON, "';' after return");
			AttachChildren(ret, kids);
			return ret;
		}

		// =================================================================
		// expr_stmt := expr ';'
		//
		// Assignment (a = b) is now handled inside the Pratt expression
		// parser as a right-associative infix operator, so ParseExprStmt
		// simply calls ParseExpr and wraps the result.
		// =================================================================
		ParseTreeNode* ParserState::ParseExprStmt()
		{
			Lexer::Token start = Peek();
			ParseTreeNode* expr = ParseExpr(PREC_NONE);

			Expect(T::PUNCT_SEMICOLON, "';' to end statement");

			ParseTreeNode* stmt = NewNode(ParseKind::ExprStmt, start);
			std::vector<ParseTreeNode*> kids;
			if (expr) kids.push_back(expr);
			AttachChildren(stmt, kids);
			return stmt;
		}

		// =================================================================
		// Pratt expression parser
		//
		// ParseExpr(minPrec):
		//   1. ParseNud() handles the current token as a prefix/atom,
		//      consuming as many tokens as needed.
		//   2. While the next token's precedence exceeds minPrec, consume
		//      it and call ParseLed(op, left) to extend the expression.
		//
		// Operator associativity is encoded in ParseLed's minPrec choice
		// for the RHS:
		//   - Left-associative (all infix except assignment):
		//       RHS = ParseExpr(op.prec)   -- same level stops recursion
		//   - Right-associative (assignment operators):
		//       RHS = ParseExpr(op.prec-1) -- same level continues recursion
		//   - Postfix (. [ ():
		//       These consume their own tokens directly in ParseLed without
		//       a recursive ParseExpr call at the operator's level.
		//
		// Prefix unary operators are handled in ParseNud.  They parse their
		// operand at PREC_MULTIPLICATIVE so that postfix operators bind to
		// the operand but binary operators do not: -a[0] is -(a[0]) and
		// -a + b is (-a) + b.  Chained prefix ops work naturally because
		// each ParseNud calls ParseExpr, which calls ParseNud again.
		// =================================================================

		ParseTreeNode* ParserState::ParseExpr(int minPrec)
		{
			ParseTreeNode* left = ParseNud();

			while (ExprPrecedence(Peek().type) > minPrec) {
				Lexer::Token op = Eat();
				left = ParseLed(op, left);
			}

			return left;
		}

		ParseTreeNode* ParserState::ParseNud()
		{
			Lexer::Token tk = Eat();

			switch (tk.type) {

			// ---- Noun: literals -----------------------------------------
			case T::LIT_INTEGER: {
				ParseTreeNode* n = NewNode(ParseKind::IntLit, tk);
				n->intValue = ParseIntLiteral(tk.Text(source));
				return n;
			}
			case T::LIT_FLOAT:
			case T::LIT_DECIMAL: {
				// Raw bytes kept in srcStart/srcLen; Morpher resolves value.
				return NewNode(ParseKind::FloatLit, tk);
			}
			case T::LIT_STRING: {
				return NewNode(ParseKind::StringLit, tk);
			}
			case T::LIT_CHAR: {
				ParseTreeNode* n = NewNode(ParseKind::CharLit, tk);
				std::string_view raw = tk.Text(source);
				if (raw.size() >= 3) n->intValue = uint8_t(raw[1]);
				return n;
			}
			case T::KW_TRUE:
			case T::KW_FALSE: {
				ParseTreeNode* n = NewNode(ParseKind::BoolLit, tk);
				n->intValue = (tk.type == T::KW_TRUE) ? 1 : 0;
				return n;
			}
			case T::KW_NULL: {
				return NewNode(ParseKind::NullLit, tk);
			}

			// ---- Noun: identifier ----------------------------------------
			case T::IDENTIFIER: {
				ParseTreeNode* n = NewNode(ParseKind::Ident, tk);
				n->name = symbols.InternString(tk.Text(source));
				return n;
			}

			// ---- Grouped expression --------------------------------------
			case T::PUNCT_LPAREN: {
				ParseTreeNode* inner = ParseExpr(PREC_NONE);
				Expect(T::PUNCT_RPAREN, "')' to close parenthesized expression");
				return inner;
			}

			// ---- Prefix unary operators (verbs) -------------------------
			case T::OP_AT:
			case T::OP_AMP: {
				// Both @ and & mean address-of in prefix position.
				ParseTreeNode* n = NewNode(ParseKind::AddressOf, tk);
				n->opToken = uint16_t(tk.type);
				ParseTreeNode* operand = ParseExpr(PREC_MULTIPLICATIVE);
				std::vector<ParseTreeNode*> kids;
				if (operand) kids.push_back(operand);
				AttachChildren(n, kids);
				return n;
			}
			case T::OP_DOLLAR: {
				ParseTreeNode* n = NewNode(ParseKind::Deref, tk);
				n->opToken = uint16_t(tk.type);
				ParseTreeNode* operand = ParseExpr(PREC_MULTIPLICATIVE);
				std::vector<ParseTreeNode*> kids;
				if (operand) kids.push_back(operand);
				AttachChildren(n, kids);
				return n;
			}
			case T::OP_MINUS: {
				ParseTreeNode* n = NewNode(ParseKind::Negate, tk);
				n->opToken = uint16_t(tk.type);
				ParseTreeNode* operand = ParseExpr(PREC_MULTIPLICATIVE);
				std::vector<ParseTreeNode*> kids;
				if (operand) kids.push_back(operand);
				AttachChildren(n, kids);
				return n;
			}
			case T::OP_PLUS: {
				ParseTreeNode* n = NewNode(ParseKind::UnaryPlus, tk);
				n->opToken = uint16_t(tk.type);
				ParseTreeNode* operand = ParseExpr(PREC_MULTIPLICATIVE);
				std::vector<ParseTreeNode*> kids;
				if (operand) kids.push_back(operand);
				AttachChildren(n, kids);
				return n;
			}
			case T::OP_BANG: {
				ParseTreeNode* n = NewNode(ParseKind::Not, tk);
				n->opToken = uint16_t(tk.type);
				ParseTreeNode* operand = ParseExpr(PREC_MULTIPLICATIVE);
				std::vector<ParseTreeNode*> kids;
				if (operand) kids.push_back(operand);
				AttachChildren(n, kids);
				return n;
			}
			case T::OP_TILDE: {
				ParseTreeNode* n = NewNode(ParseKind::BitNot, tk);
				n->opToken = uint16_t(tk.type);
				ParseTreeNode* operand = ParseExpr(PREC_MULTIPLICATIVE);
				std::vector<ParseTreeNode*> kids;
				if (operand) kids.push_back(operand);
				AttachChildren(n, kids);
				return n;
			}

			default: {
				ReportError(tk, std::string("expected expression, got ") +
					Lexer::TokenTypeName(tk.type));
				// Synthesize a NullLit so the caller has something to attach.
				return NewNode(ParseKind::NullLit, tk);
			}
			}
		}

		ParseTreeNode* ParserState::ParseLed(const Lexer::Token& op,
			ParseTreeNode* left)
		{
			switch (op.type) {

			// ---- Postfix: member access ----------------------------------
			case T::PUNCT_DOT: {
				Lexer::Token mem = Expect(T::IDENTIFIER, "member name after '.'");
				ParseTreeNode* n = NewNode(ParseKind::MemberAccess, op);
				n->name = symbols.InternString(mem.Text(source));
				std::vector<ParseTreeNode*> kids;
				if (left) kids.push_back(left);
				AttachChildren(n, kids);
				return n;
			}

			// ---- Postfix: subscript --------------------------------------
			case T::PUNCT_LBRACKET: {
				ParseTreeNode* idx = ParseExpr(PREC_NONE);
				Expect(T::PUNCT_RBRACKET, "']' to close subscript");
				ParseTreeNode* n = NewNode(ParseKind::Subscript, op);
				std::vector<ParseTreeNode*> kids;
				if (left) kids.push_back(left);
				if (idx)  kids.push_back(idx);
				AttachChildren(n, kids);
				return n;
			}

			// ---- Postfix: function call ----------------------------------
			case T::PUNCT_LPAREN: {
				ParseTreeNode* argList = NewNode(ParseKind::ArgList, op);
				std::vector<ParseTreeNode*> args;
				if (Peek().type != T::PUNCT_RPAREN) {
					while (true) {
						ParseTreeNode* a = ParseExpr(PREC_NONE);
						if (a) args.push_back(a);
						if (Peek().type == T::PUNCT_COMMA) { Eat(); continue; }
						break;
					}
				}
				Expect(T::PUNCT_RPAREN, "')' to close argument list");
				AttachChildren(argList, args);

				ParseTreeNode* call = NewNode(ParseKind::Call, op);
				std::vector<ParseTreeNode*> kids;
				if (left) kids.push_back(left);
				kids.push_back(argList);
				AttachChildren(call, kids);
				return call;
			}

			// ---- Assignment operators (right-associative) ---------------
			case T::OP_ASSIGN:
			case T::OP_ASSIGN_REF:
			case T::OP_PLUS_ASSIGN:
			case T::OP_MINUS_ASSIGN:
			case T::OP_STAR_ASSIGN:
			case T::OP_SLASH_ASSIGN:
			case T::OP_PERCENT_ASSIGN:
			case T::OP_AMP_ASSIGN:
			case T::OP_PIPE_ASSIGN:
			case T::OP_CARET_ASSIGN:
			case T::OP_LSHIFT_ASSIGN:
			case T::OP_RSHIFT_ASSIGN: {
				ParseTreeNode* rhs = ParseExpr(PREC_ASSIGNMENT - 1);
				ParseTreeNode* n = NewNode(ParseKind::Assign, op);
				n->opToken = uint16_t(op.type);
				std::vector<ParseTreeNode*> kids;
				if (left) kids.push_back(left);
				if (rhs)  kids.push_back(rhs);
				AttachChildren(n, kids);
				return n;
			}

			// ---- All other binary operators (left-associative) ----------
			default: {
				ParseTreeNode* rhs = ParseExpr(ExprPrecedence(op.type));
				ParseTreeNode* n = NewNode(ParseKind::BinOp, op);
				n->opToken = uint16_t(op.type);
				std::vector<ParseTreeNode*> kids;
				if (left) kids.push_back(left);
				if (rhs)  kids.push_back(rhs);
				AttachChildren(n, kids);
				return n;
			}
			}
		}

		// =================================================================
		// Error recovery
		// =================================================================
		void ParserState::RecoverToStmtBoundary()
		{
			while (true) {
				T tt = Peek().type;
				if (tt == T::TOK_EOF)         return;
				if (tt == T::PUNCT_SEMICOLON) { Eat(); return; }
				if (tt == T::PUNCT_RBRACE)    return;
				if (tt == T::KW_EXTERN)       return;
				Eat();
			}
		}

		// =================================================================
		// Parser log
		//
		// Sibling file alongside the source, named <stem>.parser.txt.
		// Format mirrors the lexer log: filename:line:col padded to 24
		// characters, then two spaces of indent per depth level, then
		// the ParseKind name and any interesting per-kind payload.
		//
		// sformat() from StringFunctions builds the position prefix.
		// =================================================================
		void ParserState::WriteParserLog(ParseTreeNode* root)
		{
			fs::path src(xmo.name);
			fs::path logPath = src.parent_path() /
				(src.stem().string() + ".parser.txt");
			std::ofstream out(logPath);
			if (!out) return;
			DumpNode(out, root, 0);
		}

		// Returns up to `len` bytes from source starting at `start`,
		// with newlines and tabs collapsed to spaces for log readability.
		std::string Excerpt(std::string_view src, uint32_t start, uint32_t len)
		{
			if (start >= src.size()) return {};
			uint32_t end = start + len;
			if (end > uint32_t(src.size())) end = uint32_t(src.size());
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

			std::string pos = sformat("%s:%u:%u",
				xmo.name.c_str(), n->line, n->col);
			out.write(pos.data(), std::streamsize(pos.size()));
			for (size_t pad = pos.size(); pad < 24; ++pad) out.put(' ');

			for (int i = 0; i < depth; ++i) out.put(' '), out.put(' ');

			out << ParseKindName(n->kind);

			switch (n->kind) {
			case ParseKind::ExternDecl:
			case ParseKind::FuncDecl:
			case ParseKind::Param:
			case ParseKind::VarDecl:
			case ParseKind::Ident:
			case ParseKind::MemberAccess:
			{
				out << "  ";
				if (n->qualifier.str && n->qualifier.len)
					out.write(n->qualifier.str, n->qualifier.len), out << ":";
				if (n->name.str && n->name.len)
					out.write(n->name.str, n->name.len);
				if (n->kind == ParseKind::Param &&
				    n->regHint.str && n->regHint.len)
				{
					out << " : ";
					out.write(n->regHint.str, n->regHint.len);
				}
				break;
			}
			case ParseKind::ReturnSpec:
				if (n->regHint.str && n->regHint.len) {
					out << "  : ";
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
				if (n->opToken)
					out << "  " << Lexer::TokenTypeName(T(n->opToken));
				break;
			default:
				break;
			}

			out << '\n';

			for (uint32_t i = 0; i < n->childCount; ++i)
				DumpNode(out, n->children[i], depth + 1);
		}

	} // namespace

	// =====================================================================
	// Parser::Parse
	// =====================================================================
	void Parser::Parse(
		Xmo& xmo,
		std::string_view               source,
		PipelineQueue<Lexer::Token>&   tokQueue,
		PipelineQueue<ParseTreeNode*>& nodeQueue,
		SymbolTable&                   symbols,
		const CompileJob&              job)
	{
		ParserState st(xmo, source, tokQueue, nodeQueue, symbols, job);

		ParseTreeNode* root = st.ParseFile();
		xmo.parseTree = root;

		// Drain any tokens left by error recovery before EOF.
		while (st.Peek().type != Lexer::TokenType::TOK_EOF)
			st.Eat();
		st.Eat(); // consume the EOF itself

		if (job.ParserLog)
			st.WriteParserLog(root);

		// nullptr sentinel tells the Morpher its input stream is done.
		nodeQueue.Enqueue(nullptr);
	}

} // namespace xmc
