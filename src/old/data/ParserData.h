// data/ParserData.h
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include "../deps/include/parallel_hashmap/phmap.h"
#include "../tool/TextParser.h"

#include "Arena.h"
#include "XmoData.h"

namespace data
{

	enum class BaseTypeIds : uint8_t
	{
		null = 0,
		b8,								// boolean
		i8, i16, i32, i64,				// signed
		T_U8, T_U16, T_U32, T_U64,		// unsigned
		T_F32, T_F64,					// float
		d8, d16, d32, d64,				// decimal
		c8, c16,						// char
		zs8, zs16, zsu,					// zero-terminated strings
		s8, s16, su,					// pascal strings (first 4 bytes has the size)
		ustruct,						// a user defined struct has no functions
		list, queue, stack,
		btree, hashset, hashmap,
		function, uclass,
		arena, heap
	};

	enum class RefinementBits : uint8_t
	{
		Mutable			= 0x01,
		Resizable		= 0x02,
		Virtual			= 0x04,
		Arc				= 0x08,
		Heterogenous	= 0x10,
		Fluid			= 0x20,
		Concurrent		= 0x40,
		Variant			= 0x80
	};

	enum class ScopeTypes : uint8_t
	{
		File,
		Namespace,
		Function,
		Class,
		IfThen,
		For,
		While,
		Do,
		Nested,
		Shadow
	};

	enum class AllocationType { Stack, SysHeap, UHeap, Arena };

	// Global scope ID allocator.
	// Upper 16 bits = file index, lower 16 bits = local scope counter within that file.
	// Guarantees no collisions across the whole project and makes the file origin
	// readable from the ID alone. 0 is reserved for "none/global".
	inline std::atomic<uint32_t> NextScopeId{ 1 };

	inline uint32_t AllocScopeId() {
		return NextScopeId.fetch_add(1, std::memory_order_relaxed);
	}

	// A string that has been interned into SymbolArena.
	// Hash and length are computed once at intern time and cached here,
	// so all subsequent lookups pay no string traversal cost.
	struct InternedString {
		const char* str = nullptr;
		uint32_t    len = 0;
		uint64_t    hash = 0;
	};

	struct InternedStringHasher {
		uint64_t operator()(const InternedString& s) const {
			return s.hash;
		}
	};

	struct InternedStringEqual {
		bool operator()(const InternedString& a, const InternedString& b) const {
			// Fast path: hashes differ or lengths differ — definitely not equal
			if (a.hash != b.hash || a.len != b.len) return false;
			// Same pointer means same interned string — skip memcmp
			if (a.str == b.str) return true;
			// Hash collision with different pointers — fall back to content compare
			return memcmp(a.str, b.str, a.len) == 0;
		}
	};

	struct Symbol
	{
		InternedString          name;
		Xmo*                    originXmo;
		std::atomic<uint32_t>   baseType;
		std::atomic<uint32_t>   minrmask;
		std::atomic<uint32_t>   maxrmask;
		uint32_t                offset;
		AllocationType          allocType;

		// Full namespace path from outermost to innermost scope.
		// namespacePath[0] is always the file scope ID.
		// Depth is typically very shallow (1-4 levels).
		std::vector<uint32_t>   namespacePath;

		uint32_t ScopeDepth()    const { return (uint32_t)namespacePath.size(); }
		uint32_t InnermostScope() const { return namespacePath.back(); }
		uint32_t FileScope()     const { return namespacePath.empty() ? 0 : namespacePath[0]; }

		Symbol() : originXmo(nullptr), baseType(0), minrmask(0), maxrmask(0xFF), offset(0), allocType(AllocationType::Stack) {}
	};

	struct Scope
	{
		uint32_t            ScopeId;
		ScopeTypes          BlockType;
		Scope*              Parent;
		std::vector<Scope*> Children;
		std::vector<Symbol> Symbols;
	};

	struct FunctionNodeData
	{
		string      name;
		uint32_t    totalStackSize;
		uint64_t    exportIdx;
		bool        isLeaf = false;
	};

	struct StaticData {
		std::string         name;
		std::vector<uint8_t> bytes;
		uint64_t            exportIdx;
	};

	struct ParseTreeNode {
		// Replace std::vector with a raw array + count allocated on the Arena
		uint16_t* codeBlocks = nullptr;
		uint32_t                codeBlockCount = 0;

		// Use your data::InternedString instead of std::string!
		data::InternedString* patchSymbols = nullptr;
		uint32_t                patchSymbolCount = 0;

		StaticData* staticData = nullptr;
		uint32_t                staticDataCount = 0;

		FunctionNodeData* funcData = nullptr;

		ParseTreeNode** children = nullptr;
		uint32_t                childCount = 0;
	};

	// One entry per unique name in the shard table.
	// All symbols sharing that name (across different namespaces) live in candidates.
	struct SymbolEntry {
		InternedString          name;
		std::vector<Symbol*>    candidates;
	};

	struct SymbolShard {
		std::shared_mutex shared_mutex;
		phmap::flat_hash_map<InternedString, SymbolEntry, InternedStringHasher, InternedStringEqual> table;
	};

	inline Arena        SymbolArena;
	inline SymbolShard  SymbolTable[256];


	void ConsumeLength(TextParser& p, size_t length);

} // namespace data

namespace process
{
	// Interns a raw C string into SymbolArena.
	// Computes and caches hash and length. Returns an InternedString
	// whose lifetime is tied to SymbolArena.
	data::InternedString InternString(const char* str);

	// Registers a symbol definition encountered during parsing.
	// namespacePath is the full scope stack at the point of declaration,
	// outermost (file) scope ID first.
	data::Symbol* InternSymbol(const char* tempName, std::vector<uint32_t> namespacePath);

	// Resolves a symbol reference from a given call site.
	// callerPath is the full namespace path of the reference site, outermost first.
	// Returns the best-matching symbol, or nullptr if not found or ambiguous.
	data::Symbol* ResolveSymbol(const char* name, const std::vector<uint32_t>& callerPath);
}