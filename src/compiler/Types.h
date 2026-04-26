#pragma once
// compiler/Types.h
#pragma once

#include <cstdint>
#include <vector>

namespace xmc
{
	// SymbolTable.h completes this; Scope below uses it as a pointer only.
	struct Symbol;

	// Primitive and container base types. Used by Symbol::baseType and by
	// Coder/Emitter when selecting code blocks.
	enum class BaseTypeIds : uint8_t
	{
		null = 0,
		b8,                                // boolean
		i8, i16, i32, i64,                 // signed
		T_U8, T_U16, T_U32, T_U64,         // unsigned
		T_F32, T_F64,                      // float
		d8, d16, d32, d64,                 // decimal
		c8, c16,                           // char
		zs8, zs16, zsu,                    // zero-terminated strings
		s8, s16, su,                       // pascal strings (first 4 bytes = size)
		ustruct,                           // user-defined struct (no functions)
		list, queue, stack,
		btree, hashset, hashmap,
		function, uclass,
		arena, heap
	};

	// Orthogonal qualifiers layered on top of a base type. Refiner narrows
	// the min/max refinement range stored on each Symbol.
	enum class RefinementBits : uint8_t
	{
		Mutable = 0x01,
		Resizable = 0x02,
		Virtual = 0x04,
		Arc = 0x08,
		Heterogenous = 0x10,
		Fluid = 0x20,
		Concurrent = 0x40,
		Variant = 0x80
	};

	// Classifies a lexical block.
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

	// Where a symbol's storage lives.
	enum class AllocationType : uint8_t
	{
		Stack,
		SysHeap,
		UHeap,
		Arena
	};

	// One node of the lexical scope tree.
	// Symbols is non-owning; the canonical Symbol objects live in SymbolTable.
	struct Scope
	{
		uint32_t             ScopeId = 0;
		ScopeTypes           BlockType = ScopeTypes::Nested;
		Scope* Parent = nullptr;
		std::vector<Scope*>  Children;
		std::vector<Symbol*> Symbols;
	};

} // namespace xmc