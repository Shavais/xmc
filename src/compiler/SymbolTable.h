// compiler/SymbolTable.h
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "Types.h"   // AllocationType

namespace xmc
{
	class Xmo;   // Symbol holds a back-pointer to its origin xmo

	// ----------------------------------------------------------------------
	// InternedString
	// A name that lives in a SymbolTable's name arena. Hash and length are
	// computed once at intern time and cached here, so every subsequent
	// lookup, hash, or compare is O(1) in the name length.
	// ----------------------------------------------------------------------

	struct InternedString
	{
		const char* str = nullptr;
		uint32_t    len = 0;
		uint64_t    hash = 0;
	};
	struct InternedStringHasher
	{
		uint64_t operator()(const InternedString& s) const { return s.hash; }
	};

	struct InternedStringEqual
	{
		bool operator()(const InternedString& a, const InternedString& b) const
		{
			// Fast rejects: different hash or length.
			if (a.hash != b.hash || a.len != b.len) return false;
			// Same pointer means same interned string � skip memcmp.
			if (a.str == b.str) return true;
			// Hash collision with different pointers � fall back to content.
			return std::memcmp(a.str, b.str, a.len) == 0;
		}
	};

	// ----------------------------------------------------------------------
	// Symbol
	// Compiler-wide handle for a named entity (variable, function, type,
	// namespace member, ...). Lives for the entire compile job.
	// baseType, minrmask, and maxrmask are atomic because Typer and
	// Refiner mutate them concurrently across source files.
	//
	// The struct is laid out for clarity, not for cache-line packing.
	// An earlier revision pinned Symbol to 64 bytes via alignas(64) and
	// constrained inline scope-path storage to keep it that way; the
	// constraint forced the type system to bifurcate (pointerDepth /
	// isArray on the parse node, not the Symbol), which cost more in
	// downstream code clarity than the cache-line win was worth at this
	// stage of the project. If profiling later shows Symbol-touching
	// loops are hot, revisit -- but the layout change is mechanical
	// because none of these fields cross the .xmo serialized boundary.
	// ----------------------------------------------------------------------

	// Inline path capacity. Generous default; overflow path is allocated
	// when a project legitimately nests deeper.
	static constexpr uint32_t SYMBOL_INLINE_DEPTH = 8;

	// Hard ceiling on what a project file can request. Guards against
	// typos in the project file turning into absurd allocations.
	static constexpr uint32_t SYMBOL_MAX_DEPTH = 32;

	// Sentinel for "no originating xmo". Chosen so a zero-initialized
	// Symbol (the arena path via `new (mem) Symbol()`) starts with no xmo.
	static constexpr uint32_t NO_XMO = 0xFFFFFFFFu;

	struct Symbol
	{
		InternedString        name;        // 24
		uint32_t              xmoIdx;      //  4   index into Compiler::xmos
		std::atomic<uint32_t> baseType;    //  4
		uint32_t              offset;      //  4
		std::atomic<uint8_t>  minrmask;    //  1
		std::atomic<uint8_t>  maxrmask;    //  1
		AllocationType        allocType;   //  1
		uint8_t               pathLen;     //  1

		// Type shape that doesn't fit in baseType alone. Plain (non-atomic)
		// because they're written once at declaration time before the
		// Symbol becomes visible to other threads via the subscriber index;
		// the atomic store of baseType serves as the release that publishes
		// these to readers.
		uint8_t               pointerDepth;//  1   leading '*' count
		bool                  isArray;     //  1   trailing "[]"

		// Inline path storage; spills to overflow_path when pathLen
		// exceeds SYMBOL_INLINE_DEPTH.
		union {
			uint32_t  inline_path[SYMBOL_INLINE_DEPTH];
			uint32_t* overflow_path;
		};

		Symbol()
			: xmoIdx(NO_XMO),
			baseType(0),
			offset(0),
			minrmask(0),
			maxrmask(0xFF),
			allocType(AllocationType::Stack),
			pathLen(0),
			pointerDepth(0),
			isArray(false),
			inline_path{} {
		}

		Symbol(const Symbol&) = delete;
		Symbol& operator=(const Symbol&) = delete;

		const uint32_t* Path() const {
			return pathLen <= SYMBOL_INLINE_DEPTH ? inline_path : overflow_path;
		}

		uint32_t ScopeDepth()     const { return pathLen; }
		uint32_t InnermostScope() const { return pathLen ? Path()[pathLen - 1] : 0; }
		uint32_t FileScope()      const { return pathLen ? Path()[0] : 0; }
	};

	struct SymbolKey
	{
		InternedString name;
		uint32_t       innermostScope = 0;

		bool operator==(const SymbolKey& other) const
		{
			return innermostScope == other.innermostScope && InternedStringEqual{}(name, other.name);
		}
	};

	struct SymbolKeyHash
	{
		uint64_t operator()(const SymbolKey& k) const
		{
			return k.name.hash ^ (uint64_t(k.innermostScope) * 0x9E3779B97F4A7C15ull);
		}
	};

	// ----------------------------------------------------------------------
	// SymbolTable
	// Owns the name arena, the sharded symbol hash table, and the scope
	// id allocator for one compile job. All public methods are thread-safe
	// and may be called concurrently from any pipeline stage.
	//
	// Compiler creates one SymbolTable per CompileJob and passes it by
	// reference to Parser, Typer, Refiner, Reviewer, Coder, and Xmo.
	// ----------------------------------------------------------------------
	class SymbolTable
	{
	public:
		SymbolTable();
		~SymbolTable();

		SymbolTable(const SymbolTable&) = delete;
		SymbolTable& operator=(const SymbolTable&) = delete;

		// Interns a name into the arena. Repeated interning of the same
		// text returns an InternedString with the same `str` pointer, so
		// pointer equality is a valid fast equality test.
		InternedString InternString(std::string_view text);

		// Registers a symbol definition. namespacePath is the scope stack
		// at the declaration site, outermost (file) scope id first.
		// The returned pointer is stable for the lifetime of this table.
		Symbol* InternSymbol(std::string_view name, const uint32_t* namespacePath, uint32_t pathLen);

		// Resolves a symbol reference. callerPath is the scope stack at
		// the reference site, outermost first. Returns the best-matching
		// symbol, or nullptr if not found or ambiguous.
		Symbol* ResolveSymbol(std::string_view name, const uint32_t* callerPath, uint32_t callerPathLen);

		// Allocates a fresh, globally unique scope id for this job.
		// Upper 16 bits = file index, lower 16 bits = per-file counter.
		// Zero is reserved for "none/global".
		uint32_t AllocScopeId();

	private:
		struct Impl;
		std::unique_ptr<Impl> m_;
	};

} // namespace xmc