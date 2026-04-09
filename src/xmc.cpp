#include "pch.h"

#include <iostream>

#include "experiments.h"

#include "data/CmdLineData.h"
#include "process/CmdLine.h"
#include "process/ProjectFile.h"
#include "tool/Logger.h"

#include "process/Linker.h"
#include "data/LinkerData.h"
#include "process/Util.h"

using namespace process;

int main(int argc, char* argv[])
{
	try
	{
		InitializeLogging();

		RemoveFile("hello.obj");
		CallCppFunction2();		// produces hello.obj 
		
		ParseCommandLine(argc, argv);	
		ParseProjectFile();

		GetPathToLinker();
		GetLinkerArgs();		// populates data::LinkerArgs

		//data::LinkerArgs =
		//	" /NOLOGO"
		//	" /subsystem:console"
		//	" /entry:mainCRTStartup"
		//	" /LIBPATH:\"C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/lib/x64\""
		//	" /LIBPATH:\"C:/Program Files (x86)/Windows Kits/10/Lib/10.0.19041.0/ucrt/x64\""
		//	" /LIBPATH:\"C:/Program Files (x86)/Windows Kits/10/Lib/10.0.19041.0/um/x64\""
		//	" hello.obj logger.obj pch.obj"
		//	" libcmtd.lib libcpmtd.lib libvcruntimed.lib libucrtd.lib kernel32.lib shell32.lib"
		//	" /NODEFAULTLIB:ucrtd.lib /NODEFAULTLIB:vcruntimed.lib /NODEFAULTLIB:msvcrtd.lib /NODEFAULTLIB:msvcprtd.lib"
		//;

		RemoveFile("hello.exe");
		RunShellCmd(data::PathToLinker + data::LinkerArgs);
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
