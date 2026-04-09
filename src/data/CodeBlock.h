#pragma once

#include <cstdint>

namespace data
{
	// 64 bytes total:
	struct CodeBlock
	{
		uint32_t reserveMask;
		uint32_t releaseMask;
		uint32_t altTableIndex;		// 0 for the last block in the chain, which is the spill-to-stack block.
		uint16_t codeSize;
		uint8_t  patchOffset;
		uint8_t  code[49];			 
	};
}