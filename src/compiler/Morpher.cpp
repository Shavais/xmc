// compiler/Morpher.cpp
//
// First-pass Morpher: pure analysis. See Morpher.h for contract.
//
// What this implements (the minimum hello.xm needs):
//
//   - Top-down recursive walk of xmo.parseTree.
//   - Scope-id allocation: one scope per File / ExternDecl / FuncDecl
//     parameter list / Block. Nested scopes record the full chain on
//     each Symbol's path, so SymbolTable::ResolveSymbol's outer-walk
//     finds the right binding.
//   - Symbol declaration for ExternDecl / FuncDecl / Param / VarDecl.
//     The declaring node's `declaredSymbol` back-pointer is set; the
//     Symbol's `baseType`, `minrmask`, `maxrmask`, and `allocType`
//     are populated. `xmo.ownedSymbols` collects the back-references
//     so the rest of the pipeline can iterate them.
//   - Ident resolution against the current scope chain. The resolved
//     Symbol is stored on the Ident node (via the `declaredSymbol`
//     field, reused as a "the Symbol associated with this node"
//     pointer) and the Ident's baseType / pointerDepth / isArray are
//     copied from the Symbol's declaring node.
//   - baseType propagation up the expression tree: literals get a
//     baseType from their kind; member access / address-of / deref
//     compute their baseType from the operand; calls take their
//     baseType from the callee's return spec.
//
// What this does NOT implement (deferred until Reviewer / Coder show
// they need it):
//
//   - True streaming morphing. Morph drains the leaf queue and then
//     runs the same MorphTree pass MorphTree would, so a downstream
//     stage can rely on identical results from either entry.
//   - rmask propagation beyond defaults. hello.xm doesn't exercise
//     refinement morphing (no := assignments, no shared containers,
//     no concurrency). When a real test introduces them, this is
//     where the propagation rules get added.
//   - Conversion-node insertion. Per §6.1 that's the Reviewer's job;
//     we just leave operand types as resolved and let the Reviewer
//     deal with mismatches.
//   - Cross-file subscriber bookkeeping. hello.xm is one file; until
//     we have a multi-file test, the subscriber index hooks stay out.
//
#include "pch/pch.h"
#include "Morpher.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "tool/Logger.h"
#include "Types.h"

namespace fs = std::filesystem;

namespace xmc
{
	namespace
	{
		// BaseTypeIds is an enum class : uint8_t; ParseTreeNode::baseType
		// and Symbol::baseType both store u32. This little helper avoids
		// peppering static_casts at every assignment.
		static constexpr uint32_t U(BaseTypeIds id)
		{
			return static_cast<uint32_t>(id);
		}

		// =============================================================
		// MorpherState
		//
		// Per-file walk state. One instance per MorphTree call,
		// stack-allocated.
		// =============================================================
		struct MorpherState
		{
			Xmo& xmo;
			SymbolTable& symbols;
			const CompileJob& job;

			// Scope path: outermost (file scope) at index 0, innermost
			// at back(). Re-built by the recursive walk.
			std::vector<uint32_t> scopeStack;

			// Symbol -> declaring parse node. Lets a Call site at e.g.
			// "WriteFile(...)" find the ExternDecl that declared
			// WriteFile and read its return spec / parameter shapes.
			std::unordered_map<Symbol*, ParseTreeNode*> declSite;

			// Symbols created during this walk, in declaration order.
			// Used both for the morpher log and to populate
			// xmo.ownedSymbols (the rest of the pipeline expects the
			// xmo to enumerate its own symbols).
			std::vector<Symbol*> declared;

			MorpherState(Xmo& x, SymbolTable& s, const CompileJob& j)
				: xmo(x), symbols(s), job(j) {
			}

			// ---- Walk -------------------------------------------------

