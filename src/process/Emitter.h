#pragma once
#include <vector>
#include "data/EmitterData.h"

namespace process
{
	void UpdateXmoCode(std::vector<data::Xmo*>& xmos, uint32_t maxThreads);
	void WriteToCoff(const std::vector<data::Xmo*>& xmos, const std::string& outputPath);
}