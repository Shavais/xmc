#pragma once

namespace process
{
	void GetPathToLinker();
	void GetLinkerArgs();
	void RunShellCmd(const std::string& commandLine);

}