			void MorphFile(ParseTreeNode* file);
			void MorphTopDecl(ParseTreeNode* node);
			void MorphExternDecl(ParseTreeNode* node);
			void MorphFuncDecl(ParseTreeNode* node);
			void MorphParam(ParseTreeNode* node, AllocationType alloc);
			void MorphReturnSpec(ParseTreeNode* node);
			void MorphType(ParseTreeNode* node);
			void MorphBlock(ParseTreeNode* node);
			void MorphStmt(ParseTreeNode* node);
			void MorphVarDecl(ParseTreeNode* node);
			void MorphReturn(ParseTreeNode* node);
			void MorphExprStmt(ParseTreeNode* node);
			void MorphExpr(ParseTreeNode* node);

			// ---- Helpers ----------------------------------------------

			// Declares a Symbol with `name` in the innermost current
			// scope. Sets allocType, registers the back-pointer in
			// xmo.ownedSymbols, and remembers the declaring node.
			Symbol* DeclareSymbol(InternedString name,
				AllocationType alloc,
				ParseTreeNode* declNode);

			// Resolves `name` against the current scope chain
			// (innermost-first walk inside SymbolTable).
			Symbol* ResolveSymbol(InternedString name);

			// Copy baseType / pointerDepth / isArray from `src` onto
			// `dst`. Used at every "the type of this node is the type
			// of that one" decision point.
			void CopyTypeShape(ParseTreeNode* dst,
				const ParseTreeNode* src);

			// First child of `parent` whose kind matches `k`, or nullptr.
			ParseTreeNode* FirstChildOfKind(ParseTreeNode* parent, ParseKind k);

			// Allocates a new scope id, pushes it onto scopeStack, and
			// records an XmoScope in xmo.scopeTree so the persisted scope
			// tree is complete by the time the Morpher is done.
			uint32_t PushScope(ScopeTypes blockType);
			void     PopScope();

			// Diagnostic. Stream to oserror with a "filename:line:col"
			// prefix and set ErrorOccurred.
			void Report(const ParseTreeNode* at, const std::string& msg);

			// Pretty-print baseType with pointer/array decoration for
			// the morpher log.
			std::string FormatType(uint32_t baseType,
				uint8_t pointerDepth,
				bool isArray) const;

			void WriteMorpherLog();
			void DumpNode(std::ofstream& out,
				const ParseTreeNode* n, int depth);
		};

		// -----------------------------------------------------------
		// File / top-level
		// -----------------------------------------------------------
		void MorpherState::MorphFile(ParseTreeNode* file)
		{
			PushScope(ScopeTypes::File);

			for (uint32_t i = 0; i < file->childCount; ++i) {
				MorphTopDecl(file->children[i]);
			}

			PopScope();
		}

		void MorpherState::MorphTopDecl(ParseTreeNode* node)
		{
			switch (node->kind) {
			case ParseKind::ExternDecl: MorphExternDecl(node); break;
			case ParseKind::FuncDecl:   MorphFuncDecl(node);   break;
			default:
				Report(node, "unexpected top-level declaration kind");
				break;
			}
		}

		// -----------------------------------------------------------
		// extern kernel32:WriteFile(params) -> retType:retReg ;
		//
		// The qualifier ("kernel32") stays as metadata on the
		// ExternDecl node. The Symbol itself is registered under the
		// bare name in file scope so an unqualified call from xmc
		// code resolves it. Linker-side import resolution reads the
		// qualifier from the ExternDecl payload at emit time.
		// -----------------------------------------------------------
		void MorpherState::MorphExternDecl(ParseTreeNode* node)
		{
			// allocType doesn't really fit a function-import symbol --
			// the enum is for variable storage. Stack is the harmless
			// default; downstream stages key off baseType=function and
			// the ExternDecl payload, not allocType.
			Symbol* sym = DeclareSymbol(node->name,
				AllocationType::Stack, node);
			node->declaredSymbol = sym;
			node->baseType = U(BaseTypeIds::function);
			if (sym) {
				sym->baseType.store(U(BaseTypeIds::function),
					std::memory_order_relaxed);
			}

			// Open a private parameter scope for the params. Their
			// names aren't reachable from xmc code (an unqualified
			// "handle" should not collide with a local variable named
			// "handle" elsewhere), so we wrap them in their own scope.
			PushScope(ScopeTypes::Function);

			ParseTreeNode* paramList = FirstChildOfKind(node, ParseKind::ParamList);
			if (paramList) {
				for (uint32_t i = 0; i < paramList->childCount; ++i) {
					MorphParam(paramList->children[i],
						AllocationType::Stack);
				}
			}

			ParseTreeNode* ret = FirstChildOfKind(node, ParseKind::ReturnSpec);
			if (ret) MorphReturnSpec(ret);

			PopScope();
		}

