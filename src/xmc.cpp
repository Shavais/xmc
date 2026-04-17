#include "pch.h"

#include <iostream>

#include "data/CmdLineData.h"
#include "data/GlobalData.h"
#include "data/LinkerData.h"

#include "experiments.h"

#include "process/CmdLine.h"
#include "process/Linker.h"
#include "process/ProjectFile.h"
#include "process/Util.h"

#include "tool/Logger.h"
#include "tool/StringFunctions.h"


using namespace process;
using namespace data;

namespace process {
	void LoadSourceFiles();
	void ParseModifiedSources();
	void LoadXmos(bool loadParseTrees);
	void UpdateXmoCode();
	void SaveXmos();
	void WriteToCoff();
}

void RunTestPipeline();

int main(int argc, char* argv[])
{
	try
	{
		InitializeLogging();
		ParseCommandLine(argc, argv);
		ParseProjectFile();
		RemoveFile(data::CmdLineArgs.ProjectName + ".obj");
		RemoveFile(data::CmdLineArgs.ProjectName + ".exe");

		if (CmdLineArgs.Test)
		{
			RunTestPipeline();
			//CallCppFunction3();
			//GetPathToLinker();
			//GetLinkerArgs();				// populates data::LinkerArgs
			//RunShellCmd(PathToLinker + LinkerArgs);
			//osdebug << ShellCmdLog << endl;
			if (data::ErrorOccurred) return -1;
			return 0;
		}

		// We need to refactor the rest of this a bit:
		// Parsing the modified files requires the symbols from the unmodified files to already be loaded.

		auto compileStart = std::chrono::high_resolution_clock::now();
		
		LoadSourceFiles();			// Finds and loads modified .xm files, or loads them all if this is a full build
		
		// ParseSourceFiles();		// Rebuilds xmo's for modifed files, creates their parse trees including code blocks, does not update xmo code buffers
		if (data::FullScanRequired || data::CmdLineArgs.Full)
		{
			LoadXmos(true);			// Fully loads the xmo's associated with unmodified xm's, including parse trees 
			//FullScan();			// Scans parse trees of all xmo's, updates the refinement masks of symbols as needed, determines marks changed xmo's as dirty
		}
		else
		{
			LoadXmos(false);		// Partially loads .xmo's associated with unmodified files; parse trees are not loaded
		}
		UpdateXmoCode();			// Generates machine code
		SaveXmos();					// Persists dirty .xmo
		WriteToCoff();

		auto compileEnd = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> compileDist = compileEnd - compileStart;

		auto linkStart = std::chrono::high_resolution_clock::now();

		GetPathToLinker();
		GetLinkerArgs();				// populates data::LinkerArgs
		
		RunShellCmd(PathToLinker + LinkerArgs);
		osdebug << ShellCmdLog << endl;

		auto linkEnd = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double> linkDist = linkEnd - linkStart;

		if (!data::ErrorOccurred)
		{
			osdebug << ShellCmdLog << endl;
			osdebug << sformat("Compile time: %.3f s", compileDist.count()) << endl;
			osdebug << sformat("Link time:    %.3f s", linkDist.count()) << endl;
			osdebug << sformat("Total time:   %.3f s", (compileDist + linkDist).count()) << endl;
		}
	}
	catch (const std::runtime_error& e)
	{
		std::cerr << "error: " << e.what() << std::endl;	
	}
	catch (const std::exception& e)
	{
		std::cerr << "unhandled exception: " << e.what() << std::endl;
	}
	catch (...)
	{
		std::cerr << "an unknown exception was thrown" << std::endl;
	}
	
	if (ErrorOccurred) return -1;
	return 0;
}
