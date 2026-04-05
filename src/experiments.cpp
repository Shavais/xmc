#include "pch.h"
#include "experiments.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <windows.h>

#include "tool/Coff.h"

int Hello() {

	std::cout << "RawSymbol size: " << sizeof(RawSymbol) << std::endl;

	IMAGE_FILE_HEADER header = { 0 };
	header.Machine = IMAGE_FILE_MACHINE_AMD64;
	header.NumberOfSections = 2;
	header.Characteristics = IMAGE_FILE_LINE_NUMS_STRIPPED;

	IMAGE_SECTION_HEADER textSec = { 0 }, dataSec = { 0 };
	memcpy(textSec.Name, ".text", 6);
	textSec.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

	memcpy(dataSec.Name, ".data", 6);
	dataSec.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

	std::string msg = "Hello from svl!\n";
	std::vector<uint8_t> dataBytes(msg.begin(), msg.end());
	dataBytes.push_back(0); // Null terminator
	dataSec.SizeOfRawData = (DWORD)dataBytes.size();

	// Machine Code using Indirect Calls (FF 15) for __imp symbols
	std::vector<uint8_t> code = {
		/* 00 */ 0x48, 0x83, 0xEC, 0x28,                     // sub rsp, 40
		/* 04 */ 0x48, 0xC7, 0xC1, 0xF5, 0xFF, 0xFF, 0xFF,   // mov rcx, STD_OUTPUT_HANDLE
		/* 11 */ 0xFF, 0x15,                                 // call [rel...]
		/* 13 */ 0x00, 0x00, 0x00, 0x00,                     // RELOC 1: GetStdHandle
		/* 17 */ 0x48, 0x89, 0xC1,                           // mov rcx, rax
		/* 20 */ 0x48, 0x8D, 0x15,                           // lea rdx, [rel...]
		/* 23 */ 0x00, 0x00, 0x00, 0x00,                     // RELOC 2: msg
		/* 27 */ 0x41, 0xB8, (uint8_t)msg.size(), 0x00, 0x00, 0x00, // mov r8d, len
		/* 33 */ 0x4C, 0x8D, 0x4C, 0x24, 0x20,               // lea r9, [rsp+32]
		/* 38 */ 0x6A, 0x00,                                 // push 0
		/* 40 */ 0x48, 0x83, 0xEC, 0x08,                     // sub rsp, 8
		/* 44 */ 0xFF, 0x15,                                 // call [rel...]
		/* 46 */ 0x00, 0x00, 0x00, 0x00,                     // RELOC 3: WriteFile
		/* 50 */ 0x48, 0x83, 0xC4, 0x38,                     // add rsp, 56
		/* 54 */ 0x31, 0xC0, 0xC3                            // xor eax, eax; ret
	};
	textSec.SizeOfRawData = (DWORD)code.size();

	std::vector<IMAGE_RELOCATION> relocs = {
	{ 13, 3, IMAGE_REL_AMD64_REL32 }, // call [GetStdHandle]
	{ 23, 2, IMAGE_REL_AMD64_REL32 }, // lea rdx, [msg]
	{ 46, 4, IMAGE_REL_AMD64_REL32 }  // call [WriteFile]
	};
	textSec.NumberOfRelocations = (WORD)relocs.size();

	// String table & Symbols
	std::vector<char> stringTable = { 0, 0, 0, 0 };
	auto addString = [&](const std::string& s) {
		uint32_t offset = (uint32_t)stringTable.size();
		for (char c : s) stringTable.push_back(c);
		stringTable.push_back(0);
		return offset;
		};

	std::vector<RawSymbol> symbols(5, { 0 });
	memcpy(symbols[0].Name.ShortName, "main", 5);  symbols[0].SectionNumber = 1; symbols[0].StorageClass = IMAGE_SYM_CLASS_EXTERNAL;
	memcpy(symbols[1].Name.ShortName, ".data", 6); symbols[1].SectionNumber = 2; symbols[1].StorageClass = IMAGE_SYM_CLASS_STATIC;
	memcpy(symbols[2].Name.ShortName, "msg", 4);   symbols[2].SectionNumber = 2; symbols[2].StorageClass = IMAGE_SYM_CLASS_STATIC;

	symbols[3].Name.LongName.Zeroes = 0;
	symbols[3].Name.LongName.Offset = addString("__imp_GetStdHandle");
	symbols[3].StorageClass = IMAGE_SYM_CLASS_EXTERNAL;

	symbols[4].Name.LongName.Zeroes = 0;
	symbols[4].Name.LongName.Offset = addString("__imp_WriteFile");
	symbols[4].StorageClass = IMAGE_SYM_CLASS_EXTERNAL;

	*(uint32_t*)stringTable.data() = (uint32_t)stringTable.size();
	header.NumberOfSymbols = (DWORD)symbols.size();

	// One-time Offset Calculation (CRITICAL)
	DWORD current = sizeof(header) + (sizeof(IMAGE_SECTION_HEADER) * 2);
	textSec.PointerToRawData = current;
	current += (DWORD)code.size();
	dataSec.PointerToRawData = current;
	current += (DWORD)dataBytes.size();
	textSec.PointerToRelocations = current;
	current += (DWORD)(sizeof(IMAGE_RELOCATION) * relocs.size());
	header.PointerToSymbolTable = current;

	std::ofstream ofs("test.obj", std::ios::binary);
	ofs.write((char*)&header, sizeof(header));
	ofs.write((char*)&textSec, sizeof(textSec));
	ofs.write((char*)&dataSec, sizeof(dataSec));
	ofs.write((char*)code.data(), code.size());
	ofs.write((char*)dataBytes.data(), dataBytes.size());
	ofs.write((char*)relocs.data(), sizeof(IMAGE_RELOCATION) * relocs.size());
	ofs.write((char*)symbols.data(), sizeof(RawSymbol) * symbols.size());
	ofs.write(stringTable.data(), stringTable.size());
	ofs.close();

	return 0;
}


