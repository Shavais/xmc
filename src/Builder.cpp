// Builder.cpp
#include "pch/pch.h"

#include "Builder.h"
#include "Compiler.h"
#include "builder/Linker.h"

#include "tool/Logger.h"
#include "tool/StringFunctions.h"
#include "tool/TextParser.h"

#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace xmc
{
	// -------------------------------------------------------------------------
	// Wildcard matching (* and ? against a flat name, case-insensitive)
	// Does not treat ':' as a separator — namespace-qualified test names are
	// matched as a flat string.
	// -------------------------------------------------------------------------
	static bool WildMatch(std::string_view pattern, std::string_view text)
	{
		if (pattern.empty()) return true;  // empty pattern matches everything

		const char* p = pattern.data();
		const char* t = text.data();
		const char* pEnd = p + pattern.size();
		const char* tEnd = t + text.size();
		const char* starP = nullptr;
		const char* starT = nullptr;

		while (t < tEnd)
		{
			if (p < pEnd && (*p == '?' || tolower(*p) == tolower(*t)))
			{
				++p; ++t;
			}
			else if (p < pEnd && *p == '*')
			{
				starP = p++;
				starT = t;
			}
			else if (starP)
			{
				p = starP + 1;
				t = ++starT;
			}
			else return false;
		}

		while (p < pEnd && *p == '*') ++p;
		return p == pEnd;
	}

	// --------------------------------------------------------------------------------------------------------------------------------------------------
	// ResolveSourceFiles
	// --------------------------------------------------------------------------------------------------------------------------------------------------
	std::vector<std::string> Builder::ResolveSourceFiles(
		const ProjectFileReader& project,
		const std::string& fileFilter)
	{
		// Project-file syntax for sources is the same bracketed-list convention used by libpaths/staticlibs/etc.: [a.xm, b/*.xm, c.xm]. Bare entries
		// (no brackets) are also accepted -- TextParser's leading Skip eats them transparently.
		std::string raw = project.GetString("sources", "[*.xm]");

		std::vector<std::string> entries;
		{
			TextParser p(raw);
			p.Skip(" \t\r\n[");

			// get a list of globs like "*.xm", "myfile.xm", etc
			while (!p.Empty() && !p.CheckFor(']'))
			{
				auto [item, delim] = p.ReadUntil(",]", false);
				std::string_view trimmed = lrtrim(item, " \t\r\n\"");
				if (!trimmed.empty())
					entries.emplace_back(trimmed);

				if (delim == ',') p.Skip(",");
				p.Skip(" \t\r\n");
			}
		}

		// Expand any glob entries, keep plain paths as-is.
		std::vector<std::string> allFiles;
		for (const auto& entry : entries)
		{
			if (entry.find('*') != std::string::npos || entry.find('?') != std::string::npos)
			{
				fs::path dir = fs::path(entry).parent_path();
				std::string pat = fs::path(entry).filename().string();
				if (dir.empty()) dir = ".";
				for (const auto& de : fs::directory_iterator(dir))
				{
					if (de.is_regular_file() && WildMatch(pat, de.path().filename().string()))
						allFiles.push_back(de.path().string());
				}
			}
			else
			{
				allFiles.push_back(entry);
			}
		}

		// Apply command-line file filter, if any.
		if (fileFilter.empty()) return allFiles;

		std::vector<std::string> filtered;
		for (const auto& f : allFiles)
		{
			std::string stem = fs::path(f).stem().string();
			if (WildMatch(fileFilter, stem))
				filtered.push_back(f);
		}
		return filtered;
	}

	// -------------------------------------------------------------------------
	// ResolveLogPath
	// -------------------------------------------------------------------------
	std::string Builder::ResolveLogPath(
		const ProjectFileReader& project,
		const std::string& logKey,
		const std::string& dir,
		const std::string& stem,
		const std::string& ext)
	{
		// The project key may be 0/1 (on/off) or a directory path string.
		if (project.IsInt(logKey))
		{
			if (project.GetInt(logKey) == 0) return "";
			return (fs::path(dir) / (stem + ext)).string();
		}
		std::string val = project.GetString(logKey, "");
		if (val.empty() || val == "0" || val == "false") return "";
		// Treat non-empty string as a directory override
		return (fs::path(val) / (stem + ext)).string();
	}

	// -------------------------------------------------------------------------
	// Build
	// -------------------------------------------------------------------------
	void Builder::Build(const CmdLineArgs& args, const ProjectFileReader& project)
	{
		auto allFiles = ResolveSourceFiles(project, args.FileFilter);
		if (allFiles.empty())
		{
			oserror << "No source files found";
			if (!args.FileFilter.empty())
				oserror << " matching filter '" << args.FileFilter << "'";
			oserror << ".\n";
			throw std::runtime_error("No source files to compile.");
		}

		const std::string intDir = project.IntDir();
		const std::string outDir = project.OutDir();
		const bool        perFile = !args.FileFilter.empty();

		// Build a job list. Normally one job for the whole project; one-per-file
		// when a file filter is active (or when building for --test / --suite,
		// which also produce per-file executables).
		std::vector<CompileJob> jobs;

		auto makeJob = [&](const std::vector<std::string>& files, const std::string& stem) -> CompileJob
		{
			CompileJob job;
			job.SourceFiles = files;
			job.IntDir = intDir;
			job.ObjPath = (fs::path(outDir) / (stem + ".obj")).string();
			job.ExePath = (fs::path(outDir) / (stem + ".exe")).string();
			job.Full = args.Full;
			job.InjectTestMain = args.Test;
			job.TestFilter = args.TestFilter;
			job.LexerLog = (bool)project.GetInt("lexerlog");
			job.ParserLog = (bool)project.GetInt("parserlog");
			job.MorpherLog = (bool)project.GetInt("morpherlog");
			job.ReviewerLog = (bool)project.GetInt("reviewerlog");
			job.CoderLog = (bool)project.GetInt("coderlog");
			job.EmitterLog = (bool)project.GetInt("emitterlog");
			return job;
		};

		if (perFile || args.Test || args.Suite)
		{
			for (const auto& f : allFiles)
			{
				std::string stem = fs::path(f).stem().string();
				jobs.push_back(makeJob({ f }, stem));
			}
		}
		else
		{
			jobs.push_back(makeJob(allFiles, args.ProjectName));
		}

		// Compile and link each job, accumulating timing
		double totalCompile = 0.0;
		double totalLink = 0.0;

		bool anyError = false;
		for (const auto& job : jobs)
		{
			auto compileStart = std::chrono::high_resolution_clock::now();
			Compiler::Compile(job);
			auto compileEnd = std::chrono::high_resolution_clock::now();
			totalCompile += std::chrono::duration<double>(compileEnd - compileStart).count();

			if (job.ErrorOccurred)
			{
				anyError = true;
				continue;  // try remaining jobs, report at end
			}

			//auto linkStart = std::chrono::high_resolution_clock::now();
			//Linker::Link(job, project);
			//auto linkEnd = std::chrono::high_resolution_clock::now();
			//totalLink += std::chrono::duration<double>(linkEnd - linkStart).count();
		}

		if (!anyError)
		{
			osdebug << sformat("Compile time: %.3f s", totalCompile) << "\n";
			osdebug << sformat("Link time:    %.3f s", totalLink) << "\n";
			osdebug << sformat("Total time:   %.3f s", totalCompile + totalLink) << "\n";
		}
	}

} // namespace xmc