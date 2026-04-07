#include "pch.h"
#include "Coff.h"

#include <algorithm>
#include <fstream>


// Add an external function (like SVGE_Log)
uint32_t Coff::AddExternalSymbol(const std::string& name) {
	RawSymbol sym = { 0 };

	// Use your existing helper to manage the name and string table
	SetSymbolName(sym, name);

	sym.SectionNumber = 0; // 0 indicates an external/undefined symbol
	sym.Value = 0;         // Usually 0 for external functions
	sym.StorageClass = IMAGE_SYM_CLASS_EXTERNAL;
	sym.NumberOfAuxSymbols = 0;

	symbols_.push_back(sym);
	return static_cast<uint32_t>(symbols_.size() - 1);
}

uint32_t Coff::AddDataSymbol(const std::string& name, uint16_t sectionIdx, const std::vector<uint8_t>& data) {
	// 1. Get the current offset
	uint32_t offset = static_cast<uint32_t>(sectionBuffers_[sectionIdx - 1].size());

	// 2. Append the raw data
	sectionBuffers_[sectionIdx - 1].insert(sectionBuffers_[sectionIdx - 1].end(), data.begin(), data.end());

	// 3. Create and Name the symbol
	RawSymbol sym = { 0 };

	// Call the helper to handle the 8-char vs String Table logic
	SetSymbolName(sym, name);

	sym.Value = offset;
	sym.SectionNumber = sectionIdx;

	// Use EXTERNAL if you want this data to be visible to the linker/other files
	sym.StorageClass = IMAGE_SYM_CLASS_EXTERNAL;

	symbols_.push_back(sym);
	return static_cast<uint32_t>(symbols_.size() - 1);
}

void Coff::SetSymbolName(RawSymbol& sym, const std::string& name) {
	if (name.length() <= 8) {
		memset(sym.Name.ShortName, 0, 8);
		memcpy(sym.Name.ShortName, name.c_str(), name.length());
	}
	else {
		// MUST be zero to signal "Look in string table"
		sym.Name.LongName.Zeroes = 0;

		// Offset is current size of table BEFORE adding this string
		sym.Name.LongName.Offset = (uint32_t)stringTable_.size();

		// Add string + null terminator
		for (char c : name) stringTable_.push_back(c);
		stringTable_.push_back('\0');
	}
}

// returns the 1-based section index
uint16_t Coff::CreateSection(const std::string& name, SectionType type) {
	IMAGE_SECTION_HEADER sh = { 0 };

	// Copy up to 8 bytes. Since sh was zeroed, names < 8 chars are null-padded.
	size_t len = (name.length() > 8) ? 8 : name.length();
	memcpy(sh.Name, name.c_str(), len);

	switch (type) {
	case SectionType::Code:
		sh.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
		break;
	case SectionType::Data:
		sh.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
		break;
	case SectionType::ReadOnly:
		sh.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
		break;
	case SectionType::Bss:
		sh.Characteristics = IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
		break;
	}

	// It is highly recommended to add an alignment flag (e.g. 16 bytes) 
	// so the linker doesn't pack your data too tightly.
	sh.Characteristics |= IMAGE_SCN_ALIGN_16BYTES;

	sections_.push_back(sh);
	sectionBuffers_.push_back({});
	sectionRelocs_.push_back({}); 

	return (uint16_t)sections_.size();
}

void Coff::AddRelocation(uint16_t sectionIdx, uint32_t offset, uint32_t symbolIdx, uint16_t type) {
	IMAGE_RELOCATION rel = { 0 };
	rel.VirtualAddress = offset;     // Offset within the section data
	rel.SymbolTableIndex = symbolIdx; // The index in your symbols_ vector
	rel.Type = type;                 // e.g., IMAGE_REL_AMD64_REL32

	// sectionIdx is 1-based
	sectionRelocs_[sectionIdx - 1].push_back(rel);

	// Update the section header's count
	sections_[sectionIdx - 1].NumberOfRelocations++;
}

