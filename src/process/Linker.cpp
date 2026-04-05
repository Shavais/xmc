#include "pch.h"
#include "Linker.h"

#include "tool/Logger.h"
#include "data/LinkerData.h"


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
}