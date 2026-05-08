// compiler/Types.h
//
// Cross-pipeline type and storage enums.
//
// BaseTypeIds is the type tag the Morpher writes onto every value-
// producing parse node and onto every Symbol it declares. The Reviewer
// keys Table-A lookups off it; the Coder reads it from the parse tree
// when selecting code blocks; the Linker reads AllocationType from
// Symbol when deciding section placement.
//
// Numeric values are part of the persisted .xmo format. Append new
// values; do not reorder existing ones.
//
#pragma once

#include <cstdint>
#include <string_view>

namespace xmc
{
	// =====================================================================
	// BaseTypeIds
	//
	// Enum of every primitive type the language carries, plus three
	// pipeline markers (Unresolved, Null, Error) that flow through the
	// Morpher and Reviewer. The underlying width is u8 because every
	// value fits comfortably in a byte; Symbol::baseType widens to u32
	// only because std::atomic<u8> is more painful than std::atomic<u32>
	// on x64.
	//
	// Marker semantics:
	//   Unresolved   default for a value-producing node the Morpher
	//                hasn't reached yet, and for any Symbol whose type
	//                hasn't been pinned down. The Reviewer scans
	//                xmo.ownedSymbols for this state and emits a warning.
	//                Never deliberately *set* by the Morpher -- it's a
	//                "wasn't touched" sentinel.
	//   Null         the type of the `null` literal. Distinct from
	//                Unresolved.
	//   Error        the Morpher hit a type contradiction at this node
	//                and emitted a diagnostic. Dependents propagate the
	//                Error silently rather than re-diagnosing.
	// =====================================================================
	enum class BaseTypeIds : uint8_t
	{
		Unresolved = 0,                 // never set; default state
		Null,                           // type of the `null` literal
		Error,                          // morph failure; propagate silently

		// Boolean
		b,

		// Signed integers
		i8, i16, i32, i64,

		// Unsigned integers (T_ prefix avoids Win32 macro collisions
		// with UINT8 / UCHAR / friends).
		T_U8, T_U16, T_U32, T_U64,

		// Floats (T_ prefix for symmetry, even though the conflict is
		// only theoretical here).
		T_F32, T_F64,

		// Decimals (exact rational, integer numerator + denominator)
		d8, d16, d32, d64,

		// Characters
		c8, c16,

		// Zero-terminated strings
		zs8, zs16, zsu,

		// Pascal-style strings (length-prefixed)
		s8, s16, su,

		// Aggregates
		ustruct,                        // user struct (no functions)
		list, queue, stack,
		btree, hashset, hashmap,
		function, uclass,
		arena, heap,
	};

	// Best-effort short name for a base-type id, for log output. Returns
	// the source spelling ("u8", "i32", ...) for primitives and a fallback
	// "type#NN" form for anything not specifically listed.
	const char* BaseTypeName(BaseTypeIds id);

	// Maps a primitive-type keyword's source spelling (the bytes between
	// srcStart and srcStart+srcLen on a primitive Type token) to its
	// BaseTypeIds value. Returns Unresolved for anything not recognized.
	BaseTypeIds BaseTypeFromKeyword(std::string_view kw);

	// =====================================================================
	// RefinementBits
	//
	// Orthogonal qualifiers layered on top of a base type. The Morpher
	// narrows a Symbol's [minrmask, maxrmask] window as it discovers
	// constraints; a fully-resolved Symbol has minrmask == maxrmask.
	// =====================================================================
	enum class RefinementBits : uint8_t
	{
		Mutable = 0x01,
		Resizable = 0x02,
		Virtual = 0x04,
		Arc = 0x08,
		Heterogeneous = 0x10,
		Fluid = 0x20,
		Concurrent = 0x40,
		Variant = 0x80,
	};

	// =====================================================================
	// ScopeTypes
	//
	// What kind of lexical block a scope id represents. Stored on
	// XmoScope::blockType (Xmo.h) so the persisted scope tree carries
	// enough information for incremental rebuilds.
	// =====================================================================
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
		Shadow,
	};

	const char* ScopeTypeName(ScopeTypes t);

	// =====================================================================
	// AllocationType
	//
	// Where a Symbol's storage lives. (Possibly should be `AllocLoc`;
	// the existing name carries enough that renaming is deferred.)
	// =====================================================================
	enum class AllocationType : uint8_t
	{
		Stack,                          // local: [rbp - offset]
		SysHeap,                        // OS heap (malloc / VirtualAlloc family)
		UHeap,                          // xmc-managed user heap
		Arena,                          // xmc-managed arena
	};

	const char* AllocationTypeName(AllocationType t);

} // namespace xmc