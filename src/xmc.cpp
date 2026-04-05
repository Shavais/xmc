#include "pch.h"

#include <iostream>

//#include "experiments.h"

#include "data/CmdLineData.h"
#include "process/CmdLine.h"
#include "process/ProjectFile.h"
#include "tool/Logger.h"

#include "process/Linker.h"
#include "data/LinkerData.h"

using namespace process;

int main(int argc, char* argv[])
{
	try
	{
		InitializeLogging();
		// CallCppFunction();
		ParseCommandLine(argc, argv);
		ParseProjectFile();

		process::GetPathToLink();
		process::RunShellCmd(data::PathToLinker);
		osdebug << data::ShellCmdLog << endl;
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

	return 0;
}