		// -----------------------------------------------------------
		// retType funcName(params) { body }
		//
		// FuncDecl children: [Type, ParamList, Block]. The Type child
		// IS the return type (no ReturnSpec wrapper, unlike extern).
		// -----------------------------------------------------------
		void MorpherState::MorphFuncDecl(ParseTreeNode* node)
		{
			// As with ExternDecl, allocType is irrelevant for a function
			// symbol; default it to Stack.
			Symbol* sym = DeclareSymbol(node->name,
				AllocationType::Stack, node);
			node->declaredSymbol = sym;
			node->baseType = U(BaseTypeIds::function);
			if (sym) {
				sym->baseType.store(U(BaseTypeIds::function),
					std::memory_order_relaxed);
			}

			// Return type
			ParseTreeNode* retType = FirstChildOfKind(node, ParseKind::Type);
			if (retType) MorphType(retType);

			// Open the function scope; params + body live inside.
			PushScope(ScopeTypes::Function);

			ParseTreeNode* paramList = FirstChildOfKind(node, ParseKind::ParamList);
			if (paramList) {
				for (uint32_t i = 0; i < paramList->childCount; ++i) {
					MorphParam(paramList->children[i],
						AllocationType::Stack);
				}
			}

			ParseTreeNode* body = FirstChildOfKind(node, ParseKind::Block);
			if (body) MorphBlock(body);

			PopScope();
		}

		// -----------------------------------------------------------
		// param := identifier '->' type (':' register)?
		//
		// The Param node carries the param name and the optional
		// register hint; its sole child is the Type. We resolve the
		// type, copy its shape onto the Param node, and create a
		// Symbol in the current scope.
		// -----------------------------------------------------------
		void MorpherState::MorphParam(ParseTreeNode* node,
			AllocationType alloc)
		{
			ParseTreeNode* type = FirstChildOfKind(node, ParseKind::Type);
			if (type) {
				MorphType(type);
				CopyTypeShape(node, type);
			}

			Symbol* sym = DeclareSymbol(node->name, alloc, node);
			node->declaredSymbol = sym;
			if (sym) {
				sym->pointerDepth = node->pointerDepth;
				sym->isArray = node->isArray;
				sym->baseType.store(node->baseType,
					std::memory_order_relaxed);
			}
		}

		// -----------------------------------------------------------
		// return_spec := '->' type (':' register)?
		// -----------------------------------------------------------
		void MorpherState::MorphReturnSpec(ParseTreeNode* node)
		{
			ParseTreeNode* type = FirstChildOfKind(node, ParseKind::Type);
			if (type) {
				MorphType(type);
				CopyTypeShape(node, type);
			}
			else {
			}
		}

		// -----------------------------------------------------------
		// type := '*'* simple_type ('[' ']')?
		//
		// The Parser already split off pointerDepth and isArray. We
		// just need to map `name` (the primitive keyword spelling or
		// a user-typed identifier) to a baseType id.
		// -----------------------------------------------------------
		void MorpherState::MorphType(ParseTreeNode* node)
		{
			std::string_view kw(node->name.str, node->name.len);
			BaseTypeIds id = BaseTypeFromKeyword(kw);
			// Unresolved here means "not a primitive keyword" -- could be
			// a user-defined type. hello.xm has no user types so anything
			// not in the primitive table stays Unresolved; the Reviewer's
			// scan picks it up if it matters.
			node->baseType = U(id);
		}

