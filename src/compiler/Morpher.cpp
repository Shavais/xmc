// compiler/Morpher.cpp
//
// Per-file Morpher: concurrent bottom-up type inference.
//
// Design:
//   The Parser submits one pool task per completed leaf node during Drive().
//   Each task calls MorphNoun, which applies the leaf's typing rule then walks
//   up via parent pointers (PropagateUp). At each ancestor the thread:
//     1. Atomically increments morphed_children (seq_cst).
//     2. Checks parser_complete (seq_cst) — if false, returns; the parser
//        thread will check when it closes the node.
//     3. Checks morphed_children == childCount — if false, returns; another
//        sibling's task will eventually win.
//     4. Test-and-sets morphed — if already set, returns; another thread won.
//     5. Calls ApplyRule inline and continues to the parent.
//
//   The seq_cst ordering on steps 1 and 2 prevents the lost-wakeup race:
//   if the parser stores parser_complete concurrently with the last child's
//   fetch_add, one of the two sides sees the other's write in the total
//   seq_cst order and handles the node.
//
//   Scope-opening nodes (File, ExternDecl, FuncDecl, Block) have their scope
//   IDs pre-allocated by the Parser and stored on the node, so the Morpher
//   never maintains its own scope stack.
//
//   Symbol::declNode carries the declaring ParseTreeNode* directly, so Call
//   and Ident nodes can read the callee's return type without a per-run map.
//
// What this does NOT yet implement:
//   - Re-propagation after late symbol resolution (subscriber index).
//   - rmask narrowing beyond defaults.
//   - Conversion-node insertion (Reviewer's job).
//   - Cross-file subscriber bookkeeping.
//
#include "pch/pch.h"
#include "Morpher.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "../tool/BS_thread_pool.hpp"

#include "tool/Logger.h"
#include "Types.h"

namespace fs = std::filesystem;

namespace xmc
{
	namespace
	{
		static constexpr uint32_t U(BaseTypeIds id)
		{
			return static_cast<uint32_t>(id);
		}

		// =============================================================
		// kExprDesc — typing rule for expression nodes, indexed by
		// ParseKind cast to uint16_t.
		//
		// Nodes with ExprRule::None are declaration or structural nodes
		// handled by kNodeDesc instead.
		// =============================================================
		enum class ExprRule : uint8_t
		{
			None = 0,
			IntLit,
			FloatLit,
			StringLit,
			CharLit,
			BoolLit,
			NullLit,
			Ident,
			AddressOf,
			Deref,
			CopyChild0,   // Negate, UnaryPlus, Not, BitNot, Assign, BinOp, Subscript
			MemberAccess,
			Call,
			ArgList,      // structural; no type on the ArgList itself
		};

		// One entry per ParseKind value, in enum-declaration order.
		static constexpr ExprRule kExprDesc[] = {
			ExprRule::None,        // Unknown
			ExprRule::None,        // File
			ExprRule::None,        // ExternDecl
			ExprRule::None,        // FuncDecl
			ExprRule::None,        // ParamList
			ExprRule::None,        // Param
			ExprRule::None,        // ReturnSpec
			ExprRule::None,        // Type
			ExprRule::None,        // Block
			ExprRule::None,        // VarDecl
			ExprRule::None,        // ExprStmt
			ExprRule::None,        // Return
			ExprRule::ArgList,     // ArgList
			ExprRule::CopyChild0,  // Assign
			ExprRule::Call,        // Call
			ExprRule::MemberAccess,// MemberAccess
			ExprRule::CopyChild0,  // Subscript
			ExprRule::AddressOf,   // AddressOf
			ExprRule::Deref,       // Deref
			ExprRule::CopyChild0,  // Negate
			ExprRule::CopyChild0,  // UnaryPlus
			ExprRule::CopyChild0,  // Not
			ExprRule::CopyChild0,  // BitNot
			ExprRule::CopyChild0,  // BinOp
			ExprRule::Ident,       // Ident
			ExprRule::IntLit,      // IntLit
			ExprRule::FloatLit,    // FloatLit
			ExprRule::StringLit,   // StringLit
			ExprRule::CharLit,     // CharLit
			ExprRule::BoolLit,     // BoolLit
			ExprRule::NullLit,     // NullLit
			ExprRule::None,        // Stub
		};
		static_assert(static_cast<size_t>(ParseKind::Stub) < std::size(kExprDesc),
			"kExprDesc too small — add entries for new ParseKind values");

