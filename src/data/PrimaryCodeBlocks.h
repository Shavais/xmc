// PrimaryCodeBlocis.h - generated from PrimaryCodeBlocks.xma

#pragma once

#include <vector>
#include <cstdint>

#include "CodeBlock.h"

namespace data
{
/* 

	Standard GPR Register Mask (32-bit)

	Bit	Hex Value	Reg (64-bit)	Reg (32-bit)	Typical Role
	0	0x0001		RAX				EAX				Return Value / Accumulator
	1	0x0002		RCX				ECX				1st Argument (Windows)
	2	0x0004		RDX				EDX				2nd Argument (Windows)
	3	0x0008		RBX				EBX				Callee-Saved (Preserve!)
	4	0x0010		RSP				ESP				Stack Pointer (Always Reserved)
	5	0x0020		RBP				EBP				Base/Frame Pointer
	6	0x0040		RSI				ESI				3rd Argument (Linux) / Caller-Saved
	7	0x0080		RDI				EDI				4th Argument (Linux) / Caller-Saved
	8	0x0100		R8				R8D				3rd Argument (Windows)
	9	0x0200		R9				R9D				4th Argument (Windows)
	10	0x0400		R10				R10D			Temporary / Syscall
	11	0x0800		R11				R11D			Temporary
	12	0x1000		R12				R12D			Callee-Saved
	13	0x2000		R13				R13D			Callee-Saved
	14	0x4000		R14				R14D			Callee-Saved
	15	0x8000		R15				R15D			Callee-Saved

*/

	inline struct BlockIds 
	{
		uint16_t
			prologue = 0,
			epilogue = 1,
			lea_rcx_rel = 2,
			call_rel32 = 3,
			xor_eax_eax = 4
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
			1,				// cost
			4,              // codeSize
			3,              // patchOffset (points to the immediate byte)
			{ 0x48, 0x83, 0xEC, 0x00 } // Machine code
		},

		// [id: epilogue, reserve: 0x10 (RSP), release: 0x10, patch: 3]
		// 48 83 C4 [00] -> add rsp, 
		// C3 -> ret
		{
			0x10,           // reserveMask (RSP)
			0x10,           // releaseMask (Frees RSP)
			0,              // altTableIndex
			2,				// cost
			5,              // codeSize
			3,              // patchOffset (points to the immediate byte)
			{ 0x48, 0x83, 0xC4, 0x00, 0xC3 } // Machine code
		},

		// [id: lea_rcx_rel (2), reserve: 0x02 (RCX), release: 0x00, patch: 3]
		// 48 8D 0D [00 00 00 00] -> lea rcx, [rel ?]
		{
			0x02,           // reserveMask (RCX)
			0x00,           // releaseMask
			10,             // altTableIndex
			1,				 // cost
			7,              // codeSize
			3,              // patchOffset (displacement starts at byte 3)
			{ 0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00 }
		},

		// [id: call_rel32 (3), reserve: 0x00, release: 0x00, patch: 1]
		// E8 [00 00 00 00] -> call ?
		{
			0x00,           // reserveMask (standard calls handle their own regs)
			0x00,           // releaseMask
			0,              // altTableIndex
			10,				// cost
			5,              // codeSize
			1,              // patchOffset (displacement starts at byte 1)
			{ 0xE8, 0x00, 0x00, 0x00, 0x00 }
		},

		// [id: xor_eax_eax (4)] - Cost 0: Special case in x64, zero-latency on most CPUs
		// 33 C0 is the standard 2-byte form
		{
			0x01,			// reserveMask (RAX/EAX)
			0x01,			// releaseMask (It effectively defines it)
			0,				// altTableIndex
			0,				// Cost (Idiomatic zeroing is often free)
			2,				// codeSize
			0xFF,			// patchOffset (No patch needed)
			{ 0x33, 0xC0 }
		}


	};

}