		// -----------------------------------------------------------
		// block := '{' stmt* '}'
		// -----------------------------------------------------------
		void MorpherState::MorphBlock(ParseTreeNode* node)
		{
			PushScope(ScopeTypes::Nested);

			for (uint32_t i = 0; i < node->childCount; ++i) {
				MorphStmt(node->children[i]);
			}

			PopScope();
		}

		void MorpherState::MorphStmt(ParseTreeNode* node)
		{
			switch (node->kind) {
			case ParseKind::VarDecl:  MorphVarDecl(node);  break;
			case ParseKind::Return:   MorphReturn(node);   break;
			case ParseKind::ExprStmt: MorphExprStmt(node); break;
			case ParseKind::Block:    MorphBlock(node);    break;
			default:
				Report(node, std::string("unhandled statement kind: ") +
					ParseKindName(node->kind));
				break;
			}
		}

		// -----------------------------------------------------------
		// var_decl := type identifier ('=' expr)? ';'
		// -----------------------------------------------------------
		void MorpherState::MorphVarDecl(ParseTreeNode* node)
		{
			ParseTreeNode* type = FirstChildOfKind(node, ParseKind::Type);
			if (type) {
				MorphType(type);
				CopyTypeShape(node, type);
			}

			Symbol* sym = DeclareSymbol(node->name,
				AllocationType::Stack, node);
			node->declaredSymbol = sym;
			if (sym) {
				sym->pointerDepth = node->pointerDepth;
				sym->isArray = node->isArray;
				sym->baseType.store(node->baseType,
					std::memory_order_relaxed);
			}

			// Optional initializer: any non-Type child after the Type.
			for (uint32_t i = 0; i < node->childCount; ++i) {
				if (node->children[i]->kind != ParseKind::Type) {
					MorphExpr(node->children[i]);
				}
			}
		}

		void MorpherState::MorphReturn(ParseTreeNode* node)
		{
			for (uint32_t i = 0; i < node->childCount; ++i) {
				MorphExpr(node->children[i]);
			}
			// Conformance with the enclosing function's return type
			// is the Reviewer's responsibility (per §6.1.1).
		}

		void MorpherState::MorphExprStmt(ParseTreeNode* node)
		{
			for (uint32_t i = 0; i < node->childCount; ++i) {
				MorphExpr(node->children[i]);
			}
		}

