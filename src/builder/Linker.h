// builder/Linker.h
#pragma once

#include "Compiler.h"
#include "ProjectFileReader.h"

#include <string>

namespace xmc
{
	class Linker
	{
	public:
		// Invokes the platform linker to produce job.ExePath from job.ObjPath.
		// Library paths, subsystem, and CRT selection are read from project.
		// Output from the linker process is written to the debug log stream.
		// Errors are reported via Logger.
		static void Link(const CompileJob& job, const ProjectFileReader& project);

	private:
		// Locates link.exe via vswhere. Returns empty string and logs an error
		// if Visual Studio with C++ build tools cannot be found.
		static std::string FindLinkerPath();

		// Builds the full MSVC linker argument string from job and project settings.
		static std::string BuildArgs(const CompileJob& job, const ProjectFileReader& project);

		// Launches a child process, captures combined stdout/stderr, and returns
		// the captured output. Logs an error and returns empty if launch fails.
		static std::string RunProcess(const std::string& commandLine);
	};

} // namespace xmc