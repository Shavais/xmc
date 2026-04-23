// data/SourceFileData.h
#pragma once

#include <filesystem>
#include <unordered_map>

namespace data {
	struct FileInfo {
		std::filesystem::path fullPath;
		std::filesystem::file_time_type lastWriteTime;
		std::string sourceText;
	};
	inline std::unordered_map<std::string, FileInfo> SourceFiles;
	inline std::unordered_map<std::string, FileInfo> ObjectFiles;
	inline std::vector<std::string> ModifiedSources;
}
