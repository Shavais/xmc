// process/CommandLineReader.h
#pragma once

#include <string>

namespace xmc
{
	class CommandLineReader
	{
	public:
		struct Args
		{
			std::string ProjectName;
			std::string ConfigName;
			bool Full = false;
			bool Test = false;
		};

		// Parses argc/argv and returns a populated Args struct.
		// Throws std::runtime_error if the project name argument is missing.
		static Args Parse(int argc, char* argv[]);
	};
}