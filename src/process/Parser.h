#pragma once

#include "data/ParserData.h"

using namespace data;

namespace process
{
	Symbol* GetOrCreateSymbol(const char* tempName, uint32_t scopeId);
}
