#pragma once

#include <cstdint>

namespace data
{
	// 32 bytes total:
	struct CodeBlock
	{
		uint32_t reserveMask;
		int32_t	 altTableIndex;		// 0 for the last block in the alternate chain, which is either the only available block or the spill-to-stack block.
		uint8_t  cost;
		uint16_t codeSize;
		uint8_t  patchOffset;
		uint8_t  code[19];			 
	};
}