		// =============================================================
		// kNodeDesc — rule for declaration and structural nodes, indexed
		// by ParseKind cast to uint16_t.
		//
		// Nodes whose ExprRule is non-None are not looked up in this table.
		// =============================================================
		enum class NodeRule : uint8_t
		{
			None = 0,       // structural with no action (ParamList, ExprStmt, Return, …)
			FileNode,       // record XmoScope for file scope
			TypeNode,       // map keyword → baseType
			ReturnSpecNode, // copy type from Type child
			ParamNode,      // copy type, declare symbol
			ExternDeclNode, // record scope, declare function symbol
			FuncDeclNode,   // record scope, declare function symbol
			VarDeclNode,    // copy type, declare symbol
			BlockNode,      // record XmoScope for block scope
		};

		static constexpr NodeRule kNodeDesc[] = {
			NodeRule::None,          // Unknown
			NodeRule::FileNode,      // File
			NodeRule::ExternDeclNode,// ExternDecl
			NodeRule::FuncDeclNode,  // FuncDecl
			NodeRule::None,          // ParamList — no action; structural pass-through
			NodeRule::ParamNode,     // Param
			NodeRule::ReturnSpecNode,// ReturnSpec
			NodeRule::TypeNode,      // Type
			NodeRule::BlockNode,     // Block
			NodeRule::VarDeclNode,   // VarDecl
			NodeRule::None,          // ExprStmt
			NodeRule::None,          // Return
			NodeRule::None,          // ArgList   — kExprDesc handles it
			NodeRule::None,          // Assign    — kExprDesc
			NodeRule::None,          // Call      — kExprDesc
			NodeRule::None,          // MemberAccess — kExprDesc
			NodeRule::None,          // Subscript — kExprDesc
			NodeRule::None,          // AddressOf — kExprDesc
			NodeRule::None,          // Deref     — kExprDesc
			NodeRule::None,          // Negate    — kExprDesc
			NodeRule::None,          // UnaryPlus — kExprDesc
			NodeRule::None,          // Not       — kExprDesc
			NodeRule::None,          // BitNot    — kExprDesc
			NodeRule::None,          // BinOp     — kExprDesc
			NodeRule::None,          // Ident     — kExprDesc
			NodeRule::None,          // IntLit    — kExprDesc
			NodeRule::None,          // FloatLit  — kExprDesc
			NodeRule::None,          // StringLit — kExprDesc
			NodeRule::None,          // CharLit   — kExprDesc
			NodeRule::None,          // BoolLit   — kExprDesc
			NodeRule::None,          // NullLit   — kExprDesc
			NodeRule::None,          // Stub
		};
		static_assert(static_cast<size_t>(ParseKind::Stub) < std::size(kNodeDesc),
			"kNodeDesc too small — add entries for new ParseKind values");

		// =============================================================
		// Forward declarations of free functions
		// =============================================================
		// ApplyRule/ApplyExprRule return true if PropagateUp should be called,
		// false if propagation is deferred (e.g. subscribed to a late symbol).
		static bool ApplyRule(ParseTreeNode* n,
			Xmo& xmo, SymbolTable& symbols, const CompileJob& job,
			BS::thread_pool<>& pool);
		static bool ApplyExprRule(ParseTreeNode* n, ExprRule rule,
			Xmo& xmo, SymbolTable& symbols, const CompileJob& job,
			BS::thread_pool<>& pool);
		static bool ApplyNodeRule(ParseTreeNode* n, NodeRule rule,
			Xmo& xmo, SymbolTable& symbols, const CompileJob& job,
			BS::thread_pool<>& pool);
		static void PropagateUp(ParseTreeNode* n,
			Xmo& xmo, SymbolTable& symbols, const CompileJob& job,
			BS::thread_pool<>& pool);

		// =============================================================
		// Scope helpers
		// =============================================================

		static void CollectScopePath(ParseTreeNode* startFrom,
			std::vector<uint32_t>& path)
		{
			path.clear();
			for (ParseTreeNode* cur = startFrom; cur; cur = cur->parent) {
				if (cur->scopeId != 0) path.push_back(cur->scopeId);
			}
			std::reverse(path.begin(), path.end());
		}

