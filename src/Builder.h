// Builder.h
#pragma once

#include "CommandLineReader.h"
#include "ProjectFileReader.h"

namespace xmc
{
	class Builder
	{
	public:
		// Derives one or more CompileJobs from args and project, then for each:
		//   1. Calls Compiler::Compile
		//   2. Calls Linker::Link if compile succeeded
		//
		// If args.FileFilter is set, one job is created per matching source file,
		// each named after the source file stem. Otherwise one job is created for
		// the entire project.
		//
		// If args.InjectTestMain is set, each job has InjectTestMain = true and
		// TestFilter forwarded from args.
		//
		// Compile and link timing are logged to the debug stream.
		// Logger::ErrorOccurred is set if any job fails.
		static void Build(const CmdLineArgs& args, const ProjectFileReader& project);

	private:
		// Resolves the list of .xm source files declared in the project,
		// optionally filtered by a wildcard pattern.
		static std::vector<std::string> ResolveSourceFiles(
			const ProjectFileReader& project,
			const std::string& fileFilter);

		// Builds the log file path for a component if that component's logging
		// is enabled in the project file. Returns empty string if disabled.
		static std::string ResolveLogPath(
			const ProjectFileReader& project,
			const std::string& logKey,    // e.g. "lexerlog"
			const std::string& dir,
			const std::string& stem,
			const std::string& ext);      // e.g. ".lexer"
	};

} // namespace xmc