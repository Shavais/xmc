#include "pch/pch.h"
#include "SourceLoader.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "tool/BS_thread_pool.hpp" 
#include "tool/Logger.h"

#include "pch/win32.h"

namespace fs = std::filesystem;

namespace xmc {

	std::mutex mapMutex;

	SourceLoader::SourceLoader(ConfigSection& projectFileReader) : config_(projectFileReader)
	{
	}

	std::string SourceLoader::FastLoadFile(const std::filesystem::path& path) {

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
	void SourceLoader::WalkFsTree(const fs::path& root, const std::string& extension, std::unordered_map<std::string, FileInfo>& registry, BS::thread_pool<>& pool) {
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

	void SourceLoader::LoadSourceFiles(CmdLineArgs& args) {
		auto start = std::chrono::high_resolution_clock::now();

		// Setup Paths and Pool

		
		BS::thread_pool<> pool;

		// Scan Filesystem (pool thread per file)
		WalkFsTree(srcRoot, ".xm", SourceFiles, pool);
		if (!args.Full) WalkFsTree(intRoot, ".xmo", ObjectFiles, pool);
		pool.wait();

		// Identify and Load Modified Sources (pool thread per folder)
		for (auto& [name, srcInfo] : SourceFiles) {
			bool needsRebuild = false;

			if (args.Full) {
				needsRebuild = true;
			}
			else {
				auto it = ObjectFiles.find(name);
				if (it == ObjectFiles.end() || srcInfo.lastWriteTime > it->second.lastWriteTime) {
					needsRebuild = true;
				}

				if (needsRebuild) {
					ModifiedSources.push_back(name);

					// Launch parallel load task
					pool.detach_task([this, &info = srcInfo]() {
						std::string text = FastLoadFile(info.fullPath);
						if (!text.empty()) {
							std::lock_guard<std::mutex> lock(mapMutex);
							info.sourceText = std::move(text);
						}
						});
				}
			}
			pool.wait();

		}

		auto end = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> diff = end - start;
		osdebug << "LoadModifiedSources: " << ModifiedSources.size() << " files loaded in " << std::fixed << std::setprecision(3) << diff.count() << " seconds." << endl;
	}

}