		static uint32_t GetParentScopeId(ParseTreeNode* parent)
		{
			for (ParseTreeNode* cur = parent; cur; cur = cur->parent) {
				if (cur->scopeId != 0) return cur->scopeId;
			}
			return 0;
		}

		// =============================================================
		// Utility helpers
		// =============================================================

		static void CopyTypeShape(ParseTreeNode* dst, const ParseTreeNode* src)
		{
			dst->baseType     = src->baseType;
			dst->pointerDepth = src->pointerDepth;
			dst->isArray      = src->isArray;
		}

		static ParseTreeNode* FirstChildOfKind(ParseTreeNode* parent, ParseKind k)
		{
			for (uint32_t i = 0; i < parent->childCount; ++i) {
				if (parent->children[i]->kind == k) return parent->children[i];
			}
			return nullptr;
		}

		static void Report(const ParseTreeNode* at,
			const std::string& msg,
			const Xmo& xmo,
			const CompileJob& job)
		{
			oserror << xmo.name << ":" << at->line << ":" << at->col
				<< ": morph error: " << msg << std::endl;
			job.ErrorOccurred.store(true, std::memory_order_relaxed);
		}

		// =============================================================
		// Symbol helpers
		// =============================================================

		static Symbol* DeclareSymbol(InternedString name,
			AllocationType alloc,
			ParseTreeNode* declNode,
			Xmo& xmo,
			SymbolTable& symbols,
			const CompileJob& job,
			BS::thread_pool<>& pool)
		{
			std::vector<uint32_t> path;
			CollectScopePath(declNode->parent, path);

			std::vector<std::function<void(Symbol*)>> waiters;
			Symbol* sym = symbols.InternSymbol(
				std::string_view(name.str, name.len),
				path.data(),
				static_cast<uint32_t>(path.size()),
				&waiters);
			if (!sym) {
				Report(declNode,
					"failed to intern symbol '" +
					std::string(name.str, name.len) +
					"' (scope depth " + std::to_string(path.size()) +
					" rejected by SymbolTable)",
					xmo, job);
				return nullptr;
			}
			sym->allocType = alloc;
			sym->xmoIdx    = xmo.index;
			sym->declNode  = declNode;
			xmo.PushOwnedSymbol(sym);  // thread-safe append

			// Submit subscriber callbacks as pool tasks. Each callback was
			// registered by SubscribeOrResolve on a morpher task that couldn't
			// yet resolve this symbol.
			for (auto& w : waiters) {
				pool.detach_task([sym, waiter = std::move(w)]() {
					waiter(sym);
				});
			}
			return sym;
		}

		static Symbol* ResolveSymbol(InternedString name,
			ParseTreeNode* atNode,
			SymbolTable& symbols)
		{
			std::vector<uint32_t> path;
			CollectScopePath(atNode->parent, path);
			return symbols.ResolveSymbol(
				std::string_view(name.str, name.len),
				path.data(),
				static_cast<uint32_t>(path.size()));
		}

