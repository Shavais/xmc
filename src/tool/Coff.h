#pragma once

#include <vector>
#include <string>
#include <cstdint>

#define NOMINMAX
#include "Windows.h"

#pragma pack(push, 1)
struct RawSymbol {
	union {
		char ShortName[8];
		struct { uint32_t Zeroes; uint32_t Offset; } LongName;
	} Name;
	uint32_t Value;
	int16_t SectionNumber;
	uint16_t Type;
	uint8_t StorageClass;
	uint8_t NumberOfAuxSymbols;
};
#pragma pack(pop)

enum class SectionType : uint32_t {
	Code = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ,					// Basic Code (.text)
	Data = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE,		// Initialized Data (.data)
	ReadOnly = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ,							// Constant/Read-Only Data (.rdata)
	Bss = IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE		// Uninitialized Data (.bss) - No raw data in file
};

class Coff {

public:
	// Add an external function (like SVGE_Log)
	uint32_t AddExternalSymbol(const std::string& name);

	// Add data (like your "Hello" string)
	uint32_t AddDataSymbol(const std::string& name, uint16_t sectionIdx, const std::vector<uint8_t>& data);

	// Set the name of given symbol
	void SetSymbolName(RawSymbol& sym, const std::string& name);

	uint32_t PushRawSymbol(const RawSymbol& sym);

	uint32_t DefineSymbol(const std::string& name, uint32_t value, uint16_t sectionIdx, uint8_t storageClass);
		
	// Helper to add a relocation to a specific section
	void AddRelocation(uint16_t sectionIdx, uint32_t offset, uint32_t symbolIdx, uint16_t type);

	// Add cection
	uint16_t CreateSection(const std::string& name, SectionType type);

	// Get the 
	uint32_t GetSectionBufferSize(uint16_t sectionIdx);

	void AppendPadding(uint16_t sectionIdx, uint32_t alignment);

	// Write coff file to path
	void WriteTo(const std::string& path);

private:
	IMAGE_FILE_HEADER header_ = { 0 };
	std::vector<IMAGE_SECTION_HEADER> sections_;
	std::vector<std::vector<uint8_t>> sectionBuffers_;
	std::vector<std::vector<IMAGE_RELOCATION>> sectionRelocs_;
	// Automatic Symbol Management
	std::vector<RawSymbol> symbols_;
	std::vector<char> stringTable_ = { 0,0,0,0 };
};
