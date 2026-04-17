// process/Xmo.cpp
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
		if (!node) {
			WritePod(buf, (uint8_t)0);
			return;
		}
		WritePod(buf, (uint8_t)1);

		// Code Blocks (Uses raw array and explicit codeBlockCount)
		WritePod(buf, node->codeBlockCount);
		for (uint32_t i = 0; i < node->codeBlockCount; ++i) {
			WritePod(buf, node->codeBlocks[i]);
		}

		// Patch Symbols (Unpacks InternedString into a temporary std::string)
		WritePod(buf, node->patchSymbolCount);
		for (uint32_t i = 0; i < node->patchSymbolCount; ++i) {
			const auto& interned = node->patchSymbols[i];
			std::string symStr(interned.str, interned.len);
			WriteString(buf, symStr);
		}

		// Static Data (Uses raw array and explicit staticDataCount)
		WritePod(buf, node->staticDataCount);
		for (uint32_t i = 0; i < node->staticDataCount; ++i) {
			const auto& sd = node->staticData[i];
			WriteString(buf, sd.name);
			WritePod(buf, sd.exportIdx);

			// sd.bytes is still a std::vector, so .size() is still valid here
			WritePod(buf, (uint32_t)sd.bytes.size());
			buf.insert(buf.end(), sd.bytes.begin(), sd.bytes.end());
		}

		// Function Metadata
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

		// Children (Recursive)
		WritePod(buf, node->childCount);
		for (uint32_t i = 0; i < node->childCount; ++i) {
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
		WritePod(buf, header);

		// Write Exports — ALWAYS FIRST for discovery speed
		header.exportTableOffset = (uint32_t)buf.size();
		WritePod(buf, (uint32_t)xmo->exports.size());
		for (const auto& exp : xmo->exports) {
			WriteString(buf, exp.name);
			WritePod(buf, exp.offset);

			// --- UPDATED HERE: Dropped refinementMask and added min/max ---
			WritePod(buf, exp.minrmask);
			WritePod(buf, exp.maxrmask);

			// Write the full namespace path so DeserializeXmo can reconstruct it.
			WritePod(buf, (uint32_t)exp.namespacePath.size());
			for (uint32_t id : exp.namespacePath) WritePod(buf, id);
		}

		// Write Scopes
		WritePod(buf, (uint32_t)xmo->scopeTree.size());
		for (const auto& scope : xmo->scopeTree) {
			WritePod(buf, scope.id);
			WritePod(buf, scope.parentId);
			WriteString(buf, scope.name);
		}

		// Write Code Buffer
		header.codeBufferOffset = (uint32_t)buf.size();
		WritePod(buf, (uint32_t)xmo->codeBuffer.size());
		buf.insert(buf.end(), xmo->codeBuffer.begin(), xmo->codeBuffer.end());

		// Write Relocs
		WritePod(buf, (uint32_t)xmo->relocs.size());
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

		std::string intPathStr = std::get<std::string>(data::ProjectFile["IntPath"]);
		std::string srcRootStr = std::get<std::string>(data::ProjectFile["SourceRoot"]);

		fs::path xmoRoot = fs::absolute(intPathStr) / "xmo";
		fs::path srcRoot = fs::absolute(srcRootStr);

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
		osdebug << sformat("SaveXmos: %zu files processed, %zu saved in %.3f seconds.",
			data::Xmos.size(), savedCount, diff.count()) << endl;
	}

#pragma endregion