		// =============================================================
		// ApplyExprRule — expression-node typing
		// Returns false only for the Ident case when the symbol is not yet
		// declared and a subscriber callback was registered. The caller
		// (MorphNoun) skips PropagateUp in that case; the callback will
		// resume propagation later when the declaration fires.
		// =============================================================
		static bool ApplyExprRule(ParseTreeNode* n, ExprRule rule,
			Xmo& xmo, SymbolTable& symbols, const CompileJob& job,
			BS::thread_pool<>& pool)
		{
			switch (rule) {

			case ExprRule::IntLit: {
				uint64_t v = n->intValue;
				if      (v <= 0xFFull)         n->baseType = U(BaseTypeIds::T_U8);
				else if (v <= 0xFFFFull)        n->baseType = U(BaseTypeIds::T_U16);
				else if (v <= 0xFFFFFFFFull)    n->baseType = U(BaseTypeIds::T_U32);
				else                            n->baseType = U(BaseTypeIds::T_U64);
				break;
			}

			case ExprRule::FloatLit:
				n->baseType = U(BaseTypeIds::T_F64);
				break;

			case ExprRule::StringLit:
				n->baseType = U(BaseTypeIds::T_U8);
				n->isArray  = true;
				break;

			case ExprRule::CharLit:
				n->baseType = U(BaseTypeIds::c8);
				break;

			case ExprRule::BoolLit:
				n->baseType = U(BaseTypeIds::b);
				break;

			case ExprRule::NullLit:
				n->baseType = U(BaseTypeIds::Null);
				break;

			case ExprRule::Ident: {
				std::vector<uint32_t> path;
				CollectScopePath(n->parent, path);

				Symbol* sym = symbols.SubscribeOrResolve(
					std::string_view(n->name.str, n->name.len),
					path.data(), static_cast<uint32_t>(path.size()),
					[n, &xmo, &symbols, &job, &pool](Symbol* s) {
						n->declaredSymbol = s;
						if (s->declNode) CopyTypeShape(n, s->declNode);
						else n->baseType = s->baseType.load(std::memory_order_relaxed);
						PropagateUp(n, xmo, symbols, job, pool);
					});

				if (!sym) {
					// Subscribed: the callback will set the type and propagate.
					return false;
				}
				n->declaredSymbol = sym;
				if (sym->declNode) CopyTypeShape(n, sym->declNode);
				else n->baseType = sym->baseType.load(std::memory_order_relaxed);
				break;
			}

			case ExprRule::AddressOf:
				if (n->childCount > 0) {
					const ParseTreeNode* op = n->children[0];
					n->baseType     = op->baseType;
					n->isArray      = op->isArray;
					uint32_t depth  = uint32_t(op->pointerDepth) + 1;
					n->pointerDepth = depth > 255 ? 255 : uint8_t(depth);
				}
				break;

			case ExprRule::Deref:
				if (n->childCount > 0) {
					const ParseTreeNode* op = n->children[0];
					n->baseType     = op->baseType;
					n->isArray      = op->isArray;
					n->pointerDepth = op->pointerDepth > 0 ? op->pointerDepth - 1 : 0;
				}
				break;

			case ExprRule::CopyChild0:
				if (n->childCount > 0) {
					CopyTypeShape(n, n->children[0]);
				}
				break;

			case ExprRule::MemberAccess:
				if (n->childCount > 0 &&
					n->name.len == 6 &&
					std::memcmp(n->name.str, "length", 6) == 0 &&
					n->children[0]->isArray)
				{
					n->baseType     = U(BaseTypeIds::T_U64);
					n->pointerDepth = 0;
					n->isArray      = false;
				}
				else {
					n->baseType = U(BaseTypeIds::Unresolved);
				}
				break;

			case ExprRule::Call: {
				// child[0] is the callee Ident (already resolved); get return
				// type from the declaring ExternDecl / FuncDecl via sym->declNode.
				if (n->childCount == 0) break;
				const ParseTreeNode* callee = n->children[0];
				Symbol* sym = callee->declaredSymbol;
				if (!sym) { n->baseType = U(BaseTypeIds::Unresolved); break; }
				ParseTreeNode* decl = sym->declNode;
				if (!decl)    { n->baseType = U(BaseTypeIds::Unresolved); break; }
				ParseTreeNode* retCarrier = FirstChildOfKind(decl, ParseKind::ReturnSpec);
				if (retCarrier) {
					CopyTypeShape(n, retCarrier);
				}
				// No ReturnSpec → void; leave baseType at Unresolved (= no type).
				break;
			}

			case ExprRule::ArgList:
				// No type on ArgList itself; structural pass-through.
				break;

			default:
				break;
			}

			(void)xmo; (void)job; (void)pool; // may not be used in all branches
			return true;
		}

