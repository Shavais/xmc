#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <variant>
#include <vector>
#include <unordered_map>
#include <shared_mutex>


// XmcProjectData.h
namespace data
{
	struct ParseTreeNode
	{
		NodeType;
	};

	enum BaseTypeID : uint32_t 
	{
		T_VOID = 0,
		T_B8,                          // boolean
		T_I8, T_I16, T_I32, T_I64,     // signed
		T_U8, T_U16, T_U32, T_U64,     // unsigned
		T_F32, T_F64,                  // float
		T_C8, T_C16,                   // char
		T_ZS8, T_ZS16,                 // zero-terminated strings
		T_USER_DEFINED = 0x80000000    // High bit marks a class/struct index
	};

	// The "8-bit" Refinement Mask
	enum class RefinementBits : uint8_t 
	{
		Mutable			= 0x01,				// the bit is 1/true if the symbol refers to something that is mutable, 0 if it refers to something that is immutable
		Resizable		= 0x02,				// ..
		Virtual			= 0x04, 
		Arc				= 0x08,
		Heterogenous	= 0x10, 
		Fluid			= 0x20, 
		Concurrent		= 0x40, 
		Variant			= 0x80
	};

	enum class AllocationType { Stack, OsHeap, AppHeap, Arena};	

	struct SymbolEntry 
	{
		std::string Name;
		std::atomic<uint32_t> BaseType;
		std::atomic<uint8_t> RMask;
		AllocationType 
		uint32_t ScopeId;
	};

	enum class BlockTypes : uint8_t
	{
		File,
		Namespace,
		Function,
		Class,
		IfThen,
		For,
		While,
		DoWhile,
		Nested,
		Shadow
	};

	struct Scope
	{
		uint32_t ScopeId;
		BlockTypes BlockType;
		Scope* Parent;
		std::vector<Scope*> Children;
		std::vector<SymbolEntry> Symbols;
	};

	struct XmoModule {
		std::string SourcePath;
		std::string XmoPath;
		bool SourceModified = false;
		bool RMaskModified = false;
		bool FullAstLoaded = false;

		// The symbols owned by this specific module
		std::vector<std::unique_ptr<SymbolEntry>> LocalSymbols;

		// The actual binary blocks for the .obj
		std::vector<uint8_t> MachineCodeBuffer;
	};

	// Global Project State
	inline std::unordered_map<std::string, XmoModule> Modules;
	inline std::unordered_map<std::string, SymbolEntry*> GlobalSymbolTable;
	inline std::shared_mutex SymbolTableMutex;

	// Global Flags
	inline std::atomic<bool> FullScanRequired{ false };
}