void Coff::WriteTo(const std::string& path) {
	std::ofstream f(path, std::ios::binary);

	// 1. Prepare Header
	IMAGE_FILE_HEADER fileHeader = { 0 };
	fileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
	fileHeader.NumberOfSections = (uint16_t)sections_.size();
	fileHeader.NumberOfSymbols = (uint32_t)symbols_.size();
	fileHeader.SizeOfOptionalHeader = 0; // Standard for COFF object files
	fileHeader.Characteristics = 0;

	// Helper to align offsets to 512-byte boundaries (Standard for Windows loaders)
	auto Align = [](uint32_t offset, uint32_t alignment) {
		return (offset + alignment - 1) & ~(alignment - 1);
		};

	// 2. PASS 1: Calculate all File Offsets
	// Start after the File Header and all Section Headers
	uint32_t currentOffset = sizeof(IMAGE_FILE_HEADER) + (uint32_t)(sections_.size() * sizeof(IMAGE_SECTION_HEADER));

	// Calculate Raw Data Offsets
	for (size_t i = 0; i < sections_.size(); ++i) {
		if (!sectionBuffers_[i].empty()) {
			// Align start of each section for file compliance
			currentOffset = Align(currentOffset, 512);
			sections_[i].PointerToRawData = currentOffset;
			sections_[i].SizeOfRawData = (uint32_t)sectionBuffers_[i].size();
			currentOffset += sections_[i].SizeOfRawData;
		}
		else {
			sections_[i].PointerToRawData = 0;
			sections_[i].SizeOfRawData = 0;
		}
	}

	// Calculate Relocation Offsets
	for (size_t i = 0; i < sections_.size(); ++i) {
		if (!sectionRelocs_[i].empty()) {
			// Relocations don't strictly require 512-alignment, but 4-byte is safe
			currentOffset = Align(currentOffset, 4);
			sections_[i].PointerToRelocations = currentOffset;
			sections_[i].NumberOfRelocations = (uint16_t)sectionRelocs_[i].size();
			currentOffset += (uint32_t)(sectionRelocs_[i].size() * sizeof(IMAGE_RELOCATION));
		}
		else {
			sections_[i].PointerToRelocations = 0;
			sections_[i].NumberOfRelocations = 0;
		}
	}

	// Calculate Symbol Table Offset
	currentOffset = Align(currentOffset, 4);
	fileHeader.PointerToSymbolTable = currentOffset;

	// 3. PASS 2: Physical Write Phase
	// Write Header and (now fully updated) Section Headers
	f.write((char*)&fileHeader, sizeof(fileHeader));
	f.write((char*)sections_.data(), sections_.size() * sizeof(IMAGE_SECTION_HEADER));

	// Write Raw Data with padding
	for (size_t i = 0; i < sections_.size(); ++i) {
		if (!sectionBuffers_[i].empty()) {
			// Fill the gap between previous data and this section start
			long currentPos = (long)f.tellp();
			int padding = sections_[i].PointerToRawData - currentPos;
			if (padding > 0) {
				std::vector<char> pad(padding, 0);
				f.write(pad.data(), padding);
			}
			f.write((char*)sectionBuffers_[i].data(), sectionBuffers_[i].size());
		}
	}

	// Write Relocation Tables
	for (size_t i = 0; i < sections_.size(); ++i) {
		if (!sectionRelocs_[i].empty()) {
			long currentPos = (long)f.tellp();
			int padding = sections_[i].PointerToRelocations - currentPos;
			if (padding > 0) {
				std::vector<char> pad(padding, 0);
				f.write(pad.data(), padding);
			}
			f.write((char*)sectionRelocs_[i].data(), sectionRelocs_[i].size() * sizeof(IMAGE_RELOCATION));
		}
	}

	// Write Symbols
	long symPos = (long)f.tellp();
	int symPadding = fileHeader.PointerToSymbolTable - symPos;
	if (symPadding > 0) {
		std::vector<char> pad(symPadding, 0);
		f.write(pad.data(), symPadding);
	}
	f.write((char*)symbols_.data(), symbols_.size() * sizeof(RawSymbol));

	// Finalize and Write String Table
	// Prefix the table with its total size (including the 4 bytes for the size itself)
	uint32_t totalStringTableSize = static_cast<uint32_t>(stringTable_.size());
	memcpy(stringTable_.data(), &totalStringTableSize, 4);
	f.write(stringTable_.data(), stringTable_.size());

	f.close();
}