typedef void (*PayloadFunc)();

void RunInMemory() {
	std::string msg = "Hello from memory!\n";
	std::vector<uint8_t> dataBytes(msg.begin(), msg.end());
	dataBytes.push_back(0);

	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	uint64_t addrGetStdHandle = (uint64_t)GetProcAddress(k32, "GetStdHandle");
	uint64_t addrWriteFile = (uint64_t)GetProcAddress(k32, "WriteFile");

	size_t totalSize = 4096;
	uint8_t* mem = (uint8_t*)VirtualAlloc(NULL, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!mem) return;

	uint8_t* codePtr = mem;
	uint8_t* dataPtr = mem + 100;
	uint64_t* iatPtr = (uint64_t*)(mem + 200);

	iatPtr[0] = addrGetStdHandle;
	iatPtr[1] = addrWriteFile;
	memcpy(dataPtr, dataBytes.data(), dataBytes.size());

	// RIP-relative displacement helper
	auto getDisp = [&](int instrOffset, int instrLen, uint8_t* targetAddr) -> int32_t {
		uint8_t* nextInstrAddr = codePtr + instrOffset + instrLen;
		return (int32_t)(targetAddr - nextInstrAddr);
		};

	std::vector<uint8_t> code = {
		// 1. Setup Stack Frame (56 bytes: 32 shadow + 8 for 5th arg + 16 for alignment)
		0x48, 0x83, 0xEC, 0x38,                     // 0: sub rsp, 56

		// 2. Call GetStdHandle
		0x48, 0xC7, 0xC1, 0xF5, 0xFF, 0xFF, 0xFF,   // 4: mov rcx, -11 (STD_OUTPUT_HANDLE)
		0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,         // 11: call [rel iatPtr[0]] (6 bytes)

		// 3. Prepare WriteFile Args
		0x48, 0x89, 0xC1,                           // 17: mov rcx, rax (Handle)
		0x48, 0x8D, 0x15, 0x00, 0x00, 0x00, 0x00,   // 20: lea rdx, [rel dataPtr] (7 bytes)
		0x41, 0xB8, (uint8_t)msg.size(), 0x00, 0x00, 0x00, // 27: mov r8d, len

		// Point R9 to the allocated shadow space on stack to store bytes written
		0x4C, 0x8D, 0x4C, 0x24, 0x20,               // 33: lea r9, [rsp+32] 

		// Move 5th argument (LPOVERLAPPED = NULL) to [RSP + 40] (above shadow space)
		0x48, 0xC7, 0x44, 0x24, 0x28, 0x00, 0x00, 0x00, 0x00, // 38: mov qword ptr [rsp+40], 0

		// 4. Call WriteFile
		0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,         // 47: call [rel iatPtr[1]] (6 bytes)

		// 5. Cleanup and Return
		0x48, 0x83, 0xC4, 0x38,                     // 53: add rsp, 56
		0x31, 0xC0, 0xC3                            // 57: xor eax, eax; ret
	};

	// Apply relocations targeting our internal IAT
	*(int32_t*)&code[13] = getDisp(11, 6, (uint8_t*)&iatPtr[0]);
	*(int32_t*)&code[23] = getDisp(20, 7, dataPtr);
	*(int32_t*)&code[49] = getDisp(47, 6, (uint8_t*)&iatPtr[1]);

	memcpy(codePtr, code.data(), code.size());

	// Execute the code
	((PayloadFunc)codePtr)();

	VirtualFree(mem, 0, MEM_RELEASE);
}


