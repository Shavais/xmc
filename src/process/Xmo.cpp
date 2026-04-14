#include "pch.h"

#include <filesystem>
#include <fstream>
#include <vector>

#include "win32.h"

#include "data/CmdLineData.h"
#include "data/GlobalData.h"
#include "data/ParserData.h"
#include "data/ProjectFileData.h"
#include "data/SourceFileData.h"
#include "data/XmoData.h"

#include "process/Parser.h"
#include "process/Util.h"

#include "tool/BS_thread_pool.hpp"
#include "tool/Logger.h"
#include "tool/StringFunctions.h"

namespace fs = std::filesystem;

using namespace data;

namespace process {
#pragma region Saving

	void WriteParseTree(std::vector<uint8_t>& buf, const data::ParseTreeNode* node) {
		// Write null marker (0 for null, 1 for valid)
		if (!node) {
			WritePod(buf, (uint8_t)0);
			return;
		}
		WritePod(buf, (uint8_t)1);

		// 1. Code Blocks
		WritePod(buf, (uint32_t)node->codeBlocks.size());
		for (auto cb : node->codeBlocks) WritePod(buf, cb);

		// 2. Patch Symbols
		WritePod(buf, (uint32_t)node->patchSymbols.size());
		for (const auto& sym : node->patchSymbols) WriteString(buf, sym);

		// 3. Static Data
		WritePod(buf, (uint32_t)node->staticData.size());
		for (const auto& sd : node->staticData) {
			WriteString(buf, sd.name);
			WritePod(buf, sd.exportIdx);
			WritePod(buf, (uint32_t)sd.bytes.size());
			buf.insert(buf.end(), sd.bytes.begin(), sd.bytes.end());
		}

		// 4. Function Metadata
		if (node->funcData) {
			WritePod(buf, (uint8_t)1);
			WriteString(buf, node->funcData->name);
			WritePod(buf, node->funcData->totalStackSize);
			WritePod(buf, node->funcData->exportIdx);
			WritePod(buf, node->funcData->isLeaf);
		}
		else {
			WritePod(buf, (uint8_t)0);
		}

		// 5. Children (Recursive Step)
		WritePod(buf, node->childCount);
		for (uint32_t i = 0; i < node->childCount; ++i) {
			// Correctly pass the specific child to the next recursive call
			WriteParseTree(buf, node->children[i]);
		}
	}

	void WriteXmoFile(const data::Xmo* xmo, const fs::path& outPath) {
		fs::create_directories(outPath.parent_path());
		std::vector<uint8_t> buf;
		buf.reserve(8192);

		// Write Header Placeholder
		data::XmoHeader header;
		size_t headerPos = buf.size();
		WritePod(buf, header); // Magic, Version, and 0-filled offsets

		// Write Exports (Symbols) - ALWAYS FIRST for discovery speed
		header.exportTableOffset = (uint32_t)buf.size();
		uint32_t expCount = static_cast<uint32_t>(xmo->exports.size());
		WritePod(buf, expCount);
		for (const auto& exp : xmo->exports) {
			WriteString(buf, exp.name);
			WritePod(buf, exp.offset);
			WritePod(buf, exp.refinementMask);
			WritePod(buf, exp.scopeId);
		}

		// Write Scopes
		uint32_t scopeCount = static_cast<uint32_t>(xmo->scopeTree.size());
		WritePod(buf, scopeCount);
		for (const auto& scope : xmo->scopeTree) {
			WritePod(buf, scope.id);
			WritePod(buf, scope.parentId);
			WriteString(buf, scope.name);
		}

		// Write Code Buffer
		header.codeBufferOffset = (uint32_t)buf.size();
		uint32_t codeSize = static_cast<uint32_t>(xmo->codeBuffer.size());
		WritePod(buf, codeSize);
		buf.insert(buf.end(), xmo->codeBuffer.begin(), xmo->codeBuffer.end());

		// Write Relocs
		uint32_t relCount = static_cast<uint32_t>(xmo->relocs.size());
		WritePod(buf, relCount);
		for (const auto& rel : xmo->relocs) {
			WriteString(buf, rel.targetSymbol);
			WritePod(buf, rel.offset);
			WritePod(buf, rel.type);
		}

		// Write Parse Tree
		header.parseTreeOffset = (uint32_t)buf.size();
		WriteParseTree(buf, xmo->parseTree);

		// Patch the Header with real offsets
		std::memcpy(&buf[headerPos], &header, sizeof(header));

		FastWriteBinaryFile(outPath, buf);
	}

