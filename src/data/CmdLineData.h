// data/CmdLineData.h
#pragma once

#include <string>

namespace data
{
	struct CmdLineArgs_
	{
		std::string ProjectName;
		std::string ConfigName;
		bool Full = false;
	};

	inline CmdLineArgs_ CmdLineArgs;
}
