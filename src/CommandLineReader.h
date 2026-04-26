#pragma once
#include <string>

namespace xmc
{
	class CommandLineReader
	{
	public:
		struct Args
		{
			std::string ProjectName;
			std::string ConfigName;
			std::string FileFilter;   // optional; if present, build per-file coffs/exes
			std::string TestFilter;   // optional; wildcard filter on test block names
			bool Full = false;
			bool Test = false;
			bool Suite = false;
		};

		// xmc <project> [config] [file_filter] [--test [test_filter] | --suite] [--full]
		// Throws std::runtime_error if project name is missing or --test and --suite both appear.
		static Args Parse(int argc, char* argv[]);
	};
}