void RunCppFunction() 
{
	IMAGE_FILE_HEADER header = { 0 };
	
	header.Machine = IMAGE_FILE_MACHINE_AMD64;
	header.NumberOfSections = 2;
	header.Characteristics = IMAGE_FILE_LINE_NUMS_STRIPPED;

	IMAGE_SECTION_HEADER textSec = { 0 }, dataSec = { 0 };
	
	memcpy(textSec.Name, ".text", 6);
	textSec.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

	memcpy(dataSec.Name, ".data", 6);
	dataSec.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

	std::string msg = "Hello from svl!\n";
	std::vector<uint8_t> dataBytes(msg.begin(), msg.end());
	dataBytes.push_back(0); // Null terminator
	dataSec.SizeOfRawData = (DWORD)dataBytes.size();

	// Call SVGE_Log
	std::vector<uint8_t> code = {
		0x48, 0x83, 0xEC, 0x28,							// sub rsp, 40
		0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00,		// lea rcx, [rel msg] (Reloc @ 7)
		0xE8, 0x00, 0x00, 0x00, 0x00,					// call SVGE_Log (Reloc @ 12)
		0x48, 0x83, 0xC4, 0x28,							// add rsp, 40
		0x31, 0xC0, 0xC3								// xor eax, eax; ret
	};	
	
	textSec.SizeOfRawData = (DWORD)code.size();

	std::vector<IMAGE_RELOCATION> relocs = {
		{7, 2, IMAGE_REL_AMD64_REL32},
		{12, 1, IMAGE_REL_AMD64_REL32}
	};

	textSec.NumberOfRelocations = (WORD)relocs.size();

	// String table & Helper
	std::vector<char> stringTable = { 0, 0, 0, 0 };
	auto addString = [&](const std::string& s) {
		uint32_t offset = (uint32_t)stringTable.size();
		for (char c : s) stringTable.push_back(c);
		stringTable.push_back(0);
		return offset;
		};

	// Symbols
	std::vector<RawSymbol> symbols(3, { 0 });

	// symbol[0]: main (defined in this object)
	memcpy(symbols[0].Name.ShortName, "main", 5);
	symbols[0].SectionNumber = 1;
	symbols[0].StorageClass = IMAGE_SYM_CLASS_EXTERNAL;

	// symbol[1]: SVGE_Log (external/undefined)
	symbols[1].Name.LongName.Zeroes = 0;
	symbols[1].Name.LongName.Offset = addString("SVGE_Log");
	symbols[1].SectionNumber = 0; // 0 = IMAGE_SYM_UNDEFINED
	symbols[1].StorageClass = IMAGE_SYM_CLASS_EXTERNAL;

	// symbols[2]: 
	memcpy(symbols[2].Name.ShortName, "msg", 4);
	symbols[2].SectionNumber = 2; // It lives in the .data section
	symbols[2].StorageClass = IMAGE_SYM_CLASS_STATIC;

	// Finalize header
	header.NumberOfSymbols = (DWORD)symbols.size();

	// One-time Offset Calculation 
	DWORD current = sizeof(header) + (sizeof(IMAGE_SECTION_HEADER) * 2);
	textSec.PointerToRawData = current;
	current += (DWORD)code.size();
	dataSec.PointerToRawData = current;
	current += (DWORD)dataBytes.size();
	textSec.PointerToRelocations = current;
	current += (DWORD)(sizeof(IMAGE_RELOCATION) * relocs.size());
	header.PointerToSymbolTable = current;

	// Update the first 4 bytes of the string table with its total size
	*(uint32_t*)stringTable.data() = (uint32_t)stringTable.size();

	std::ofstream ofs("test2.obj", std::ios::binary);
	ofs.write((char*)&header, sizeof(header));
	ofs.write((char*)&textSec, sizeof(textSec));
	ofs.write((char*)&dataSec, sizeof(dataSec));
	ofs.write((char*)code.data(), code.size());
	ofs.write((char*)dataBytes.data(), dataBytes.size());
	ofs.write((char*)relocs.data(), sizeof(IMAGE_RELOCATION) * relocs.size());
	ofs.write((char*)symbols.data(), sizeof(RawSymbol) * symbols.size());
	ofs.write(stringTable.data(), stringTable.size());
	ofs.close();
}