//void Coff::WriteTo(const std::string& path) {
//	std::ofstream f(path, std::ios::binary);
//
//	auto align = [](uint32_t offset, uint32_t alignment) {
//		return (offset + alignment - 1) & ~(alignment - 1);
//	};
//
//	// 1. Prepare Header
//	IMAGE_FILE_HEADER fileHeader = { 0 };
//	fileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
//	fileHeader.NumberOfSections = (uint16_t)sections_.size();
//	fileHeader.NumberOfSymbols = (uint32_t)symbols_.size();
//
//	// 2. Calculate Offsets 
//	uint32_t currentOffset = sizeof(IMAGE_FILE_HEADER) + (uint32_t)(sections_.size() * sizeof(IMAGE_SECTION_HEADER));
//
//	// First: All Raw Data
//	for (size_t i = 0; i < sections_.size(); ++i) {
//		if (!sectionBuffers_[i].empty()) {
//			sections_[i].PointerToRawData = currentOffset;
//			sections_[i].SizeOfRawData = (uint32_t)sectionBuffers_[i].size();
//			currentOffset = align(currentOffset + sections_[i].SizeOfRawData, 4);
//		}
//		else {
//			sections_[i].PointerToRawData = 0;
//			sections_[i].SizeOfRawData = 0;
//		}
//	}
//
//	// Second: All Relocations
//	for (size_t i = 0; i < sections_.size(); ++i) {
//		if (!sectionRelocs_[i].empty()) {
//			sections_[i].PointerToRelocations = currentOffset;
//			sections_[i].NumberOfRelocations = (uint16_t)sectionRelocs_[i].size();
//			currentOffset += (uint32_t)(sectionRelocs_[i].size() * sizeof(IMAGE_RELOCATION));
//		}
//		else {
//			sections_[i].PointerToRelocations = 0;
//			sections_[i].NumberOfRelocations = 0;
//		}
//	}
//
//	fileHeader.PointerToSymbolTable = currentOffset;
//
//	// 3. Physical Write Phase
//	f.write((char*)&fileHeader, sizeof(fileHeader));
//	f.write((char*)sections_.data(), sections_.size() * sizeof(IMAGE_SECTION_HEADER));
//
//	// Write all Raw Data buffers sequentially (No padding)
//	for (size_t i = 0; i < sections_.size(); ++i) {
//		if (!sectionBuffers_[i].empty()) {
//			f.write((char*)sectionBuffers_[i].data(), sectionBuffers_[i].size());
//		}
//	}
//
//	// Write all Relocation tables sequentially (No padding)
//	for (size_t i = 0; i < sections_.size(); ++i) {
//		if (!sectionRelocs_[i].empty()) {
//			f.write((char*)sectionRelocs_[i].data(), sectionRelocs_[i].size() * sizeof(IMAGE_RELOCATION));
//		}
//	}
//
//	// Write Symbols and String Table
//	uint32_t totalSize = static_cast<uint32_t>(stringTable_.size());
//	memcpy(stringTable_.data(), &totalSize, 4);
//
//	f.write((char*)symbols_.data(), symbols_.size() * sizeof(RawSymbol));
//	f.write((char*)stringTable_.data(), stringTable_.size());
//
//	f.close();
//}