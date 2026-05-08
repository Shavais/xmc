#include "pch/pch.h"

#include "CommandLineReader.h"
#include "tool/Logger.h"

#include <string_view>

namespace xmc
{
	static bool FlagMatch(const char* arg, const char* bare, const char* dashed)
	{
		return _stricmp(arg, bare) == 0 || _stricmp(arg, dashed) == 0;
	}

	static void PrintUsage()
	{
		oserror
			<< "Usage:\n"
			<< "  xmc <project> [config] [file_filter] [--full]\n"
			<< "  xmc <project> [config] [file_filter] --test [test_filter] [--full]\n"
			<< "  xmc <project> [config] [file_filter] --suite [--full]\n"
			<< "\n"
			<< "  file_filter   wildcard matched against source .xm filenames\n"
			<< "  test_filter   wildcard matched against test block names (namespace paths use ':')\n";
	}

	CmdLineArgs CommandLineReader::Parse(int argc, char* argv[])
	{
		if (argc < 2)
		{
			PrintUsage();
			throw std::runtime_error("No arguments provided.");
		}

		CmdLineArgs args;

		// First pass: resolve flags so we know how to interpret positionals.
		for (int i = 1; i < argc; ++i)
		{
			if (FlagMatch(argv[i], "full", "--full")) { args.Full = true; continue; }
			if (FlagMatch(argv[i], "suite", "--suite")) { args.Suite = true; continue; }
			if (FlagMatch(argv[i], "test", "--test"))
			{
				args.Test = true;
				// The token immediately following --test, if not another flag, is the test filter
				if (i + 1 < argc && argv[i + 1][0] != '-')
					args.TestFilter = argv[++i];
				continue;
			}
		}

		if (args.Test && args.Suite)
		{
			PrintUsage();
			throw std::runtime_error("--test and --suite are mutually exclusive.");
		}

		// Second pass: assign positional arguments, skipping flags and the
		// test filter (already consumed above).
		bool testFilterConsumed = args.Test && !args.TestFilter.empty();
		for (int i = 1; i < argc; ++i)
		{
			if (FlagMatch(argv[i], "full", "--full")) continue;
			if (FlagMatch(argv[i], "suite", "--suite")) continue;
			if (FlagMatch(argv[i], "test", "--test"))
			{
				if (testFilterConsumed) { ++i; }  // skip the already-consumed test filter
				continue;
			}

			if (args.ProjectName.empty()) args.ProjectName = argv[i];
			else if (args.ConfigName.empty())  args.ConfigName = argv[i];
			else if (args.FileFilter.empty())  args.FileFilter = argv[i];
		}

		if (args.ProjectName.empty())
		{
			PrintUsage();
			throw std::runtime_error("Project name required.");
		}

		return args;
	}
}