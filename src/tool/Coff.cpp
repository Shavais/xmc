#include "pch/pch.h"
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

uint32_t Coff::AddDataSymbol(const std::string& name, uint16_t sectionIdx, const std::vector<uint8_t>& data,
                             uint8_t storageClass) {
	uint32_t offset = static_cast<uint32_t>(sectionBuffers_[sectionIdx - 1].size());
	sectionBuffers_[sectionIdx - 1].insert(sectionBuffers_[sectionIdx - 1].end(), data.begin(), data.end());

	RawSymbol sym = { 0 };
	SetSymbolName(sym, name);
	sym.Value = offset;
	sym.SectionNumber = sectionIdx;
	sym.StorageClass = storageClass;

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

uint32_t Coff::PushRawSymbol(const RawSymbol& sym) 
{
	symbols_.push_back(sym);
	return static_cast<uint32_t>(symbols_.size() - 1);
}

void Coff::AddRawBytes(uint16_t sectionIdx, const std::vector<uint8_t>& data) {
	// sectionIdx is 1-based, so we subtract 1 for the vector access
	if (sectionIdx > 0 && sectionIdx <= sectionBuffers_.size()) {
		sectionBuffers_[sectionIdx - 1].insert(
			sectionBuffers_[sectionIdx - 1].end(),
			data.begin(),
			data.end()
		);
	}
}
uint32_t Coff::DefineSymbol(const std::string& name, uint32_t value, uint16_t sectionIdx, uint8_t storageClass) {
	RawSymbol sym = { 0 };
	SetSymbolName(sym, name);
	sym.Value = value;
	sym.SectionNumber = sectionIdx;
	sym.StorageClass = storageClass;
	sym.NumberOfAuxSymbols = 0;
	sym.Type = 0; // Default to 'no type'

	return PushRawSymbol(sym);
}

// returns the 1-based section index
uint16_t Coff::CreateSection(const std::string& name, SectionType type) {
	IMAGE_SECTION_HEADER sh = { 0 };

	// Copy up to 8 bytes. Since sh was zeroed, names < 8 chars are null-padded.
	uint64_t len = (name.length() > 8) ? 8 : name.length();
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

uint32_t Coff::GetSectionBufferSize(uint16_t sectionIdx) {
	// sectionIdx is 1-based
	if (sectionIdx > 0 && sectionIdx <= sectionBuffers_.size()) {
		return static_cast<uint32_t>(sectionBuffers_[sectionIdx - 1].size());
	}
	return 0;
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

void Coff::UpgradeSymbol(uint32_t symbolIdx, uint32_t value, uint16_t sectionIdx) {
	if (symbolIdx < (uint32_t)symbols_.size()) {
		symbols_[symbolIdx].Value = value;
		symbols_[symbolIdx].SectionNumber = (int16_t)sectionIdx;
	}
}

void Coff::AppendPadding(uint16_t sectionIdx, uint32_t alignment) {
	uint32_t currentSize = static_cast<uint32_t>(sectionBuffers_[sectionIdx - 1].size());
	uint32_t paddedSize = (currentSize + alignment - 1) & ~(alignment - 1);
	uint32_t paddingNeeded = paddedSize - currentSize;

	if (paddingNeeded > 0) {
		// resize() appends 0x00 by default
		sectionBuffers_[sectionIdx - 1].resize(paddedSize, 0x00);
	}
}

void Coff::WriteTo(const std::string& path) {
    std::ofstream f(path, std::ios::binary);

    // 1. Prepare Header
    IMAGE_FILE_HEADER fileHeader = { 0 };
    fileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    fileHeader.NumberOfSections = (uint16_t)sections_.size();
    fileHeader.NumberOfSymbols = (uint32_t)symbols_.size();
    fileHeader.SizeOfOptionalHeader = 0; 
    fileHeader.Characteristics = 0;

    auto Align = [](uint32_t offset, uint32_t alignment) {
        return (offset + alignment - 1) & ~(alignment - 1);
    };

    // 2. PASS 1: Calculate all File Offsets
    uint32_t currentOffset = sizeof(IMAGE_FILE_HEADER) + (uint32_t)(sections_.size() * sizeof(IMAGE_SECTION_HEADER));

    // Raw Data Offsets (Aligned to 512 for PE compatibility)
    for (uint64_t i = 0; i < sections_.size(); ++i) {
        if (!sectionBuffers_[i].empty()) {
            currentOffset = Align(currentOffset, 512);
            sections_[i].PointerToRawData = currentOffset;
            sections_[i].SizeOfRawData = (uint32_t)sectionBuffers_[i].size();
            currentOffset += sections_[i].SizeOfRawData;
        }
    }

    // Relocation Offsets (Aligned to 4)
    for (uint64_t i = 0; i < sections_.size(); ++i) {
        if (!sectionRelocs_[i].empty()) {
            currentOffset = Align(currentOffset, 4);
            sections_[i].PointerToRelocations = currentOffset;
            sections_[i].NumberOfRelocations = (uint16_t)sectionRelocs_[i].size();
            currentOffset += (uint32_t)(sectionRelocs_[i].size() * sizeof(IMAGE_RELOCATION));
        }
    }

    // Symbol Table Offset
    currentOffset = Align(currentOffset, 4);
    fileHeader.PointerToSymbolTable = currentOffset;

    // 3. PASS 2: Physical Write Phase
    f.write((char*)&fileHeader, sizeof(fileHeader));
    f.write((char*)sections_.data(), sections_.size() * sizeof(IMAGE_SECTION_HEADER));

    // Write Raw Data with padding
    for (uint64_t i = 0; i < sections_.size(); ++i) {
        if (!sectionBuffers_[i].empty()) {
            long currentPos = (long)f.tellp();
            int padding = sections_[i].PointerToRawData - currentPos;
            if (padding > 0) {
                std::vector<char> pad(padding, 0);
                f.write(pad.data(), padding);
            }
            f.write((char*)sectionBuffers_[i].data(), sectionBuffers_[i].size());
        }
    }

    // Write Relocations
    for (uint64_t i = 0; i < sections_.size(); ++i) {
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
    long currentPos = (long)f.tellp();
    int symPadding = fileHeader.PointerToSymbolTable - currentPos;
    if (symPadding > 0) {
        std::vector<char> pad(symPadding, 0);
        f.write(pad.data(), symPadding);
    }
    f.write((char*)symbols_.data(), symbols_.size() * sizeof(RawSymbol));

    // Finalize and Write String Table
    // The first 4 bytes MUST be the total size of the table
    uint32_t totalStringTableSize = static_cast<uint32_t>(stringTable_.size());
    memcpy(stringTable_.data(), &totalStringTableSize, 4);
    f.write(stringTable_.data(), stringTable_.size());

    f.close();
}
