// data/XmoData.h
#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

#include "data/Arena.h"
#include "CodeBlock.h"

using std::string;
using std::unique_ptr;
using std::vector;

namespace data
{

	struct FileMapping {
		void* address = nullptr;
		HANDLE fileHandle = INVALID_HANDLE_VALUE;
		HANDLE mappingHandle = INVALID_HANDLE_VALUE;
		size_t size = 0;

		void Unmap() {
			if (address) UnmapViewOfFile(address);
			if (mappingHandle != INVALID_HANDLE_VALUE) CloseHandle(mappingHandle);
			if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
			address = nullptr;
		}
	};

	struct XmoHeader {
		uint32_t magic = 0x584D4F21; // "XMO!"
		uint32_t version = 1;
		uint32_t exportTableOffset;
		uint32_t codeBufferOffset;
		uint32_t parseTreeOffset;
	};

	struct XmoScope {
		uint32_t id;
		uint32_t parentId;
		string   name;
	};

	struct XmoImport {
		string   targetSymbol;
		uint32_t expectedMask;
	};

	struct XmoExport {
		string                name;
		uint32_t              offset;
		uint32_t              minrmask;
		uint32_t              maxrmask;
		// Full namespace path, outermost (file) scope first.
		// Replaces the old flat scopeId.
		std::vector<uint32_t> namespacePath;
	};

	struct XmoRelocation {
		string   targetSymbol;
		uint32_t offset;
		uint16_t type;
	};

	struct ParseTreeNode;

	struct Xmo {
		string              name;
		vector<uint8_t>     codeBuffer;
		vector<XmoExport>   exports;
		vector<XmoRelocation> relocs;
		vector<XmoScope>    scopeTree;
		ParseTreeNode*      parseTree;
		uint32_t            tempGlobalOffset = 0;
		bool                dirty_ = false;
		FileMapping         mapping;
		Arena               arena;
	};

	inline vector<Xmo*> Xmos;

} // namespace data
