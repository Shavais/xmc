// sv/util/StringFunctions.cpp
#include "pch/pch.h"
#include "StringFunctions.h"

#include <iosfwd>

#include "Logger.h"

namespace sv
{
	using std::endl;

	string GetErrorMessage(int errval)
	{
		if (errval == 0) return "";

		char buffer[2048];
		strerror_s(buffer, sizeof(buffer), errval);
		return buffer;
	}

	string DateTimeStamp()
	{
		time_t rawtime;
		time(&rawtime);

		struct tm timeinfo;
		errno_t result = localtime_s(&timeinfo, &rawtime);
		if (result != 0)
		{
			errlog << "Error getting local time: " << GetErrorMessage(result) << endl;
			return "";
		}

		char buffer[64];
		uint64_t u8sWritten = strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
		if (u8sWritten == 0) {
			errlog << "Error formatting time string: " << GetErrorMessage(errno) << std::endl;
			return "";
		}

		return buffer;
	}

}
