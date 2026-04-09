// PrimaryCodeBlocis.h - generated from PrimaryCodeBlocks.xma

#pragma once

#include <vector>
#include <cstdint>

#include "CodeBlock.h"

namespace data
{
	inline struct BlockIds 
	{
		uint16_t
			prologue = 0,
			epilogue = 1,
			lea_rcx_rel = 2,
			call_rel32 = 3
		;
	} bid;

	inline std::vector<CodeBlock> PrimaryCodeBlocks =
	{
		// [id: prologue, reserve: 0x10 (RSP), release: 0x00, patch: 3]
		// 48 83 EC [XX] -> sub rsp, XX
		{
			0x10,           // reserveMask (RSP)
			0x00,           // releaseMask
			0,              // altTableIndex (Last in chain)
			4,              // codeSize
			3,              // patchOffset (points to the immediate byte)
			{ 0x48, 0x83, 0xEC, 0x00 } // Machine code
		},

		// [id: epilogue, reserve: 0x10 (RSP), release: 0x10, patch: 3]
		// 48 83 C4 [XX]; C3 -> add rsp, XX; ret
		{
			0x10,           // reserveMask (RSP)
			0x10,           // releaseMask (Frees RSP)
			0,              // altTableIndex
			5,              // codeSize
			3,              // patchOffset (points to the immediate byte)
			{ 0x48, 0x83, 0xC4, 0x00, 0xC3 } // Machine code
		},

		// [id: call, reserve:, release:, patch: ]
		{
		}
	};

}