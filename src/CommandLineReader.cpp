// process/CommandLineReader.cpp
#include "pch/pch.h"

#include "tool/Logger.h"
#include "tool/TextParser.h"
#include "CommandLineReader.h"

namespace xmc
{
	CommandLineReader::Args CommandLineReader::Parse(int argc, char* argv[])
	{
		if (argc < 2)
		{
			oserror
				<< "Usage: \n"
				<< "  xmc [projectname] [config] full\n"
				<< "  xmc [projectname] [config] test" << endl;

			throw std::runtime_error("Project name required.");
		}

		Args args;
		args.ProjectName = argv[1];

		// Simple "sliding" logic for optional args
		for (int i = 2; i < argc; ++i)
		{
			if (_stricmp(argv[i], "full") == 0)
			{
				args.Full = true;
			}
			else if (_stricmp(argv[i], "test") == 0)
			{
				args.Test = true;
			}
			else if (args.ConfigName.empty())
			{
				// If it's not "full" or "test" and we don't have a config yet, it's the config
				args.ConfigName = argv[i];
			}
		}

		TextParser p("");
		return args;
	}
}