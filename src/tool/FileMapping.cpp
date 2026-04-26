// tool/FileMapping.cpp
#include "pch/pch.h"
#include "FileMapping.h"

#include "tool/Logger.h"

#if defined(_WIN32)
#include "pch/win32.h"
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace xmc
{
	// -------------------------------------------------------------------------
	// Lifetime
	// -------------------------------------------------------------------------

	FileMapping::~FileMapping()
	{
		Unmap();
	}

	FileMapping::FileMapping(FileMapping&& other) noexcept
		: base_(other.base_),
		size_(other.size_),
		handle_(other.handle_),
		mapHandle_(other.mapHandle_)
	{
		other.base_ = nullptr;
		other.size_ = 0;
		other.handle_ = nullptr;
		other.mapHandle_ = nullptr;
	}

	FileMapping& FileMapping::operator=(FileMapping&& other) noexcept
	{
		if (this != &other) {
			Unmap();
			base_ = other.base_;
			size_ = other.size_;
			handle_ = other.handle_;
			mapHandle_ = other.mapHandle_;
			other.base_ = nullptr;
			other.size_ = 0;
			other.handle_ = nullptr;
			other.mapHandle_ = nullptr;
		}
		return *this;
	}

	// -------------------------------------------------------------------------
	// Map / Unmap
	// -------------------------------------------------------------------------

#if defined(_WIN32)

	bool FileMapping::Map(const std::filesystem::path& path)
	{
		Unmap();

		// path::c_str() returns wchar_t* on Windows -- pass directly to
		// CreateFileW so non-ASCII paths work without manual conversion.
		HANDLE h = ::CreateFileW(
			path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);

		if (h == INVALID_HANDLE_VALUE) {
			oserror << "FileMapping: CreateFileW failed for "
				<< path.string()
				<< " (GetLastError=" << ::GetLastError() << ")" << std::endl;
			return false;
		}

		LARGE_INTEGER fileSize{};
		if (!::GetFileSizeEx(h, &fileSize)) {
			oserror << "FileMapping: GetFileSizeEx failed for "
				<< path.string()
				<< " (GetLastError=" << ::GetLastError() << ")" << std::endl;
			::CloseHandle(h);
			return false;
		}

		// Empty file: succeed with a "mapped but no view" state. Callers
		// check Size() before reading. We retain the file handle so
		// IsMapped() returns true and the object owns *something*.
		if (fileSize.QuadPart == 0) {
			handle_ = h;
			mapHandle_ = nullptr;
			base_ = nullptr;
			size_ = 0;
			return true;
		}

		HANDLE mh = ::CreateFileMappingW(
			h, nullptr, PAGE_READONLY,
			0, 0,                  // map full file
			nullptr);

		if (mh == nullptr) {
			oserror << "FileMapping: CreateFileMappingW failed for "
				<< path.string()
				<< " (GetLastError=" << ::GetLastError() << ")" << std::endl;
			::CloseHandle(h);
			return false;
		}

		void* view = ::MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
		if (view == nullptr) {
			oserror << "FileMapping: MapViewOfFile failed for "
				<< path.string()
				<< " (GetLastError=" << ::GetLastError() << ")" << std::endl;
			::CloseHandle(mh);
			::CloseHandle(h);
			return false;
		}

		handle_ = h;
		mapHandle_ = mh;
		base_ = view;
		size_ = static_cast<uint64_t>(fileSize.QuadPart);
		return true;
	}

	void FileMapping::Unmap()
	{
		if (base_) { ::UnmapViewOfFile(base_);  base_ = nullptr; }
		if (mapHandle_) { ::CloseHandle(mapHandle_); mapHandle_ = nullptr; }
		if (handle_) { ::CloseHandle(handle_);    handle_ = nullptr; }
		size_ = 0;
	}

#else  // POSIX

	bool FileMapping::Map(const std::filesystem::path& path)
	{
		Unmap();

		int fd = ::open(path.c_str(), O_RDONLY);
		if (fd < 0) {
			oserror << "FileMapping: open() failed for "
				<< path.string() << " (errno=" << errno << ")" << std::endl;
			return false;
		}

		struct stat st {};
		if (::fstat(fd, &st) != 0) {
			oserror << "FileMapping: fstat() failed for "
				<< path.string() << " (errno=" << errno << ")" << std::endl;
			::close(fd);
			return false;
		}

		if (st.st_size == 0) {
			// Stash fd as handle_ so IsMapped() is true. mapHandle_
			// unused on POSIX -- left at nullptr.
			handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
			base_ = nullptr;
			size_ = 0;
			return true;
		}

		void* view = ::mmap(nullptr, static_cast<size_t>(st.st_size),
			PROT_READ, MAP_PRIVATE, fd, 0);
		if (view == MAP_FAILED) {
			oserror << "FileMapping: mmap() failed for "
				<< path.string() << " (errno=" << errno << ")" << std::endl;
			::close(fd);
			return false;
		}

		handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
		base_ = view;
		size_ = static_cast<uint64_t>(st.st_size);
		return true;
	}

	void FileMapping::Unmap()
	{
		if (base_) {
			::munmap(base_, static_cast<size_t>(size_));
			base_ = nullptr;
		}
		if (handle_) {
			int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
			::close(fd);
			handle_ = nullptr;
		}
		size_ = 0;
	}

#endif

} // namespace xmc