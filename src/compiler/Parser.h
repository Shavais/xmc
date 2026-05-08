// compiler/Parser.h
//
// Parse-tree definitions and the per-file Parser entry point.
//
// The parse tree is the spine of every later stage:
//   - the Morpher reads/writes baseType and the rmask fields,
//   - the Reviewer decorates payload with parameter ids,
//   - the Coder fills codeBlocks,
//   - the Emitter walks codeBlocks and writes COFF.
//
// Per Under_The_Hood ?2a, ParseTreeNode lives in the per-file Xmo::arena
// and is never freed individually. Fields are plain (non-atomic) because
// per-file morphing is single-threaded.
//
#pragma once

#include <cstdint>
#include <string_view>

#include "Lexer.h"
#include "PipelineQueue.h"
#include "SymbolTable.h"
#include "Xmo.h"
#include "../Compiler.h"

namespace xmc
{
	// =====================================================================
	// ParseKind
	//
	// Tag identifying the construct each ParseTreeNode represents. Three
	// shape families per Under_The_Hood ?6.1:
	//
	//   nouns       - leaves; hold values (literals, identifiers).
	//   verbs       - produce values (calls, member access, operators,
	//                 unary @ and $, address-of and dereference).
	//   conjunctions- structure flow (functions, blocks, if/while/return,
	//                 etc.). Hello.xm exercises only File/ExternDecl/
	//                 FuncDecl/Block/VarDecl/ExprStmt/Return.
	//
	// New kinds are appended; do not reorder existing values, because
	// payload-shape decisions are keyed off this enum.
	// =====================================================================
	enum class ParseKind : uint16_t
	{
		Unknown = 0,

		// ----- Top-level / structural ----------------------------------
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

		// ----- Verbs ---------------------------------------------------
		Assign,         // a = b      (statement-flavored; left is lvalue)
		Call,           // child[0] = callee, then ArgList
		MemberAccess,   // obj.member  -> child[0] is obj; member name in payload
		Subscript,      // a[i]        -> children = [a, i]
		AddressOf,      // @x or &x    -> child[0] = operand
		Deref,          // $x          -> child[0] = operand
		Negate,         // -x
		UnaryPlus,      // +x
		Not,            // !x
		BitNot,         // ~x   (only when ~ appears in expression context)
		BinOp,          // child[0] op child[1]; opToken says which op

		// ----- Nouns ---------------------------------------------------
		Ident,          // identifier reference
		IntLit,         // integer literal
		FloatLit,       // floating-point literal
		StringLit,      // "..." ; raw text via srcStart/srcLen
		CharLit,        // '.' ; charValue in payload
		BoolLit,        // true | false
		NullLit,        // null

		// ----- Forward-reference placeholder ---------------------------
		Stub,           // see Under_The_Hood ?2d
	};

	// Human-readable name for parser-log output.
	const char* ParseKindName(ParseKind k);

	// =====================================================================
	// ParseTreeNode
	//
	// One node in the parse tree. Allocated from Xmo::arena; never freed
	// individually. children is an arena-allocated array of childCount
	// child pointers. Parent points up the tree.
	//
	// The struct is laid out for arena packing rather than for ABI stability;
	// add new fields at the bottom and accept the size growth.
	// =====================================================================
	struct ParseTreeNode
	{
		ParseKind       kind = ParseKind::Unknown;
		uint16_t        opToken = 0;        // Lexer::TokenType value for BinOp / Assign

		ParseTreeNode* parent = nullptr;
		ParseTreeNode** children = nullptr;
		uint32_t        childCount = 0;

		// Resolution state. The Parser leaves baseType at 0 (Unresolved)
		// for everything except literals whose type is obvious from the
		// token; the Morpher fills the rest.
		uint32_t        baseType = 0;
		uint8_t         minrmask = 0;
		uint8_t         maxrmask = 0xFF;

