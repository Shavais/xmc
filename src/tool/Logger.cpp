#include "pch.h"
#include "Logger.h"

#include <string>
#include <iostream>
#include <ostream>
#include <fstream>
#include <filesystem>
#include <shlobj.h>

#include "data/GlobalData.h"

namespace fs = std::filesystem;

using std::ostream;
using std::string;
using std::ofstream;

bool StopLogger = false;

static ofstream debug_file;
static ofstream error_file;

static Logger debug_stream(&debug_file, false);
static Logger error_stream(&error_file, true);

ostream osdebug(&debug_stream);
ostream oserror(&error_stream);

bool isDirectoryWritable(const std::wstring& dirPath);

#if defined(_DEBUG) || defined(FORCE_DEBUG_OUTPUT)
#define DebugOutput 1
#else
#define DebugOutput 0
#endif

std::wstring GetProgramDataPath() 
{
	WCHAR path[MAX_PATH];
	SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, path);
	return std::wstring(path) + L"\\svl";
}

void InitializeLogging()
{
	string errmsg = Logger::Init();
	if (!errmsg.empty()) std::cerr << "Could not intialize logging:\n";
}


// LogStream - - - - - - - - - - - -

/// <summary>
/// Creates the folder %TEMP%/svl, if it doesn't exist, and creates or truncates the files log.txt and error.txt in that folder
/// </summary>
/// <returns>null on success, a string error message on failure</returns>
const string Logger::Init()
{
	std::error_code errorCode;

	fs::path dataDir = GetProgramDataPath();
	if (!fs::create_directory(dataDir, errorCode) && errorCode.value() != 0)
	{
		return "Failed to create or open directory " + dataDir.string() + ":\n" + errorCode.message();
	}

	fs::path pathname = dataDir / "errors.txt";

	error_file.open(pathname, std::ios::out | std::ios::trunc);
	if (!error_file.is_open())
	{
		return GetLastErrorMessage();
	}

	if (DebugOutput)
	{
		pathname = dataDir / "debug.txt";
		debug_file.open(pathname, std::ios::out | std::ios::trunc);
		if (!debug_file.is_open())
		{
			return GetLastErrorMessage();
		}
	}

	return "";
}

Logger::Logger(std::ofstream* out, bool isErrorStream) : output_(out), isError_(isErrorStream) {}

Logger::~Logger()
{
}

std::streamsize Logger::xsputn(const char_type* s, std::streamsize n)
{
	if (StopLogger) return 0;

	if (isError_ && n > 0) data::ErrorOccurred = true;

	std::string input(s, n); 

	OutputDebugStringA(input.c_str());
	if (output_ && output_->is_open()) output_->write(input.c_str(), n);
	
	if (isError_) std::cerr.write(s, n);
	else std::cout.write(s, n);

	return n;
}

std::streambuf::int_type Logger::overflow(int_type c)
{
	if (c != std::char_traits<char>::eof()) {
		char_type ch = static_cast<char_type>(c);
		xsputn(&ch, 1);
	}
	return c;
}

int Logger::sync()
{
	if (StopLogger) return 0;
	if (output_ && output_->is_open()) output_->flush();

	if (isError_) std::cerr.flush();
	else std::cout.flush();

	return 0;
}

void removeTrailingCrLf(std::string& str) {
	if (str.size() >= 2 && str.substr(str.size() - 2) == "\r\n") {
		str.pop_back(); // Remove '\n'
		str.pop_back(); // Remove '\r'
	}
}

string GetLastErrorMessage()
{
	DWORD errorMessageID = GetLastError();

	LPSTR messageBuffer = nullptr;
	size_t size = FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL
	);
	string message(messageBuffer);
	LocalFree(messageBuffer);

	removeTrailingCrLf(message);
	return message;
}

bool isDirectoryWritable(const std::wstring& dirPath) {
	DWORD desiredAccess = FILE_ADD_FILE | FILE_WRITE_DATA;
	HANDLE hDir = CreateFileW(
		dirPath.c_str(),
		desiredAccess,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr
	);

	if (hDir != INVALID_HANDLE_VALUE) {
		CloseHandle(hDir);
		return true;
	}

	return false;
}


extern "C" void SVGE_Log(const char* msg) {
	std::cout << "[SVGE Engine]: " << msg << std::endl;
}
