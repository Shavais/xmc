// compiler/Compiler.h
//
// Drives one CompileJob end-to-end: triage, load unmodified .xmo
// headers, per-file Lex/Parse/Morph pipeline tasks (one pool task per
// modified .xm, three jthreads inside it linked by blocking concurrent
// queues), barrier and break-out recovery, Reviewer, Coder, Emitter
// (phase 1 + phase 2 + .xmo save + .obj write). Builder constructs the
// job; Compiler::Compile is the entry point; Linker::Link picks up the
// .obj after we return.
//
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace xmc
{
	// -----------------------------------------------------------------
	// CompileJob
	//
	// One complete compilation unit. Builder fills it in, Compiler reads
	// from it (and toggles the mutable atomic break-out and error flags
	// as needed), Linker reads from it.
	// -----------------------------------------------------------------
	struct CompileJob
	{
		std::vector<std::string> SourceFiles;
		std::string              IntDir;
		std::string              ObjPath;
		std::string              ExePath;
		bool                     Full = false;
		bool                     InjectTestMain = false;
		std::string              TestFilter;

		// Maximum number of source files compiled in parallel. Each
		// file occupies one pool worker plus three jthreads (Lex /
		// Parse / Morph) for its pipeline. Default 6.
		uint32_t MaxConcurrentFiles = 6;

		// Per-stage logs. When true, each compiled .xm produces a sibling
		// log file named "<xm-stem>.<stage>.txt" next to the source.
		bool LexerLog = false;
		bool ParserLog = false;
		bool MorpherLog = false;
		bool ReviewerLog = false;
		bool CoderLog = false;
		bool EmitterLog = false;

		// Pipeline control flags. The Morpher sets these from any worker
		// thread; the dispatcher reads them at the barrier in Compile().
		//
		// ReparseRequired: a structural change (base-type morph, or one
		// of the structural refinement bits Variant/Arc/Concurrent/Fluid)
		// reached an unmodified observer. The observer's parse tree may
		// compile to different node shapes; its source must be re-parsed.
		//
		// ParseTreeScanRequired: a non-structural refinement narrowing
		// reached such an observer. Parse-tree shape is still valid;
		// only the Morpher needs to re-run on the loaded parse tree.
		//
		// Each flag fires at most once per job (compiler invariant).
		mutable std::atomic<bool> ReparseRequired{ false };
		mutable std::atomic<bool> ParseTreeScanRequired{ false };

		// Set by any stage that emits a fatal diagnostic. Builder skips
		// the linker for jobs whose ErrorOccurred is set on return.
		mutable std::atomic<bool> ErrorOccurred{ false };

		CompileJob() = default;

		// std::atomic is neither copyable nor movable; CompileJob is, so
		// we hand-roll copy/move that loads through the atomics.
		CompileJob(const CompileJob& other)
			: SourceFiles(other.SourceFiles)
			, IntDir(other.IntDir)
			, ObjPath(other.ObjPath)
			, ExePath(other.ExePath)
			, Full(other.Full)
			, InjectTestMain(other.InjectTestMain)
			, TestFilter(other.TestFilter)
			, MaxConcurrentFiles(other.MaxConcurrentFiles)
			, LexerLog(other.LexerLog)
			, ParserLog(other.ParserLog)
			, MorpherLog(other.MorpherLog)
			, ReviewerLog(other.ReviewerLog)
			, ReparseRequired(other.ReparseRequired.load())
			, ParseTreeScanRequired(other.ParseTreeScanRequired.load())
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
			, MaxConcurrentFiles(other.MaxConcurrentFiles)
			, LexerLog(std::move(other.LexerLog))
			, ParserLog(std::move(other.ParserLog))
			, MorpherLog(std::move(other.MorpherLog))
			, ReviewerLog(std::move(other.ReviewerLog))
			, ReparseRequired(other.ReparseRequired.load())
			, ParseTreeScanRequired(other.ParseTreeScanRequired.load())
			, ErrorOccurred(other.ErrorOccurred.load())
		{
		}
	};

	// -----------------------------------------------------------------
	// Compiler
	//
	// Per-job state lives on Compile()'s stack in an internal helper
	// struct; this class is just the public entry point.
	// -----------------------------------------------------------------
	class Compiler
	{
	public:
		static void Compile(const CompileJob& job);
	};

} // namespace xmc