		// For declaration nodes (FuncDecl / Param / VarDecl / ExternDecl),
		// the Symbol declared. nullptr for expression / literal nodes.
		Symbol* declaredSymbol = nullptr;

		// Source location of the *principal* token of this node (for
		// error messages and the parser log). srcLen covers just that
		// token, not the whole subtree's span.
		uint32_t        srcStart = 0;
		uint32_t        srcLen = 0;
		uint32_t        line = 0;
		uint32_t        col = 0;

		// --- Payload ----------------------------------------------------
		//
		// Most of the payload is interpreted per ParseKind:
		//
		//   Ident, MemberAccess, FuncDecl, Param, VarDecl, ExternDecl,
		//   Type:                 `name` carries the principal identifier
		//                         (member name for MemberAccess; type
		//                         spelling for Type when it's a named
		//                         type rather than a primitive keyword).
		//   ExternDecl, FuncDecl: `qualifier` carries the namespace
		//                         qualifier (e.g. "kernel32" in
		//                         "kernel32:WriteFile"); empty if none.
		//   Param, ReturnSpec:    `regHint` carries the register name
		//                         after `:` (e.g. "rcx", "rax"); empty
		//                         if no register clause was given.
		//   IntLit:               `intValue` is the parsed integer.
		//   FloatLit:             `floatValue` is the parsed double.
		//   CharLit:              `intValue` low bits hold the char.
		//   Type:                 `pointerDepth` counts leading '*';
		//                         `isArray` true for trailing "[]";
		//                         primitive types use baseType; named
		//                         types use `name`.
		//
		// Declaring all fields here keeps each node 1-2 cache lines
		// rather than playing union games. Worth it.
		// ---------------------------------------------------------------
		InternedString  name;            // primary identifier
		InternedString  qualifier;       // namespace prefix (extern decls)
		InternedString  regHint;         // register clause (Param / ReturnSpec)

		uint64_t        intValue = 0;    // IntLit / CharLit
		double          floatValue = 0;  // FloatLit

		uint8_t         pointerDepth = 0; // Type: leading '*' count
		bool            isArray = false;  // Type: trailing "[]"

		// Coder fills this; Emitter consumes. Empty until the Coder runs.
		// We reserve the field at the parse-tree level so later stages
		// don't need to side-table it. Stored as an arena-allocated
		// pointer + count to avoid a std::vector per node.
		uint16_t* codeBlocks = nullptr;
		uint32_t        codeBlockCount = 0;
	};

	// =====================================================================
	// Parser
	//
	// One entry point per source file. Runs as the second jthread in the
	// Lex / Parse / Morph pipeline (see Compiler.cpp::RunFilePipeline).
	//
	// Pipeline contract:
	//   - Drains tokQueue one Token at a time until TOK_EOF.
	//   - Builds the parse tree in xmo.arena, with the root attached at
	//     xmo.parseTree.
	//   - Pushes each completed ParseTreeNode* onto nodeQueue (post-order:
	//     children precede their parent), so the Morpher can begin
	//     resolving leaves while later levels are still being built.
	//   - After TOK_EOF and the root has been pushed, pushes a single
	//     nullptr sentinel onto nodeQueue and returns. Both halves are
	//     required: dropping the drain stalls the Lexer; dropping the
	//     sentinel deadlocks the Morpher's join.
	//
	// On parse errors the Parser sets job.ErrorOccurred, emits a
	// diagnostic to oserror, attempts to recover to the next likely
	// statement boundary, and continues so downstream stages still see
	// a complete (if partially BaseType::Error-tainted) tree.
	// =====================================================================
	class Parser
	{
	public:
		static void Parse(
			Xmo& xmo,
			std::string_view                source,
			PipelineQueue<Lexer::Token>& tokQueue,
			PipelineQueue<ParseTreeNode*>& nodeQueue,
			SymbolTable& symbols,
			const CompileJob& job);
	};

} // namespace xmc