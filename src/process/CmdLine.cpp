#include "pch.h"

#include "tool/Logger.h"
#include "CmdLine.h"
#include "data/CmdLineData.h"

#include "tool/TextParser.h"

namespace process
{
	void ParseCommandLine(int argc, char* argv[]) {
		if (argc < 1) {
			oserror 
				<< "Usage: \n"
				<< "  xmc [projectname] [config] full\n" 
				<< "  xmc test" << endl;
			;
			throw std::runtime_error("Project name required.");
		}

		if (_stricmp(argv[0], "test") == 0)
		{
			data::CmdLineArgs.Test = true;
		}

		data::CmdLineArgs.ProjectName = argv[1];

		// Simple "sliding" logic for optional args
		for (int i = 2; i < argc; ++i) {
			if (_stricmp(argv[i], "full") == 0) {
				data::CmdLineArgs.Full = true;
			}
			else if (data::CmdLineArgs.ConfigName.empty()) {
				// If it's not "full" and we don't have a config yet, it's the config
				data::CmdLineArgs.ConfigName = argv[i];
			}
		}

		TextParser p("");
	}

}