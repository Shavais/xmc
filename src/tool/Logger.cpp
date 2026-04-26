#include "pch/pch.h"
#include "Logger.h"
#include <string>
#include <iostream>
#include <ostream>
#include <fstream>
#include <filesystem>
#include <shlobj.h>
#include <mutex>

namespace fs = std::filesystem;
using std::ostream;
using std::string;
using std::ofstream;

// Definitions
std::mutex Logger::writeMutex;
static thread_local std::string threadBuffer;

bool StopLogger = false;
static ofstream debug_file;
static ofstream error_file;
static Logger debug_stream(&debug_file, false);
static Logger error_stream(&error_file, true);
ostream osdebug(&debug_stream);
ostream oserror(&error_stream);

#if defined(_DEBUG) || defined(FORCE_DEBUG_OUTPUT)
#define DebugOutput 1
#else
#define DebugOutput 0
#endif

// Logic stays the same, just ensured it's declared for Init()
std::wstring GetProgramDataPath() {
	WCHAR path[MAX_PATH];
	SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, path);
	return std::wstring(path) + L"\\xmc";
}

void InitializeLogging() {
	string errmsg = Logger::Init();
	if (!errmsg.empty()) std::cerr << "Could not intialize logging:\n" << errmsg << "\n";
}

const string Logger::Init() {
	std::error_code errorCode;
	// Explicit call to our specific function to avoid "overloaded-function" error
	fs::path dataDir = GetProgramDataPath();

	if (!fs::create_directory(dataDir, errorCode) && errorCode.value() != 0) {
		return "Failed to create directory " + dataDir.string() + ": " + errorCode.message();
	}

	fs::path pathname = dataDir / "errors.txt";
	error_file.open(pathname, std::ios::out | std::ios::trunc);
	if (!error_file.is_open()) return GetLastErrorMessage();

	if (DebugOutput) {
		pathname = dataDir / "debug.txt";
		debug_file.open(pathname, std::ios::out | std::ios::trunc);
		if (!debug_file.is_open()) return GetLastErrorMessage();
	}
	return "";
}

Logger::Logger(std::ofstream* out, bool isErrorStream) : output_(out), isError_(isErrorStream) {}
Logger::~Logger() {}

std::streamsize Logger::xsputn(const char_type* s, std::streamsize n) {
	if (StopLogger) return 0;

	// Buffer per thread until a newline to prevent character intermixing
	for (std::streamsize i = 0; i < n; ++i) {
		threadBuffer += s[i];
		if (s[i] == '\n') {
			CommitLine(threadBuffer);
			threadBuffer.clear();
		}
	}
	return n;
}

std::streambuf::int_type Logger::overflow(int_type c) {
	if (StopLogger) return EOF;
	if (c != std::char_traits<char>::eof()) {
		char ch = static_cast<char>(c);
		xsputn(&ch, 1);
	}
	return c;
}

void Logger::CommitLine(const std::string& line) {
	if (isError_ && !line.empty()) ErrorOccurred = true;

	// Mutex lock ensures one thread writes its whole line before the next
	std::lock_guard<std::mutex> lock(writeMutex);

	OutputDebugStringA(line.c_str());
	if (output_ && output_->is_open()) {
		output_->write(line.c_str(), line.size());
		output_->flush();
	}

	if (isError_) {
		std::cerr.write(line.c_str(), line.size());
		std::cerr.flush();
	}
	else {
		std::cout.write(line.c_str(), line.size());
		std::cout.flush();
	}
}

int Logger::sync() {
	if (StopLogger) return 0;
	if (!threadBuffer.empty()) {
		CommitLine(threadBuffer);
		threadBuffer.clear();
	}
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
	uint64_t size = FormatMessageA(
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
