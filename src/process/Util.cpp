#include "pch.h"
#include "Util.h"

#include <filesystem>

#include "tool/Logger.h"
#include "tool/StringFunctions.h"

namespace fs = std::filesystem;

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

	std::string GetWin32ErrorString(DWORD errorCode) {
		if (errorCode == 0) return "No error.";

		LPWSTR messageBuffer = nullptr;
		DWORD size = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			errorCode,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPWSTR)&messageBuffer,
			0,
			NULL
		);

		if (size == 0) return "Unknown error code: " + std::to_string(errorCode);

		// Calculate required buffer size for UTF-8
		int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, messageBuffer, (int)size, NULL, 0, NULL, NULL);
		std::string result(sizeNeeded, 0);

		// Perform the actual conversion
		WideCharToMultiByte(CP_UTF8, 0, messageBuffer, (int)size, &result[0], sizeNeeded, NULL, NULL);

		LocalFree(messageBuffer);

		// Clean up trailing newlines FormatMessage often adds
		if (!result.empty() && result.back() == '\n') result.pop_back();
		if (!result.empty() && result.back() == '\r') result.pop_back();

		return result;
	}

	std::string ReadStringFromPtr(const uint8_t* base, uint64_t& offset) {
		uint32_t len = ReadPodFromPtr<uint32_t>(base, offset);
		std::string str(reinterpret_cast<const char*>(base + offset), len);
		offset += len;
		return str;
	}


	std::string ReadString(const std::vector<uint8_t>& buf, uint64_t& offset) {
		uint32_t len = ReadPod<uint32_t>(buf, offset);
		if (offset + len <= buf.size()) {
			std::string s(reinterpret_cast<const char*>(&buf[offset]), len);
			offset += len;
			return s;
		}
		return "";
	}

	void WriteString(std::vector<uint8_t>& buf, const std::string& s) {
		uint32_t len = static_cast<uint32_t>(s.size());
		WritePod(buf, len);
		buf.insert(buf.end(), s.begin(), s.end());
	}

	std::vector<uint8_t> FastReadBinaryFile(const fs::path& path) {
		// 1. Open the file handle with sequential scan hint for SSD optimization
		HANDLE hFile = CreateFileW(
			path.wstring().c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
			NULL
		);

		if (hFile == INVALID_HANDLE_VALUE) {
			// Only log if the file actually exists but failed to open
			if (fs::exists(path)) {
				oserror << sformat("Failed to open binary file: %s", path.string().c_str()) << endl;
			}
			return {};
		}

		// 2. Get file size for mapping
		LARGE_INTEGER size;
		if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
			CloseHandle(hFile);
			return {};
		}

		// 3. Create a file mapping object
		HANDLE hMapping = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
		if (!hMapping) {
			oserror << sformat("Failed to create file mapping: %s", path.string().c_str()) << endl;
			CloseHandle(hFile);
			return {};
		}

		// 4. Map the view into memory
		void* pData = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
		std::vector<uint8_t> content;

		if (pData) {
			// Copy the mapped data into our vector
			// This is typically faster than ReadFile because it bypasses extra system buffering
			content.assign(
				static_cast<const uint8_t*>(pData),
				static_cast<const uint8_t*>(pData) + size.QuadPart
			);
			UnmapViewOfFile(pData);
		}

		// 5. Cleanup handles
		CloseHandle(hMapping);
		CloseHandle(hFile);

		return content;
	}

	void FastWriteBinaryFile(const fs::path& outPath, const std::vector<uint8_t>& buf) {
		HANDLE hFile = CreateFileW(outPath.wstring().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			DWORD err = GetLastError();
			oserror << sformat("Failed to create file: %s. Error: %s", outPath.string().c_str(), GetWin32ErrorString(err).c_str()) << endl;
			return;
		}

		LARGE_INTEGER liSize;
		liSize.QuadPart = buf.size();
		if (SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN)) {
			SetEndOfFile(hFile);
			SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
		}

		DWORD written;
		if (!WriteFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &written, NULL)) {
			DWORD err = GetLastError();
			oserror << sformat("Failed to write to file: %s. Error: %s", outPath.string().c_str(), GetWin32ErrorString(err).c_str()) << endl;
		}

		CloseHandle(hFile);
	}

}