void CallCppFunction() {
	Coff coff;

	// 1. Create Sections
	uint16_t textIdx = coff.CreateSection(".text", SectionType::Code);
	uint16_t dataIdx = coff.CreateSection(".data", SectionType::Data);

	// 2. Add Data and Symbols
	// "Hello" string in .data
	std::string msg = "Hello from svl!\n";
	std::vector<uint8_t> msgBytes(msg.begin(), msg.end());
	msgBytes.push_back(0); // Null terminator
	uint32_t msgSymIdx = coff.AddDataSymbol("msg", dataIdx, msgBytes);

	// External function symbol
	uint32_t logSymIdx = coff.AddExternalSymbol("SVGE_Log");

	// 3. Define Machine Code
	// We use placeholders (00 00 00 00) for the relative offsets
	std::vector<uint8_t> code = {
		0x48, 0x83, 0xEC, 0x28,                     // sub rsp, 40
		0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00,   // lea rcx, [rel msg] (Offset 7)
		0xE8, 0x00, 0x00, 0x00, 0x00,               // call SVGE_Log      (Offset 12)
		0x48, 0x83, 0xC4, 0x28,                     // add rsp, 40
		0x31, 0xC0, 0xC3                            // xor eax, eax; ret
	};

	// Add code to .text and create a symbol for the entry point
	// We can just add the bytes manually to the buffer
	// or create a method AddCodeSymbol if you prefer.
	uint32_t mainSymIdx = coff.AddDataSymbol("main", textIdx, code);

	// 4. Add Relocations
	// Use the indices returned by AddDataSymbol/AddExternalSymbol
	coff.AddRelocation(textIdx, 7, msgSymIdx, IMAGE_REL_AMD64_REL32);
	coff.AddRelocation(textIdx, 12, logSymIdx, IMAGE_REL_AMD64_REL32);

	// 5. Write to file
	coff.WriteTo("tests/hello/hello.obj");
}
