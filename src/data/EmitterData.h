#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <variant>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

#include "XmoData.h"

// XmcProjectData.h
namespace data
{
	struct EmitterFuncContext {
		size_t prologueImmediatePos; // Offset in codeBuffer to the SUB RSP value
		uint32_t currentSpills;      // Tracks spills for this specific function
		uint32_t peakSpills;          // The peak spill count encountered
		uint32_t localVars;          // From FunctionNodeData
	};


	// Global Project State
	//inline std::unordered_map<std::string, XmoModule> Modules;
	inline std::unordered_map<std::string, SymbolEntry*> GlobalSymbolTable;
	inline std::shared_mutex SymbolTableMutex;

	// Global Flags
	inline std::atomic<bool> FullScanRequired{ false };
}