	void SaveXmos() {
		auto start = std::chrono::high_resolution_clock::now();

		// Retrieve paths from your global data config
		std::string intPathStr = std::get<std::string>(data::ProjectFile["IntPath"]);
		std::string srcRootStr = std::get<std::string>(data::ProjectFile["SourceRoot"]);

		fs::path xmoRoot = fs::absolute(intPathStr) / "xmo";
		fs::path srcRoot = fs::absolute(srcRootStr);

		// Full builds wipe the cache
		if (data::CmdLineArgs.Full && fs::exists(xmoRoot)) {
			fs::remove_all(xmoRoot);
		}

		BS::thread_pool<> pool;
		size_t savedCount = 0;

		for (data::Xmo* xmo : data::Xmos) {
			if (!xmo->dirty_) continue;

			savedCount++;
			pool.detach_task(
				[xmo, xmoRoot, srcRoot]() {
					auto it = data::SourceFiles.find(xmo->name);
					if (it == data::SourceFiles.end()) return;

					// Maintain directory structure: src/foo/bar.xm -> int/xmo/foo/bar.xmo
					fs::path relPath = fs::relative(it->second.fullPath, srcRoot);
					fs::path outPath = xmoRoot / relPath;
					outPath.replace_extension(".xmo");

					WriteXmoFile(xmo, outPath);
				}
			);
		}

		pool.wait();

		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> diff = end - start;
		osdebug << sformat("SaveXmos: %zu files processed, %zu saved in %.3f seconds.", data::Xmos.size(), savedCount, diff.count()) << endl;
	}

#pragma endregion

#pragma region Loading

	bool MapXmoFile(const fs::path& path, data::FileMapping& outMapping) {
		outMapping.fileHandle = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);

		if (outMapping.fileHandle == INVALID_HANDLE_VALUE) return false;

		LARGE_INTEGER size;
		GetFileSizeEx(outMapping.fileHandle, &size);
		outMapping.size = (size_t)size.QuadPart;

		outMapping.mappingHandle = CreateFileMappingW(outMapping.fileHandle, NULL, PAGE_READONLY, 0, 0, NULL);
		if (!outMapping.mappingHandle) {
			CloseHandle(outMapping.fileHandle);
			return false;
		}

