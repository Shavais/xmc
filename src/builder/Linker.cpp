// builder/Linker.cpp
#include "pch/pch.h"
#include "Linker.h"

#include "tool/Logger.h"
#include "tool/TextParser.h"
#include "tool/StringFunctions.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace xmc
{
	// -------------------------------------------------------------------------
	// Link  (public)
	// -------------------------------------------------------------------------
	void Linker::Link(const CompileJob& job, const ProjectFileReader& project)
	{
		std::string linkerPath = FindLinkerPath();
		if (linkerPath.empty()) return;  // error already logged

		std::string args = BuildArgs(job, project);
		std::string cmdLine = "\"" + linkerPath + "\"" + args;
		std::string output = RunProcess(cmdLine);

		if (!output.empty())
			osdebug << output << "\n";
	}

	// -------------------------------------------------------------------------
	// FindLinkerPath  (private)
	// -------------------------------------------------------------------------
	std::string Linker::FindLinkerPath()
	{
		std::string vswherePath =
			"C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";

		if (!fs::exists(vswherePath))
		{
			oserror << "error: vswhere.exe not found at " << vswherePath
				<< ". Is Visual Studio installed?\n";
			return "";
		}

		// Ask vswhere for the VS installation root
		std::string command =
			"\"" + vswherePath + "\""
			" -latest -products *"
			" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
			" -property installationPath";

		std::string vsRoot;
		char buffer[MAX_PATH];
		FILE* pipe = _popen(command.c_str(), "r");
		if (pipe)
		{
			if (fgets(buffer, sizeof(buffer), pipe))
			{
				vsRoot = buffer;
				vsRoot.erase(vsRoot.find_last_not_of("\r\n") + 1);
			}
			_pclose(pipe);
		}

		if (vsRoot.empty())
		{
			oserror << "error: vswhere could not find a Visual Studio installation"
				" with C++ Build Tools.\n";
			return "";
		}

		std::replace(vsRoot.begin(), vsRoot.end(), '\\', '/');

		// Read the default MSVC toolset version
		std::string versionFile = vsRoot + "/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt";
		std::string version;
		std::ifstream vf(versionFile);
		if (vf.is_open())
		{
			std::getline(vf, version);
			version.erase(version.find_last_not_of("\r\n") + 1);
		}

		if (version.empty())
		{
			oserror << "error: could not determine MSVC version from " << versionFile << "\n";
			return "";
		}

		std::string linkerPath =
			vsRoot + "/VC/Tools/MSVC/" + version + "/bin/Hostx64/x64/link.exe";

		if (!fs::exists(linkerPath))
		{
			oserror << "error: linker not found at expected path: " << linkerPath << "\n";
			return "";
		}

		return linkerPath;
	}

	// -------------------------------------------------------------------------
	// BuildArgs  (private)
	// -------------------------------------------------------------------------
	std::string Linker::BuildArgs(const CompileJob& job, const ProjectFileReader& project)
	{
		// Helper: reads a [item, item, ...] list from a project key and appends
		// each entry to `out`, with an optional prefix and default extension.
		auto appendList = [&](std::string& out,
			const std::string& key,
			const std::string& prefix = "",
			const std::string& defaultExt = "")
			{
				std::string raw = project.GetString(key, "");
				if (raw.empty()) return;

				TextParser p(raw);
				p.Skip(" \t\r\n[");

				while (!p.Empty() && !p.CheckFor(']'))
				{
					auto [item, delim] = p.ReadUntil(",]", false);
					std::string_view trimmed = lrtrim(item, " \t\r\n\"");

					if (!trimmed.empty())
					{
						std::string entry(trimmed);
						if (!defaultExt.empty() && entry.find('.') == std::string::npos)
							entry += defaultExt;
						out += " " + prefix + "\"" + entry + "\"";
					}

					if (delim == ',') p.Skip(",");
					p.Skip(" \t\r\n");
				}
			};

		std::string args = " /NOLOGO";

		// Subsystem and entry point
		args += " /SUBSYSTEM:" + project.GetString("subsystem", "CONSOLE");
		args += " /ENTRY:mainCRTStartup";

		// CRT selection
		std::string crt = Lowercase(project.GetString("crt", "static"));
		if (crt == "static" || crt == "static-debug")
		{
			args += " libcmtd.lib libcpmtd.lib libvcruntimed.lib libucrtd.lib";
			args += " /NODEFAULTLIB:ucrtd.lib"
				" /NODEFAULTLIB:vcruntimed.lib"
				" /NODEFAULTLIB:msvcrtd.lib"
				" /NODEFAULTLIB:msvcprtd.lib";
		}

		// Library paths and libs from project file
		appendList(args, "libpaths", "/LIBPATH:");
		appendList(args, "staticlibs");
		appendList(args, "dynamiclibs");

		// Dead-code elimination
		args += " /OPT:REF /OPT:ICF";

		// Input and output
		args += " \"" + job.ObjPath + "\"";
		args += " /OUT:\"" + job.ExePath + "\"";

		return args;
	}

	// -------------------------------------------------------------------------
	// RunProcess  (private)
	// -------------------------------------------------------------------------
	std::string Linker::RunProcess(const std::string& commandLine)
	{
		HANDLE hRead = NULL;
		HANDLE hWrite = NULL;

		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		sa.lpSecurityDescriptor = NULL;

		if (!CreatePipe(&hRead, &hWrite, &sa, 0))
		{
			oserror << "error: failed to create pipe for linker output.\n";
			return "";
		}

		// The read end must not be inherited by the child
		SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOA si{};
		si.cb = sizeof(si);
		si.hStdOutput = hWrite;
		si.hStdError = hWrite;
		si.dwFlags = STARTF_USESTDHANDLES;

		PROCESS_INFORMATION pi{};

		BOOL launched = CreateProcessA(
			NULL,
			const_cast<LPSTR>(commandLine.c_str()),
			NULL, NULL,
			TRUE,               // inherit handles
			CREATE_NO_WINDOW,
			NULL, NULL,
			&si, &pi);

		// Parent must close its copy of the write end or ReadFile blocks forever
		CloseHandle(hWrite);

		if (!launched)
		{
			oserror << "error: failed to launch linker process.\n";
			CloseHandle(hRead);
			return "";
		}

		std::string output;
		char buf[4096];
		DWORD bytesRead;
		while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0)
		{
			buf[bytesRead] = '\0';
			output += buf;
		}

		WaitForSingleObject(pi.hProcess, INFINITE);

		DWORD exitCode = 0;
		GetExitCodeProcess(pi.hProcess, &exitCode);
		if (exitCode != 0)
		{
			if (output.empty())
				output = "Linker failed with exit code: " + std::to_string(exitCode);
			oserror << "error: linker exited with code " << exitCode << "\n";
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		CloseHandle(hRead);

		return output;
	}

} // namespace xmc