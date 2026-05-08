// compiler/Types.cpp
//
// Lookup tables for log output and for keyword -> BaseTypeIds mapping.
//
#include "pch/pch.h"
#include "Types.h"

#include <cstring>

namespace xmc
{
	// =================================================================
	// BaseTypeName
	// =================================================================
	const char* BaseTypeName(BaseTypeIds id)
	{
		switch (id) {
		case BaseTypeIds::Unresolved: return "<unresolved>";
		case BaseTypeIds::Null:       return "null";
		case BaseTypeIds::Error:      return "<error>";

		case BaseTypeIds::b:          return "b";

		case BaseTypeIds::i8:         return "i8";
		case BaseTypeIds::i16:        return "i16";
		case BaseTypeIds::i32:        return "i32";
		case BaseTypeIds::i64:        return "i64";

		case BaseTypeIds::T_U8:       return "u8";
		case BaseTypeIds::T_U16:      return "u16";
		case BaseTypeIds::T_U32:      return "u32";
		case BaseTypeIds::T_U64:      return "u64";

		case BaseTypeIds::T_F32:      return "f32";
		case BaseTypeIds::T_F64:      return "f64";

		case BaseTypeIds::d8:         return "d8";
		case BaseTypeIds::d16:        return "d16";
		case BaseTypeIds::d32:        return "d32";
		case BaseTypeIds::d64:        return "d64";

		case BaseTypeIds::c8:         return "c8";
		case BaseTypeIds::c16:        return "c16";

		case BaseTypeIds::zs8:        return "zs8";
		case BaseTypeIds::zs16:       return "zs16";
		case BaseTypeIds::zsu:        return "zsu";

		case BaseTypeIds::s8:         return "s8";
		case BaseTypeIds::s16:        return "s16";
		case BaseTypeIds::su:         return "su";

		case BaseTypeIds::ustruct:    return "ustruct";
		case BaseTypeIds::list:       return "list";
		case BaseTypeIds::queue:      return "queue";
		case BaseTypeIds::stack:      return "stack";
		case BaseTypeIds::btree:      return "btree";
		case BaseTypeIds::hashset:    return "hashset";
		case BaseTypeIds::hashmap:    return "hashmap";
		case BaseTypeIds::function:   return "function";
		case BaseTypeIds::uclass:     return "uclass";
		case BaseTypeIds::arena:      return "arena";
		case BaseTypeIds::heap:       return "heap";
		}
		return "<bad-type>";
	}

	// =================================================================
	// BaseTypeFromKeyword
	//
	// Maps the source spelling of a primitive type keyword onto its
	// BaseTypeIds value. The match is exact (no whitespace, no decoration);
	// caller has already isolated the keyword text from the token.
	// =================================================================
	BaseTypeIds BaseTypeFromKeyword(std::string_view kw)
	{
		// Order is by frequency in typical hello-world-ish code. A
		// hash-table version is straightforward if this ever shows up
		// on a profile, but for ~30 keywords the linear scan is fine.
		struct Entry { const char* spelling; BaseTypeIds id; };
		static const Entry table[] = {
			{ "u8",  BaseTypeIds::T_U8  },
			{ "u16", BaseTypeIds::T_U16 },
			{ "u32", BaseTypeIds::T_U32 },
			{ "u64", BaseTypeIds::T_U64 },
			{ "i8",  BaseTypeIds::i8   },
			{ "i16", BaseTypeIds::i16  },
			{ "i32", BaseTypeIds::i32  },
			{ "i64", BaseTypeIds::i64  },
			{ "b",   BaseTypeIds::b    },
			{ "f32", BaseTypeIds::T_F32 },
			{ "f64", BaseTypeIds::T_F64 },
			{ "d8",  BaseTypeIds::d8   },
			{ "d16", BaseTypeIds::d16  },
			{ "d32", BaseTypeIds::d32  },
			{ "d64", BaseTypeIds::d64  },
			{ "c8",  BaseTypeIds::c8   },
			{ "c16", BaseTypeIds::c16  },
			{ "zs8", BaseTypeIds::zs8  },
			{ "zs16",BaseTypeIds::zs16 },
			{ "zsu", BaseTypeIds::zsu  },
			{ "s8",  BaseTypeIds::s8   },
			{ "s16", BaseTypeIds::s16  },
			{ "su",  BaseTypeIds::su   },
		};
		for (const Entry& e : table) {
			if (kw.size() == std::strlen(e.spelling) &&
				std::memcmp(kw.data(), e.spelling, kw.size()) == 0)
			{
				return e.id;
			}
		}
		return BaseTypeIds::Unresolved;
	}

	// =================================================================
	// AllocationTypeName
	// =================================================================
	const char* AllocationTypeName(AllocationType t)
	{
		switch (t) {
		case AllocationType::Stack:   return "stack";
		case AllocationType::SysHeap: return "sysheap";
		case AllocationType::UHeap:   return "uheap";
		case AllocationType::Arena:   return "arena";
		}
		return "<bad-alloc>";
	}

	// =================================================================
	// ScopeTypeName
	// =================================================================
	const char* ScopeTypeName(ScopeTypes t)
	{
		switch (t) {
		case ScopeTypes::File:      return "File";
		case ScopeTypes::Namespace: return "Namespace";
		case ScopeTypes::Function:  return "Function";
		case ScopeTypes::Class:     return "Class";
		case ScopeTypes::IfThen:    return "IfThen";
		case ScopeTypes::For:       return "For";
		case ScopeTypes::While:     return "While";
		case ScopeTypes::Do:        return "Do";
		case ScopeTypes::Nested:    return "Nested";
		case ScopeTypes::Shadow:    return "Shadow";
		}
		return "<bad-scope>";
	}

} // namespace xmc