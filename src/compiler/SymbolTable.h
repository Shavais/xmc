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
			// Same pointer means same interned string — skip memcmp.
			if (a.str == b.str) return true;
			// Hash collision with different pointers — fall back to content.
			return std::memcmp(a.str, b.str, a.len) == 0;
		}
	};

	// ----------------------------------------------------------------------
	// Symbol
	// Compiler-wide handle for a named entity (variable, function, type,
	// namespace member, ...). Lives for the entire compile job.
	// baseType, minrmask, and maxrmask are atomic because Typer and
	// Refiner mutate them concurrently across source files.
	// ----------------------------------------------------------------------
    // Inline path capacity. Fixed at compile time to keep Symbol = 64 bytes.
    // Projects that need deeper nesting spill to arena-allocated overflow.
	static constexpr uint32_t SYMBOL_INLINE_DEPTH = 5;

	// Hard ceiling on what a project file can request. Guards against
	// typos in the project file turning into absurd allocations.
	static constexpr uint32_t SYMBOL_MAX_DEPTH = 32;

	// Sentinel for "no originating xmo". Chosen so a zero-initialized
	// Symbol (the arena path via `new (mem) Symbol()`) starts with no xmo.
	static constexpr uint32_t NO_XMO = 0xFFFFFFFFu;

	struct alignas(64) Symbol
	{
		InternedString        name;        // 24
		uint32_t              xmoIdx;      //  4   index into Compiler::xmos
		std::atomic<uint32_t> baseType;    //  4
		uint32_t              offset;      //  4
		std::atomic<uint8_t>  minrmask;    //  1
		std::atomic<uint8_t>  maxrmask;    //  1
		AllocationType        allocType;   //  1
		uint8_t               pathLen;     //  1

		// 20 bytes of path storage. For pathLen <= SYMBOL_INLINE_DEPTH the
		// inline array is live; otherwise overflow_path points at arena
		// memory holding `pathLen` u32s.
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

	static_assert(sizeof(Symbol) == 64, "Symbol must be exactly one cache line");
	static_assert(alignof(Symbol) == 64, "Symbol must be cache-line aligned");

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