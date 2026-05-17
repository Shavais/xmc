#pragma once

#include <cstdint>
#include <vector>

namespace xmc
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
			prologue         = 0,   // sub rsp, imm8   (patch: totalStackSize)
			epilogue         = 1,   // add rsp, imm8; ret  (patch: totalStackSize)
			lea_rcx_rel      = 2,   // lea rcx, [rip+rel32]  (relocation)
			call_rel32       = 3,   // call rel32            (relocation)
			xor_eax_eax      = 4,   // xor eax, eax

			mov_ecx_imm32    = 5,   // mov ecx,  imm32       (patch: intValue low 32 bits)
			mov_rcx_rsp_off8 = 6,   // mov rcx,  [rsp+disp8] (patch: Symbol::offset)
			lea_rdx_rip32    = 7,   // lea rdx,  [rip+rel32] (relocation to string data)
			mov_r8d_imm32    = 8,   // mov r8d,  imm32       (patch: intValue low 32 bits)
			lea_r9_rsp_off8  = 9,   // lea r9,   [rsp+disp8] (patch: intValue = address-of offset)
			mov_rsp32_imm0   = 10,  // mov qword [rsp+32], 0 (5th arg = null, no patch)
			mov_rsp_off8_rax = 11   // mov [rsp+disp8], rax  (patch: Symbol::offset)
		;
	} bid;

	inline std::vector<CodeBlock> CodeBlocks =
	{
		// [id: prologue, reserve: 0x10 (RSP), release: 0x00, patch: 3]
		// 48 83 EC [XX] -> sub rsp, XX
		{
			0x10,           // reserveMask (RSP)
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
			0,              // altTableIndex (last in chain)
			1,              // cost
			7,              // codeSize
			3,              // patchOffset (displacement starts at byte 3)
			{ 0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00 }
		},

		// [id: call_rel32 (3), reserve: 0x00, release: 0x00, patch: 1]
		// E8 [00 00 00 00] -> call ?
		{
			0x00,           // reserveMask (standard calls handle their own regs)
			0,              // altTableIndex
			10,				// cost
			5,              // codeSize
			1,              // patchOffset (displacement starts at byte 1)
			{ 0xE8, 0x00, 0x00, 0x00, 0x00 }
		},

		// [id: xor_eax_eax (4)] - Cost 0: Special case in x64, zero-latency on most CPUs
		// 33 C0 is the standard 2-byte form
		{
			0x01,           // reserveMask (RAX/EAX)
			0,              // altTableIndex
			0,              // cost
			2,              // codeSize
			0xFF,           // patchOffset (no patch)
			{ 0x33, 0xC0 }
		},

		// [id: mov_ecx_imm32 (5)]
		// B9 [imm32] -> mov ecx, imm32   (zero-extends to rcx)
		// Patch: 4-byte little-endian immediate from IntLit.intValue
		{
			0x02,           // reserveMask (RCX)
			0,              // altTableIndex
			1,              // cost
			5,              // codeSize
			1,              // patchOffset
			{ 0xB9, 0x00, 0x00, 0x00, 0x00 }
		},

		// [id: mov_rcx_rsp_off8 (6)]
		// 48 8B 4C 24 [off8] -> mov rcx, [rsp+disp8]
		// Patch: 1-byte disp8 from Symbol::offset
		{
			0x02,           // reserveMask (RCX)
			0,              // altTableIndex
			1,              // cost
			5,              // codeSize
			4,              // patchOffset
			{ 0x48, 0x8B, 0x4C, 0x24, 0x00 }
		},

		// [id: lea_rdx_rip32 (7)]
		// 48 8D 15 [rel32] -> lea rdx, [rip+disp32]
		// Patch: 4-byte RIP-relative displacement (relocation record)
		{
			0x04,           // reserveMask (RDX)
			0,              // altTableIndex
			2,              // cost
			7,              // codeSize
			3,              // patchOffset
			{ 0x48, 0x8D, 0x15, 0x00, 0x00, 0x00, 0x00 }
		},

		// [id: mov_r8d_imm32 (8)]
		// 41 B8 [imm32] -> mov r8d, imm32   (zero-extends to r8)
		// Patch: 4-byte little-endian immediate from intValue (e.g. string length)
		{
			0x0100,         // reserveMask (R8)
			0,              // altTableIndex
			1,              // cost
			6,              // codeSize
			2,              // patchOffset
			{ 0x41, 0xB8, 0x00, 0x00, 0x00, 0x00 }
		},

		// [id: lea_r9_rsp_off8 (9)]
		// 4C 8D 4C 24 [off8] -> lea r9, [rsp+disp8]
		// Patch: 1-byte disp8 from intValue (address-of offset stored by Coder)
		{
			0x0200,         // reserveMask (R9)
			0,              // altTableIndex
			1,              // cost
			5,              // codeSize
			4,              // patchOffset
			{ 0x4C, 0x8D, 0x4C, 0x24, 0x00 }
		},

		// [id: mov_rsp32_imm0 (10)]
		// 48 C7 44 24 20 00 00 00 00 -> mov qword [rsp+32], 0
		// Used for the 5th call argument (null).  No patch needed.
		{
			0x10,           // reserveMask (RSP implicit)
			0,              // altTableIndex
			2,              // cost
			9,              // codeSize
			0xFF,           // patchOffset (no patch)
			{ 0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00 }
		},

		// [id: mov_rsp_off8_rax (11)]
		// 48 89 44 24 [off8] -> mov [rsp+disp8], rax
		// Patch: 1-byte disp8 from declaredSymbol->offset
		{
			0x01,           // reserveMask (RAX source)
			0,              // altTableIndex
			1,              // cost
			5,              // codeSize
			4,              // patchOffset
			{ 0x48, 0x89, 0x44, 0x24, 0x00 }
		},

	};

}