		// -----------------------------------------------------------
		// Expressions. Recursive bottom-up: process children, then
		// derive this node's type from theirs.
		// -----------------------------------------------------------
		void MorpherState::MorphExpr(ParseTreeNode* node)
		{
			// Resolve children first (post-order type derivation).
			for (uint32_t i = 0; i < node->childCount; ++i) {
				MorphExpr(node->children[i]);
			}

			switch (node->kind) {
				// -- Leaves ----------------------------------------
			case ParseKind::IntLit: {
				// Smallest unsigned that fits. The Reviewer will
				// handle context-driven conversion (e.g. fitting
				// 0xFFFFFFF5 into an i32 parameter slot).
				uint64_t v = node->intValue;
				if (v <= 0xFFull)         node->baseType = U(BaseTypeIds::T_U8);
				else if (v <= 0xFFFFull)       node->baseType = U(BaseTypeIds::T_U16);
				else if (v <= 0xFFFFFFFFull)   node->baseType = U(BaseTypeIds::T_U32);
				else                            node->baseType = U(BaseTypeIds::T_U64);
				break;
			}
			case ParseKind::FloatLit:
				node->baseType = U(BaseTypeIds::T_F64);
				break;
			case ParseKind::StringLit:
				// "..." is an open array of u8 (per §9 + §28's
				// *u8[] usage on FFI parameters). Treating it as
				// u8 + isArray=true matches the spelling u8[] of
				// the variable that holds it in hello.xm.
				node->baseType = U(BaseTypeIds::T_U8);
				node->isArray = true;
				break;
			case ParseKind::CharLit:
				node->baseType = U(BaseTypeIds::c8);
				break;
			case ParseKind::BoolLit:
				node->baseType = U(BaseTypeIds::b);
				break;
			case ParseKind::NullLit:
				node->baseType = U(BaseTypeIds::Null);
				break;

			case ParseKind::Ident: {
				Symbol* sym = ResolveSymbol(node->name);
				if (!sym) {
					Report(node, "undeclared identifier: " +
						std::string(node->name.str, node->name.len));
					node->baseType = U(BaseTypeIds::Error);
					break;
				}
				// Reuse declaredSymbol as "the Symbol this node
				// is associated with" for both declarers and
				// references. Comment in Parser.h scopes the field
				// to declarers; this is a minor reinterpretation
				// the rest of the pipeline can rely on.
				node->declaredSymbol = sym;

				// Pull type shape from the declaring node so we
				// inherit pointerDepth / isArray, not just baseType.
				auto it = declSite.find(sym);
				if (it != declSite.end()) {
					CopyTypeShape(node, it->second);
				}
				else {
					node->baseType = sym->baseType.load(
						std::memory_order_relaxed);
				}
				break;
			}

								 // -- Unary / value-producing verbs -----------------
			case ParseKind::AddressOf: {
				// One child: the operand. Result is a pointer to
				// the operand's type.
				if (node->childCount > 0) {
					const ParseTreeNode* op = node->children[0];
					node->baseType = op->baseType;
					node->isArray = op->isArray;
					uint32_t depth = uint32_t(op->pointerDepth) + 1;
					node->pointerDepth = depth > 255 ? 255 : uint8_t(depth);
				}
				break;
			}
			case ParseKind::Deref: {
				if (node->childCount > 0) {
					const ParseTreeNode* op = node->children[0];
					node->baseType = op->baseType;
					node->isArray = op->isArray;
					node->pointerDepth = op->pointerDepth > 0 ?
						op->pointerDepth - 1 : 0;
				}
				break;
			}
			case ParseKind::Negate:
			case ParseKind::UnaryPlus:
			case ParseKind::Not:
			case ParseKind::BitNot: {
				if (node->childCount > 0) {
					CopyTypeShape(node, node->children[0]);
				}
				// !x conceptually returns b; for hello.xm's needs
				// we don't trip over this -- the literal/numeric
				// passthrough keeps types consistent enough for
				// the Reviewer to refine.
				break;
			}

			case ParseKind::MemberAccess: {
				// child[0] is the object; payload `name` is the
				// member spelling. The one case hello.xm exercises
				// is .length on an array, which is u64.
				if (node->childCount > 0 &&
					node->name.len == 6 &&
					std::memcmp(node->name.str, "length", 6) == 0 &&
					node->children[0]->isArray)
				{
					node->baseType = U(BaseTypeIds::T_U64);
					node->pointerDepth = 0;
					node->isArray = false;
				}
				else {
					// Fields on user types aren't resolved yet;
					// punt to Unresolved so the Reviewer's later
					// scan flags it if it matters.
					node->baseType = U(BaseTypeIds::Unresolved);
				}
				break;
			}

			case ParseKind::Subscript: {
				// arr[i]: result is the element type of arr.
				if (node->childCount > 0) {
					const ParseTreeNode* arr = node->children[0];
					node->baseType = arr->baseType;
					node->pointerDepth = arr->pointerDepth;
					node->isArray = false;
				}
				break;
			}

			case ParseKind::Call: {
				// child[0] = callee (Ident), child[1] = ArgList.
				// Take the call's baseType from the callee's
				// return spec via its declaring node.
				if (node->childCount == 0) break;
				const ParseTreeNode* callee = node->children[0];
				Symbol* sym = callee->declaredSymbol;
				if (!sym) {
					node->baseType = U(BaseTypeIds::Unresolved);
					break;
				}
				auto it = declSite.find(sym);
				if (it == declSite.end()) {
					node->baseType = U(BaseTypeIds::Unresolved);
					break;
				}
				ParseTreeNode* decl = it->second;
				ParseTreeNode* retCarrier = nullptr;
				if (decl->kind == ParseKind::ExternDecl) {
					retCarrier = FirstChildOfKind(decl,
						ParseKind::ReturnSpec);
				}
				else if (decl->kind == ParseKind::FuncDecl) {
					retCarrier = FirstChildOfKind(decl,
						ParseKind::Type);
				}
				if (retCarrier) {
					CopyTypeShape(node, retCarrier);
				}
				else {
					// No return spec: void.
				}
				break;
			}

			case ParseKind::Assign: {
				// rhs's type is the assignment's type.
				if (node->childCount >= 2) {
					CopyTypeShape(node, node->children[1]);
				}
				break;
			}

			case ParseKind::ArgList: {
				break;
			}

			case ParseKind::BinOp: {
				// Numeric binops take the wider operand type. Not
				// exercised by hello.xm; left as a hook.
				if (node->childCount >= 2) {
					CopyTypeShape(node, node->children[0]);
				}
				break;
			}

			default:
				// Conjunctions / structural nodes fall through;
				// MorphStmt handles the ones we know about.
				break;
			}
		}

