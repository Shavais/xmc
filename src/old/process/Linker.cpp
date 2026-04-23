#include "pch.h"
#include "Linker.h"

#include <string>
#include <algorithm>
#include <filesystem> 

#include "tool/Logger.h"
#include "tool/TextParser.h"
#include "tool/StringFunctions.h"
#include "data/LinkerData.h"
#include "data/CmdLineData.h"
#include "process/ProjectFile.h"

namespace process
{

	void RunShellCmd(const std::string& commandLine) {
		data::ShellCmdLog.clear();

		HANDLE hChildStdOutRead = NULL;
		HANDLE hChildStdOutWrite = NULL;

		// Set up security attributes to allow the pipe handles to be inherited
		SECURITY_ATTRIBUTES saAttr;
		saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
		saAttr.bInheritHandle = TRUE;
		saAttr.lpSecurityDescriptor = NULL;

		// Create a pipe for the child process's STDOUT and STDERR
		if (!CreatePipe(&hChildStdOutRead, &hChildStdOutWrite, &saAttr, 0)) {
			oserror << "XMC Error: Failed to create output pipe.";
			return;
		}

		// Ensure the reading handle is NOT inherited
		SetHandleInformation(hChildStdOutRead, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOA si = { sizeof(si) };
		si.hStdError = hChildStdOutWrite;
		si.hStdOutput = hChildStdOutWrite; // Capture both into the same log
		si.dwFlags |= STARTF_USESTDHANDLES;

		PROCESS_INFORMATION pi;

		// InheritHandles must be TRUE to pass the pipe to the child
		BOOL success = CreateProcessA(
			NULL,
			(LPSTR)commandLine.c_str(),
			NULL, NULL,
			TRUE,
			CREATE_NO_WINDOW,
			NULL, NULL, &si, &pi
		);

		if (success) {
			// IMPORTANT: Close the write end in the parent, or ReadFile will block forever
			CloseHandle(hChildStdOutWrite);

			char buffer[4096];
			DWORD bytesRead;
			// Read from pipe until it is closed by the child
			while (ReadFile(hChildStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
				buffer[bytesRead] = '\0';
				data::ShellCmdLog += buffer;
			}

			WaitForSingleObject(pi.hProcess, INFINITE);

			DWORD exitCode = 0;
			GetExitCodeProcess(pi.hProcess, &exitCode);
			if (exitCode != 0 && data::ShellCmdLog.empty()) {
				data::ShellCmdLog = "Process failed with exit code: " + std::to_string(exitCode);
			}

			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
		}
		else {
			data::ShellCmdLog = "XMC Error: Failed to launch linker process.";
			CloseHandle(hChildStdOutWrite); // Cleanup on failure
		}

		CloseHandle(hChildStdOutRead);
	}

	void GetPathToLinker() {
		namespace fs = std::filesystem;
		data::PathToLinker.clear();

		// Check if vswhere exists
		std::string vswherePath = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
		if (!fs::exists(vswherePath)) {
			oserror << "XMC Error: vswhere.exe not found at " + vswherePath + ". Is Visual Studio installed?" << endl;
			return;
		}

		// Run vswhere to find VS installation root
		std::string command = "\"" + vswherePath + "\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath";

		std::string vsRoot = "";
		char buffer[MAX_PATH];
		FILE* pipe = _popen(command.c_str(), "r");

		if (pipe) {
			if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
				vsRoot = buffer;
				vsRoot.erase(vsRoot.find_last_not_of("\r\n") + 1);
			}
			_pclose(pipe);
		}

		if (vsRoot.empty()) {
			oserror << "XMC Error: vswhere could not find a Visual Studio installation with C++ Build Tools." << endl;
			return;
		}

		// Convert backslashes for consistency
		std::replace(vsRoot.begin(), vsRoot.end(), '\\', '/');

		// Get the default MSVC version string
		std::string versionFile = vsRoot + "/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt";
		std::string version = "";

		if (fs::exists(versionFile)) {
			std::ifstream vFile(versionFile);
			if (vFile.is_open()) {
				if (std::getline(vFile, version)) {
					version.erase(version.find_last_not_of("\r\n") + 1);
				}
				vFile.close();
			}
		}

		if (version.empty()) {
			oserror << "Could not determine MSVC version from " << versionFile << endl;
			return;
		}

		// Set the final path
		std::string finalPath = vsRoot + "/VC/Tools/MSVC/" + version + "/bin/Hostx64/x64/link.exe";

		if (fs::exists(finalPath)) {
			data::PathToLinker = finalPath;
		}
		else {
			oserror << "XMC Error: Linker not found at expected path: " << finalPath << endl;
		}
	}

	void GetLinkerArgs() {
		using namespace process;
		using namespace data;

		auto AppendFromList = 
			[&](std::string& out, const std::string& key, const std::string& prefix = "", const std::string& defaultExt = "") {
				std::string raw = GetString(key, "");
				if (raw.empty()) return;

				TextParser p(raw);
				p.Skip(" \t\r\n["); // Skip start of list

				while (!p.Empty() && !p.CheckFor(']')) {
					auto [item, delim] = p.ReadUntil(",]", false);

					// Clean up whitespace/newlines/quotes from the path
					string_view trimmed = lrtrim(item, " \t\r\n\"");

					if (!trimmed.empty()) {
						std::string entry(trimmed);

						// Only add extension if one isn't already present
						if (!defaultExt.empty() && entry.find('.') == std::string::npos) {
							entry += defaultExt;
						}

						out += " " + prefix + "\"" + entry + "\"";
					}

					if (delim == ',') p.Skip(",");
					p.Skip(" \t\r\n"); // Crucial for multi-line LibPaths

				}
			}
		;
		// Build the string
		std::string args = " /NOLOGO";

		// Subsystem & Entry
		args += " /SUBSYSTEM:" + GetString("subsystem", "CONSOLE");
		args += " /ENTRY:mainCRTStartup";

		// CRT Selection (Maps to your working set)
		std::string crt = Lowercase(GetString("crt", "static"));
		if (crt == "static" || crt == "static-debug") {
			// Since your current manual test uses debug libs:
			args += " libcmtd.lib libcpmtd.lib libvcruntimed.lib libucrtd.lib";
			args += " /NODEFAULTLIB:ucrtd.lib /NODEFAULTLIB:vcruntimed.lib /NODEFAULTLIB:msvcrtd.lib /NODEFAULTLIB:msvcprtd.lib";
		}

		// Process the .xmc lists
		AppendFromList(args, "libpaths", "/LIBPATH:");
		AppendFromList(args, "staticlibs");  // No default ext needed, you have .obj in xmc
		AppendFromList(args, "dynamiclibs"); // No default ext needed, you have .lib in xmc

		// Throw away unused functions
		args += " /OPT:REF /OPT:ICF";

		// Primary Input
		args += " " + data::CmdLineArgs.ProjectName + ".obj";
		args += " /OUT:\"" + CmdLineArgs.ProjectName + ".exe\"";

		data::LinkerArgs = args;
	}
}