		// =============================================================
		// ApplyNodeRule — declaration and structural nodes
		// =============================================================
		static bool ApplyNodeRule(ParseTreeNode* n, NodeRule rule,
			Xmo& xmo, SymbolTable& symbols, const CompileJob& job,
			BS::thread_pool<>& pool)
		{
			switch (rule) {

			case NodeRule::FileNode: {
				XmoScope rec;
				rec.scopeId       = n->scopeId;
				rec.parentScopeId = 0;
				rec.blockType     = static_cast<uint8_t>(ScopeTypes::File);
				xmo.PushScopeRecord(rec);
				break;
			}

			case NodeRule::TypeNode: {
				std::string_view kw(n->name.str, n->name.len);
				n->baseType = U(BaseTypeFromKeyword(kw));
				break;
			}

			case NodeRule::ReturnSpecNode: {
				ParseTreeNode* type = FirstChildOfKind(n, ParseKind::Type);
				if (type) CopyTypeShape(n, type);
				break;
			}

			case NodeRule::ParamNode: {
				ParseTreeNode* type = FirstChildOfKind(n, ParseKind::Type);
				if (type) CopyTypeShape(n, type);
				Symbol* sym = DeclareSymbol(n->name, AllocationType::Stack, n, xmo, symbols, job, pool);
				n->declaredSymbol = sym;
				if (sym) {
					sym->pointerDepth = n->pointerDepth;
					sym->isArray      = n->isArray;
					sym->baseType.store(n->baseType, std::memory_order_relaxed);
				}
				break;
			}

			case NodeRule::ExternDeclNode: {
				// Record the parameter scope opened by this extern decl.
				XmoScope rec;
				rec.scopeId       = n->scopeId;
				rec.parentScopeId = GetParentScopeId(n->parent);
				rec.blockType     = static_cast<uint8_t>(ScopeTypes::Function);
				xmo.PushScopeRecord(rec);
				// Declare the function symbol in the parent (file) scope.
				Symbol* sym = DeclareSymbol(n->name, AllocationType::Stack, n, xmo, symbols, job, pool);
				n->declaredSymbol = sym;
				n->baseType       = U(BaseTypeIds::function);
				if (sym) {
					sym->baseType.store(U(BaseTypeIds::function),
						std::memory_order_relaxed);
				}
				break;
			}

			case NodeRule::FuncDeclNode: {
				// Record the function scope.
				XmoScope rec;
				rec.scopeId       = n->scopeId;
				rec.parentScopeId = GetParentScopeId(n->parent);
				rec.blockType     = static_cast<uint8_t>(ScopeTypes::Function);
				xmo.PushScopeRecord(rec);
				// Declare the function symbol in the parent (file) scope.
				Symbol* sym = DeclareSymbol(n->name, AllocationType::Stack, n, xmo, symbols, job, pool);
				n->declaredSymbol = sym;
				n->baseType       = U(BaseTypeIds::function);
				if (sym) {
					sym->baseType.store(U(BaseTypeIds::function),
						std::memory_order_relaxed);
				}
				break;
			}

			case NodeRule::VarDeclNode: {
				ParseTreeNode* type = FirstChildOfKind(n, ParseKind::Type);
				if (type) CopyTypeShape(n, type);
				Symbol* sym = DeclareSymbol(n->name, AllocationType::Stack, n, xmo, symbols, job, pool);
				n->declaredSymbol = sym;
				if (sym) {
					sym->pointerDepth = n->pointerDepth;
					sym->isArray      = n->isArray;
					sym->baseType.store(n->baseType, std::memory_order_relaxed);
				}
				break;
			}

			case NodeRule::BlockNode: {
				XmoScope rec;
				rec.scopeId       = n->scopeId;
				rec.parentScopeId = GetParentScopeId(n->parent);
				rec.blockType     = static_cast<uint8_t>(ScopeTypes::Nested);
				xmo.PushScopeRecord(rec);
				break;
			}

			case NodeRule::None:
			default:
				break;
			}

			(void)symbols; (void)pool; // may not be used in all branches
			return true;
		}

		// =============================================================
		// ApplyRule — dispatch to expr or node rule
		// =============================================================
		static bool ApplyRule(ParseTreeNode* n,
			Xmo& xmo, SymbolTable& symbols, const CompileJob& job,
			BS::thread_pool<>& pool)
		{
			auto k = static_cast<size_t>(n->kind);
			ExprRule er = (k < std::size(kExprDesc)) ? kExprDesc[k] : ExprRule::None;
			if (er != ExprRule::None) {
				return ApplyExprRule(n, er, xmo, symbols, job, pool);
			}
			else {
				NodeRule nr = (k < std::size(kNodeDesc)) ? kNodeDesc[k] : NodeRule::None;
				return ApplyNodeRule(n, nr, xmo, symbols, job, pool);
			}
		}