		// =============================================================
		// Helpers
		// =============================================================

		Symbol* MorpherState::DeclareSymbol(InternedString name,
			AllocationType alloc,
			ParseTreeNode* declNode)
		{
			Symbol* sym = symbols.InternSymbol(
				std::string_view(name.str, name.len),
				scopeStack.data(),
				static_cast<uint32_t>(scopeStack.size()));
			if (!sym) {
				// SymbolTable rejected the intern -- typically because
				// the scope path is deeper than the table's configured
				// maximum. Diagnose and let the caller carry on with
				// declaredSymbol = nullptr; nothing the Morpher does
				// later actually requires the Symbol to exist.
				Report(declNode,
					"failed to intern symbol '" +
					std::string(name.str, name.len) +
					"' (scope path depth " +
					std::to_string(scopeStack.size()) +
					" rejected by SymbolTable)");
				return nullptr;
			}
			sym->allocType = alloc;
			sym->xmoIdx = xmo.index;
			xmo.ownedSymbols.push_back(sym);
			declSite[sym] = declNode;
			declared.push_back(sym);
			return sym;
		}

		Symbol* MorpherState::ResolveSymbol(InternedString name)
		{
			return symbols.ResolveSymbol(
				std::string_view(name.str, name.len),
				scopeStack.data(),
				static_cast<uint32_t>(scopeStack.size()));
		}

		void MorpherState::CopyTypeShape(ParseTreeNode* dst,
			const ParseTreeNode* src)
		{
			dst->baseType = src->baseType;
			dst->pointerDepth = src->pointerDepth;
			dst->isArray = src->isArray;
		}

		ParseTreeNode* MorpherState::FirstChildOfKind(ParseTreeNode* parent,
			ParseKind k)
		{
			for (uint32_t i = 0; i < parent->childCount; ++i) {
				if (parent->children[i]->kind == k) return parent->children[i];
			}
			return nullptr;
		}

		uint32_t MorpherState::PushScope(ScopeTypes blockType)
		{
			uint32_t id = symbols.AllocScopeId();
			XmoScope rec;
			rec.scopeId = id;
			rec.parentScopeId = scopeStack.empty() ? 0 : scopeStack.back();
			rec.blockType = static_cast<uint8_t>(blockType);
			xmo.scopeTree.push_back(rec);
			scopeStack.push_back(id);
			return id;
		}

		void MorpherState::PopScope()
		{
			scopeStack.pop_back();
		}