#pragma region Loading

	bool MapXmoFile(const fs::path& path, data::FileMapping& outMapping) {
		outMapping.fileHandle = CreateFileW(
			path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL
		);

		if (outMapping.fileHandle == INVALID_HANDLE_VALUE) return false;

		LARGE_INTEGER size;
		GetFileSizeEx(outMapping.fileHandle, &size);
		outMapping.size = (size_t)size.QuadPart;

		outMapping.mappingHandle = CreateFileMappingW(
			outMapping.fileHandle, NULL, PAGE_READONLY, 0, 0, NULL
		);
		if (!outMapping.mappingHandle) {
			CloseHandle(outMapping.fileHandle);
			return false;
		}

		outMapping.address = MapViewOfFile(outMapping.mappingHandle, FILE_MAP_READ, 0, 0, 0);
		return outMapping.address != nullptr;
	}

	data::ParseTreeNode* ReadParseTreeFromPtr(const uint8_t* base, uint64_t& offset, data::Arena& arena) {
		if (ReadPodFromPtr<uint8_t>(base, offset) == 0) return nullptr;

		auto* node = (data::ParseTreeNode*)arena.Allocate(sizeof(data::ParseTreeNode));
		new (node) data::ParseTreeNode();

		// Code Blocks (Uses raw array and explicit codeBlockCount)
		node->codeBlockCount = ReadPodFromPtr<uint32_t>(base, offset);
		if (node->codeBlockCount > 0) {
			node->codeBlocks = (uint16_t*)arena.Allocate(sizeof(uint16_t) * node->codeBlockCount);
			for (uint32_t i = 0; i < node->codeBlockCount; ++i) {
				node->codeBlocks[i] = ReadPodFromPtr<uint16_t>(base, offset);
			}
		}

		// Patch Symbols (Decouples raw strings into your sharded SymbolTable)
		node->patchSymbolCount = ReadPodFromPtr<uint32_t>(base, offset);
		if (node->patchSymbolCount > 0) {
			node->patchSymbols = (data::InternedString*)arena.Allocate(sizeof(data::InternedString) * node->patchSymbolCount);
			for (uint32_t i = 0; i < node->patchSymbolCount; ++i) {
				std::string tempStr = ReadStringFromPtr(base, offset);
				// This caches the hash and binds the backing char* to your global SymbolArena
				node->patchSymbols[i] = process::InternString(tempStr.c_str());
			}
		}

		// Static Data (Uses raw array and explicit staticDataCount)
		node->staticDataCount = ReadPodFromPtr<uint32_t>(base, offset);
		if (node->staticDataCount > 0) {
			node->staticData = (data::StaticData*)arena.Allocate(sizeof(data::StaticData) * node->staticDataCount);
			for (uint32_t i = 0; i < node->staticDataCount; ++i) {
				data::StaticData& sd = node->staticData[i];
				new (&sd) data::StaticData(); // placement new for std::string and vector internals

				sd.name = ReadStringFromPtr(base, offset);
				sd.exportIdx = ReadPodFromPtr<uint64_t>(base, offset);

				uint32_t byteCount = ReadPodFromPtr<uint32_t>(base, offset);
				sd.bytes.assign(base + offset, base + offset + byteCount);
				offset += byteCount;
			}
		}

		// Function Metadata
		if (ReadPodFromPtr<uint8_t>(base, offset) == 1) {
			node->funcData = (data::FunctionNodeData*)arena.Allocate(sizeof(data::FunctionNodeData));
			new (node->funcData) data::FunctionNodeData();
			node->funcData->name = ReadStringFromPtr(base, offset);
			node->funcData->totalStackSize = ReadPodFromPtr<uint32_t>(base, offset);
			node->funcData->exportIdx = ReadPodFromPtr<uint64_t>(base, offset);
			node->funcData->isLeaf = ReadPodFromPtr<bool>(base, offset);
		}

		// Children
		node->childCount = ReadPodFromPtr<uint32_t>(base, offset);
		if (node->childCount > 0) {
			node->children = (data::ParseTreeNode**)arena.Allocate(sizeof(data::ParseTreeNode*) * node->childCount);
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

		uint64_t offset = header->exportTableOffset;

		// Exports
		uint32_t expCount = ReadPodFromPtr<uint32_t>(base, offset);
		for (uint32_t i = 0; i < expCount; ++i) {
			std::string name = ReadStringFromPtr(base, offset);
			uint32_t expOffset = ReadPodFromPtr<uint32_t>(base, offset);
			uint32_t minrmask = ReadPodFromPtr<uint32_t>(base, offset);
			uint32_t maxrmask = ReadPodFromPtr<uint32_t>(base, offset);

			// Read the full namespace path written by WriteXmoFile
			uint32_t pathLen = ReadPodFromPtr<uint32_t>(base, offset);
			std::vector<uint32_t> namespacePath;
			namespacePath.reserve(pathLen);
			for (uint32_t j = 0; j < pathLen; ++j)
				namespacePath.push_back(ReadPodFromPtr<uint32_t>(base, offset));

			Symbol* symbol = InternSymbol(name.c_str(), std::move(namespacePath));
			symbol->originXmo = xmo;
			symbol->offset    = expOffset;
			symbol->minrmask  = minrmask;
			symbol->maxrmask = maxrmask;
		}

		// Scopes
		uint32_t scopeCount = ReadPodFromPtr<uint32_t>(base, offset);
		for (uint32_t i = 0; i < scopeCount; ++i) {
			data::XmoScope scope;
			scope.id       = ReadPodFromPtr<uint32_t>(base, offset);
			scope.parentId = ReadPodFromPtr<uint32_t>(base, offset);
			scope.name     = ReadStringFromPtr(base, offset);
			xmo->scopeTree.push_back(scope);
		}

		if (loadParseTree) {
			offset = header->parseTreeOffset;
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
