#include "pch.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "data/CmdLineData.h"
#include "data/GlobalData.h"
#include "data/ProjectFileData.h"
#include "data/SourceFileData.h"

#include "tool/BS_thread_pool.hpp" 
#include "tool/Logger.h"

#include "win32.h"

namespace fs = std::filesystem;

namespace process {

	std::mutex mapMutex;

	std::string FastLoadFile(const std::filesystem::path& path) {

		HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) return "";

		LARGE_INTEGER size;
		if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) { CloseHandle(hFile); return ""; }

		HANDLE hMapping = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL );
		if (!hMapping) { CloseHandle(hFile); return ""; }

		void* pData = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);

		std::string content;
		if (pData) {
			content.assign(static_cast<const char*>(pData), static_cast<uint64_t>(size.QuadPart));
			UnmapViewOfFile(pData);
		}

		CloseHandle(hMapping);
		CloseHandle(hFile);

		return content;
	}

	// Helper to walk the tree and identify files
	void WalkFsTree(const fs::path& root, const std::string& extension,
		std::unordered_map<std::string, data::FileInfo>& registry,
		BS::thread_pool<>& pool) {
		if (!fs::exists(root)) return;

		for (const auto& entry : fs::directory_iterator(root)) {
			if (entry.is_directory()) {
				pool.detach_task([&, path = entry.path()]() {
					WalkFsTree(path, extension, registry, pool);
					});
			}
			else if (entry.is_regular_file() && entry.path().extension() == extension) {
				std::string relPath = fs::relative(entry.path(), root).stem().string();
				std::lock_guard<std::mutex> lock(mapMutex);
				registry[relPath] = { entry.path(), entry.last_write_time(), "" };
			}
		}
	}

	void LoadSourceFiles() {
		auto start = std::chrono::high_resolution_clock::now();

		// Setup Paths and Pool
		std::string srcRootStr = std::get<std::string>(data::ProjectFile["SourceRoot"]);
		std::string intPathStr = std::get<std::string>(data::ProjectFile["IntPath"]);
		fs::path srcRoot = fs::absolute(srcRootStr);
		fs::path intPath = fs::absolute(intPathStr);
		BS::thread_pool<> pool;

		// Scan Filesystem (pool thread per file)
		WalkFsTree(srcRoot, ".xm", data::SourceFiles, pool);
		if (!data::CmdLineArgs.Full) WalkFsTree(intPath, ".xmo", data::ObjectFiles, pool);
		pool.wait();

		// Identify and Load Modified Sources (pool thread per folder)
		for (auto& [name, srcInfo] : data::SourceFiles) {
			bool needsRebuild = false;

			if (data::CmdLineArgs.Full) {
				needsRebuild = true;
			}
			else {
				auto it = data::ObjectFiles.find(name);
				if (it == data::ObjectFiles.end() || srcInfo.lastWriteTime > it->second.lastWriteTime) {
					needsRebuild = true;
				}

				if (needsRebuild) {
					data::ModifiedSources.push_back(name);

					// Launch parallel load task
					pool.detach_task([&info = srcInfo]() {
						std::string text = FastLoadFile(info.fullPath);
						if (!text.empty()) {
							std::lock_guard<std::mutex> lock(mapMutex);
							info.sourceText = std::move(text);
						}
						});
				}
			}
			pool.wait();

			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> diff = end - start;
			osdebug << "LoadModifiedSources: " << data::ModifiedSources.size() << " files loaded in " << std::fixed << std::setprecision(3) << diff.count() << " seconds." << endl;
		}
	}

}