		void MorpherState::Report(const ParseTreeNode* at,
			const std::string& msg)
		{
			oserror << xmo.name << ":" << at->line << ":" << at->col
				<< ": morph error: " << msg << std::endl;
			job.ErrorOccurred.store(true, std::memory_order_relaxed);
		}

		// =============================================================
		// Morpher log
		//
		// Same indented tree shape as the parser log so the two can be
		// diffed side-by-side. Each line ends with the resolved type
		// in the form "  :: <baseType><stars><[]>". A "Symbols:"
		// section follows the tree, listing each declared symbol with
		// its scope path and resolved type.
		// =============================================================
		std::string MorpherState::FormatType(uint32_t baseType,
			uint8_t pointerDepth,
			bool isArray) const
		{
			std::string out;
			for (uint8_t i = 0; i < pointerDepth; ++i) out += '*';
			out += BaseTypeName(static_cast<BaseTypeIds>(baseType));
			if (isArray) out += "[]";
			return out;
		}

		void MorpherState::WriteMorpherLog()
		{
			fs::path src(xmo.name);
			fs::path logPath = src.parent_path() /
				(src.stem().string() + ".morpher.txt");
			std::ofstream out(logPath);
			if (!out) return;

			DumpNode(out, xmo.parseTree, 0);

			out << "\nSymbols:\n";
			for (Symbol* sym : declared) {
				out << "  ";
				out.write(sym->name.str, sym->name.len);
				out << "  ::  "
					<< FormatType(
						sym->baseType.load(std::memory_order_relaxed),
						sym->pointerDepth, sym->isArray)
					<< "  ["
					<< AllocationTypeName(sym->allocType)
					<< "]  scope=";
				const uint32_t* path = sym->Path();
				for (uint32_t i = 0; i < sym->pathLen; ++i) {
					if (i > 0) out << '.';
					out << path[i];
				}
				out << '\n';
			}
		}

		void MorpherState::DumpNode(std::ofstream& out,
			const ParseTreeNode* n, int depth)
		{
			if (!n) return;

			std::string pos = xmo.name + ":" +
				std::to_string(n->line) + ":" + std::to_string(n->col);
			out.write(pos.data(), std::streamsize(pos.size()));
			for (size_t pad = pos.size(); pad < 24; ++pad) out.put(' ');

			for (int i = 0; i < depth; ++i) out.put(' '), out.put(' ');

			out << ParseKindName(n->kind);

			// Identifying payload (mirrors the parser log)
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
			default:
				break;
			}

			// Resolved type, if interesting
			if (n->baseType != U(BaseTypeIds::Unresolved))
			{
				out << "  ::  "
					<< FormatType(n->baseType, n->pointerDepth, n->isArray);
			}
			out << '\n';

			for (uint32_t i = 0; i < n->childCount; ++i) {
				DumpNode(out, n->children[i], depth + 1);
			}
		}

	} // namespace

	// =================================================================
	// Public entry points
	// =================================================================

	void Morpher::Morph(
		Xmo& xmo,
		PipelineQueue<ParseTreeNode*>& nodeQueue,
		SymbolTable& symbols,
		const CompileJob& job)
	{
		// Drain the streaming queue. The Parser pushes one entry per
		// leaf and a nullptr sentinel after it finishes; we wait on
		// the sentinel so the Parser's enqueues don't pile up and to
		// guarantee xmo.parseTree is set before we walk it (the
		// Parser sets parseTree before pushing the sentinel).
		ParseTreeNode* node = nullptr;
		while (true) {
			nodeQueue.WaitDequeue(node);
			if (node == nullptr) break;
		}

		MorphTree(xmo, symbols, job);
	}

	void Morpher::MorphTree(
		Xmo& xmo,
		SymbolTable& symbols,
		const CompileJob& job)
	{
		if (!xmo.parseTree) return;

		MorpherState st(xmo, symbols, job);
		st.MorphFile(xmo.parseTree);

		if (job.MorpherLog) {
			st.WriteMorpherLog();
		}
	}

} // namespace xmc