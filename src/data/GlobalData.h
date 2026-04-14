#pragma once

#include <atomic>

#include "ParserData.h"

namespace data
{
	// Global Project State
	inline std::atomic<bool> ErrorOccurred = false;

	// Global Flags
	inline std::atomic<bool> FullScanRequired{ false };
	inline uint8_t MaxThreads = 6;

} // namespace data