		// =============================================================
		// PropagateUp — walk parent pointers after a node has been morphed.
		//
		// For each ancestor:
		//   1. Increment morphed_children (seq_cst) — signals one more child done.
		//   2. Load parser_complete (seq_cst) — if false, the parser thread has
		//      not closed this ancestor yet; it will check when it does.
		//   3. Check morphed_children == childCount — if false, a sibling is
		//      still pending; that sibling's task will eventually win.
		//   4. Test-and-set morphed — if already true, another thread won.
		//   5. Apply the ancestor's rule inline and continue upward.
		//
		// The seq_cst pair on steps 1–2 eliminates the lost-wakeup race with
		// the parser thread: both sides observe each other's write in the
		// single total seq_cst order, so exactly one of them handles the node.
		// =============================================================
		static void PropagateUp(ParseTreeNode* n,
			Xmo& xmo, SymbolTable& symbols, const CompileJob& job,
			BS::thread_pool<>& pool)
		{
			ParseTreeNode* cur = n->parent;
			while (cur) {
				uint32_t mc = cur->morphed_children.fetch_add(
					1, std::memory_order_seq_cst) + 1;
				if (!cur->parser_complete.load(std::memory_order_seq_cst))
					return; // parser hasn't closed this node yet
				if (mc != cur->childCount)
					return; // siblings still pending
				if (cur->morphed.exchange(true, std::memory_order_acq_rel))
					return; // another thread already claimed this node
				if (!ApplyRule(cur, xmo, symbols, job, pool))
					return; // propagation deferred (subscribed callback will resume)
				cur = cur->parent;
			}
		}

		// =============================================================
		// Morpher log
		// =============================================================

		static std::string FormatType(uint32_t baseType,
			uint8_t pointerDepth, bool isArray)
		{
			std::string out;
			for (uint8_t i = 0; i < pointerDepth; ++i) out += '*';
			out += BaseTypeName(static_cast<BaseTypeIds>(baseType));
			if (isArray) out += "[]";
			return out;
		}

		static void DumpNode(std::ofstream& out,
			const ParseTreeNode* n,
			const std::string& xmoName,
			int depth)
		{
			if (!n) return;

			std::string pos = xmoName + ":" +
				std::to_string(n->line) + ":" + std::to_string(n->col);
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

			if (n->baseType != U(BaseTypeIds::Unresolved)) {
				out << "  ::  "
					<< FormatType(n->baseType, n->pointerDepth, n->isArray);
			}
			out << '\n';

			for (uint32_t i = 0; i < n->childCount; ++i) {
				DumpNode(out, n->children[i], xmoName, depth + 1);
			}
		}

		static void WriteMorpherLog(const Xmo& xmo)
		{
			fs::path src(xmo.name);
			fs::path logPath = src.parent_path() /
				(src.stem().string() + ".morpher.txt");
			std::ofstream out(logPath);
			if (!out) return;

			DumpNode(out, xmo.parseTree, xmo.name, 0);

			out << "\nSymbols:\n";
			for (const Symbol* sym : xmo.ownedSymbols) {
				out << "  ";
				out.write(sym->name.str, sym->name.len);
				out << "  ::  "
					<< FormatType(
						sym->baseType.load(std::memory_order_relaxed),
						sym->pointerDepth, sym->isArray)
					<< "  ["
					<< AllocationTypeName(sym->allocType)
					<< "]  scope=";
				const uint32_t* p = sym->Path();
				for (uint32_t i = 0; i < sym->pathLen; ++i) {
					if (i > 0) out << '.';
					out << p[i];
				}
				out << '\n';
			}
		}

	} // namespace

	// =================================================================
	// Public entry points
	// =================================================================

	void Morpher::MorphNoun(
		ParseTreeNode*     node,
		Xmo&               xmo,
		SymbolTable&       symbols,
		const CompileJob&  job,
		BS::thread_pool<>& pool)
	{
		if (ApplyRule(node, xmo, symbols, job, pool)) {
			PropagateUp(node, xmo, symbols, job, pool);
		}
		// If ApplyRule returned false the node subscribed to a late symbol;
		// the callback will call PropagateUp once the declaration fires.
	}

	void Morpher::MorphTree(
		Xmo&              xmo,
		SymbolTable&      symbols,
		const CompileJob& job)
	{
		// All MorphNoun tasks completed before this is called (pool drained by
		// Compiler). Nothing to process here — just write the morpher log.
		if (job.MorpherLog) {
			WriteMorpherLog(xmo);
		}
	}

} // namespace xmc
