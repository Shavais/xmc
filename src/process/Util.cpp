#include "pch.h"
#include "tool/Logger.h"
namespace process
{
	void RemoveFile(const std::string& pathname) {
		std::error_code ec;
		if (std::filesystem::exists(pathname)) {
			if (!std::filesystem::remove(pathname, ec)) {
				// If it fails, the file might be locked (e.g., the program is still running)
				osdebug << "Warning: Could not delete " << pathname << ": " << ec.message() << endl;
			}
		}
	}
}