#pragma once
#include <vector>
#include "data/EmitterData.h"

namespace process
{
	void WriteToCoff(const std::vector<data::Xmo*>& xmos, const std::string& outputPath);
	void UpdateXmoCode();
}