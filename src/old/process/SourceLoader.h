#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ProjectFileReader.h"
#include "../CommandLineReader.h"

namespace xmc
{
	struct FileInfo {
		std::filesystem::path fullPath;
		std::filesystem::file_time_type lastWriteTime;
		std::string sourceText;
	};

	class SourceLoader
	{
	public:
		SourceLoader(ConfigSection& projectFileReader);

		std::string FastLoadFile(const std::filesystem::path& path);
		void WalkFsTree(const fs::path& root, const std::string& extension, std::unordered_map<std::string, FileInfo>& registry, BS::thread_pool<>& pool);

		void LoadSourceFiles(CmdLineArgs& args);

	public:
		std::unordered_map<std::string, FileInfo> SourceFiles;
		std::unordered_map<std::string, FileInfo> ObjectFiles;
		std::vector<std::string> ModifiedSources;

	private:
		ConfigSection& config_;
	};

}