		outMapping.address = MapViewOfFile(outMapping.mappingHandle, FILE_MAP_READ, 0, 0, 0);
		return outMapping.address != nullptr;
	}

	data::ParseTreeNode* ReadParseTreeFromPtr(const uint8_t* base, uint64_t& offset, data::Arena& arena) {
		// Check for Null Marker
		// We wrote 0 for null and 1 for valid nodes
		if (ReadPodFromPtr<uint8_t>(base, offset) == 0) {
			return nullptr;
		}

		// Allocate and Initialize the Node
		// We use the Arena to avoid individual 'new' heap calls
		auto* node = (data::ParseTreeNode*)arena.Allocate(sizeof(data::ParseTreeNode));
		new (node) data::ParseTreeNode(); // Placement new: initializes the vectors/members

		// Load Code Blocks
		uint32_t cbCount = ReadPodFromPtr<uint32_t>(base, offset);
		node->codeBlocks.reserve(cbCount);
		for (uint32_t i = 0; i < cbCount; ++i) {
			node->codeBlocks.push_back(ReadPodFromPtr<uint16_t>(base, offset));
		}

		// Load Patch Symbols
		uint32_t psCount = ReadPodFromPtr<uint32_t>(base, offset);
		node->patchSymbols.reserve(psCount);
		for (uint32_t i = 0; i < psCount; ++i) {
			node->patchSymbols.push_back(ReadStringFromPtr(base, offset));
		}

		// Load Static Data
		uint32_t sdCount = ReadPodFromPtr<uint32_t>(base, offset);
		node->staticData.reserve(sdCount);
		for (uint32_t i = 0; i < sdCount; ++i) {
			data::StaticData sd;
			sd.name = ReadStringFromPtr(base, offset);
			sd.exportIdx = ReadPodFromPtr<uint64_t>(base, offset);
			uint32_t byteCount = ReadPodFromPtr<uint32_t>(base, offset);

			// Efficiency note: We still copy bytes here. 
			// For a kernel-scale build, we eventually want to point directly to 'base + offset'
			sd.bytes.assign(base + offset, base + offset + byteCount);
			offset += byteCount;

			node->staticData.push_back(std::move(sd));
		}

		// Load Function Metadata
		if (ReadPodFromPtr<uint8_t>(base, offset) == 1) {
			node->funcData = (data::FunctionNodeData*)arena.Allocate(sizeof(data::FunctionNodeData));
			new (node->funcData) data::FunctionNodeData();
			node->funcData->name = ReadStringFromPtr(base, offset);
			node->funcData->totalStackSize = ReadPodFromPtr<uint32_t>(base, offset);
			node->funcData->exportIdx = ReadPodFromPtr<uint64_t>(base, offset);
			node->funcData->isLeaf = ReadPodFromPtr<bool>(base, offset);
		}

		// Recursive Child Loading
		node->childCount = ReadPodFromPtr<uint32_t>(base, offset);
		if (node->childCount > 0) {
			// Allocate the array of pointers in the arena to keep it all contiguous
			node->children = (ParseTreeNode**)arena.Allocate(sizeof(ParseTreeNode*) * node->childCount);
			for (uint32_t i = 0; i < node->childCount; ++i) {
				node->children[i] = ReadParseTreeFromPtr(base, offset, arena);
			}
		}

		return node;
	}

	std::mutex xmoListMutex;

	void DeserializeXmo(data::Xmo* xmo, const uint8_t* base, bool loadParseTree) {
		auto* header = reinterpret_cast<const data::XmoHeader*>(base);
		if (header->magic != 0x584D4F21) return;

		// Discovery: Only jump to the Export/Symbol table
		uint64_t offset = header->exportTableOffset;

		// Exports
		uint32_t expCount = ReadPodFromPtr<uint32_t>(base, offset);
		for (uint32_t i = 0; i < expCount; ++i) {
			std::string tempName = ReadStringFromPtr(base, offset);
			uint32_t scopeId = ReadPodFromPtr<uint32_t>(base, offset);
			Symbol* symbol = GetOrCreateSymbol(tempName.c_str(), scopeId);
			symbol->originXmo = xmo;
			symbol->offset = ReadPodFromPtr<uint32_t>(base, offset);
			symbol->rmask = ReadPodFromPtr<uint32_t>(base, offset);
		}
		// Scopes (Assuming they follow exports in file)
		uint32_t scopeCount = ReadPodFromPtr<uint32_t>(base, offset);
		for (uint32_t i = 0; i < scopeCount; ++i) {
			data::XmoScope scope;
			scope.id = ReadPodFromPtr<uint32_t>(base, offset);
			scope.parentId = ReadPodFromPtr<uint32_t>(base, offset);
			scope.name = ReadStringFromPtr(base, offset);
			xmo->scopeTree.push_back(scope);
		}

		// Full Scan: Load the massive parse tree only if requested
		if (loadParseTree) {
			offset = header->parseTreeOffset;
			// Pass the Xmo's specific Arena to keep this file's nodes contiguous
			xmo->parseTree = ReadParseTreeFromPtr(base, offset, xmo->arena);
		}
	}

	void LoadXmos(bool loadParseTrees) {
		auto start = std::chrono::high_resolution_clock::now();

		std::string intPathStr = std::get<std::string>(data::ProjectFile["IntPath"]);
		std::string srcRootStr = std::get<std::string>(data::ProjectFile["SourceRoot"]);
		fs::path xmoRoot = fs::absolute(intPathStr) / "xmo";
		fs::path srcRoot = fs::absolute(srcRootStr);

		BS::thread_pool<> pool;
		std::unordered_set<std::string> modified(data::ModifiedSources.begin(), data::ModifiedSources.end());

		for (auto const& [name, srcInfo] : data::SourceFiles) {
			// Only load if it WASN'T modified in this compile session
			if (modified.find(name) == modified.end()) {
				pool.detach_task([&, name, srcInfo, loadParseTrees, xmoRoot, srcRoot]() {
					fs::path relPath = fs::relative(srcInfo.fullPath, srcRoot);
					fs::path xmoPath = xmoRoot / relPath;
					xmoPath.replace_extension(".xmo");

					if (fs::exists(xmoPath)) {
						data::Xmo* xmo = new data::Xmo();
						if (MapXmoFile(xmoPath, xmo->mapping)) {
							DeserializeXmo(xmo, static_cast<const uint8_t*>(xmo->mapping.address), loadParseTrees);

							std::lock_guard<std::mutex> lock(xmoListMutex);
							data::Xmos.push_back(xmo);
						}
						else {
							delete xmo;
						}
					}
					});
			}
		}
		pool.wait();

		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> diff = end - start;

		osdebug << sformat("LoadXmos: %zu unmodified XMOs loaded in %.3f seconds.",
			data::Xmos.size(), diff.count()) << endl;
	}

#pragma endregion

} // namespace process
