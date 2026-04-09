#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

#include "CodeBlock.h"
#include "ParserData.h"

namespace data
{

	struct XmoScope {
		uint32_t id;
		uint32_t parentId;   // 0 for Global
		std::string name;    // Optional: for debugging or namespaces
	};

	struct XmoImport { // Renamed from Relocation for clarity
		std::string targetSymbol;
		uint32_t expectedMask;   // <--- What we thought it was when we compiled
	};

	struct XmoExport {
		std::string name;
		uint32_t offset;
		uint32_t refinementMask;
		uint32_t scopeId;
	};

	struct XmoRelocation {
		std::string targetSymbol; // Name of what we are calling
		uint32_t offset;          // Offset in this Xmo's code to patch
		uint16_t type;            // e.g., IMAGE_REL_AMD64_REL32
	};


	class Xmo {
	public:
		std::string name;
		std::vector<uint8_t> codeBuffer;
		std::vector<XmoExport> exports;
		std::vector<XmoRelocation> relocs;
		std::vector<XmoScope> scopeTree;
		ParseTreeNode* parseTree;
		uint32_t tempGlobalOffset;
	};

	inline vector<Xmo*> Xmos;

}