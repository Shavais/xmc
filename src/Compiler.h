// Compiler.h
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace xmc
{
	// Describes one complete compilation unit — a set of source files compiled
	// together into a single .obj. Builder constructs these and passes them to
	// Compiler::Compile and then Linker::Link.
	struct CompileJob
	{
		std::vector<std::string> SourceFiles;
		std::string              IntDir;
		std::string              ObjPath;
		std::string              ExePath;
		bool                     Full = false;
		bool                     InjectTestMain = false;
		std::string              TestFilter;
		std::string LexerLog;
		std::string ParserLog;
		std::string TyperLog;
		std::string RefinerLog;
		mutable std::atomic<bool> RefinementBreakOut = false;
		mutable std::atomic<bool> InferenceBreakOut = false;
		mutable std::atomic_bool ErrorOccurred = false;

		CompileJob() = default;

		CompileJob(const CompileJob& other)
			: SourceFiles(other.SourceFiles)
			, IntDir(other.IntDir)
			, ObjPath(other.ObjPath)
			, ExePath(other.ExePath)
			, Full(other.Full)
			, InjectTestMain(other.InjectTestMain)
			, TestFilter(other.TestFilter)
			, LexerLog(other.LexerLog)
			, ParserLog(other.ParserLog)
			, TyperLog(other.TyperLog)
			, RefinerLog(other.RefinerLog)
			, RefinementBreakOut(other.RefinementBreakOut.load())
			, InferenceBreakOut(other.InferenceBreakOut.load())
			, ErrorOccurred(other.ErrorOccurred.load())
		{
		}

		CompileJob(CompileJob&& other) noexcept
			: SourceFiles(std::move(other.SourceFiles))
			, IntDir(std::move(other.IntDir))
			, ObjPath(std::move(other.ObjPath))
			, ExePath(std::move(other.ExePath))
			, Full(other.Full)
			, InjectTestMain(other.InjectTestMain)
			, TestFilter(std::move(other.TestFilter))
			, LexerLog(std::move(other.LexerLog))
			, ParserLog(std::move(other.ParserLog))
			, TyperLog(std::move(other.TyperLog))
			, RefinerLog(std::move(other.RefinerLog))
			, RefinementBreakOut(other.RefinementBreakOut.load())
			, InferenceBreakOut(other.InferenceBreakOut.load())
			, ErrorOccurred(other.ErrorOccurred.load())
		{
		}
	};

	class Compiler
	{
	public:
		// Drives the full pipeline (Lexer -> Parser -> Typer -> Refiner ->
		// Reviewer -> Coder -> Emitter) for the given job. Writes .xmo
		// intermediates to job.IntDir and the final .obj to job.ObjPath.
		// Errors are reported via Logger; Logger sets Logger::ErrorOccurred if oserror is used.
		static void Compile(const CompileJob& job);
	};

} // namespace xmc