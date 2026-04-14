#pragma once

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include "../deps/include/parallel_hashmap/phmap.h"

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


	// The "8-bit" Refinement Mask
	enum class RefinementBits : uint8_t
	{
		Mutable			= 0x01,			// the bit is 1/true if the symbol refers to something that is mutable, 0 if it refers to something that is immutable
		Resizable		= 0x02,			// ..
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


	struct Symbol
	{
		InternedString name;
		Xmo* originXmo;
		std::atomic<uint32_t> baseType;
		std::atomic<uint32_t> rmask;
		uint32_t offset;				// for writing to xmo's and to the coff
		AllocationType allocType;
		
		Symbol() :name(nullptr), scopeId(0), originXmo(nullptr), baseType(0), rmask(0) {};
		Symbol(const char* name, uint32_t scopeId, Xmo* originXmo, uint32_t baseType, uint32_t rmask) 
			: name(name), scopeId(scopeId), originXmo(originXmo), baseType(baseType), rmask(rmask) {}
	};

	struct Scope
	{
		uint32_t ScopeId;
		ScopeTypes BlockType;
		Scope* Parent;
		std::vector<Scope*> Children;
		std::vector<Symbol> Symbols;
	};

	struct FunctionNodeData
	{
		string name;
		uint32_t totalStackSize;
		uint64_t exportIdx;
		bool isLeaf = false;
	};

	struct StaticData {
		std::string name;       
		std::vector<uint8_t> bytes;
		uint64_t exportIdx;
	};

	struct ParseTreeNode {
		// Standard Fields
		std::vector<uint16_t> codeBlocks;
		std::vector<std::string> patchSymbols;
		std::vector<StaticData> staticData;
		FunctionNodeData* funcData = nullptr;

		// Hierarchy
		ParseTreeNode** children;
		uint32_t childCount = 0;
	};

	struct InternedString {
		const char* str;
		uint32_t len;
		uint64_t hash;
	};

	struct InternedStringHasher {
		uint64_t operator()(const InternedString& s) const { return s.hash; }
	};

	struct InternedStringEqual {
		bool operator()(const InternedString& a, const InternedString& b) const {
			return a.hash == b.hash
				&& a.len == b.len
				&& memcmp(a.str, b.str, a.len) == 0;
		}
	};
	struct SymbolShard
	{
		std::shared_mutex shared_mutex;
		phmap::flat_hash_map<SymbolKey, SymbolEntry, SymbolKeyHasher> table;
	};

	inline Arena SymbolArena;

	inline SymbolShard SymbolTable[256];
}
