// tool/FileMapping.h
#pragma once

#include <cstdint>
#include <filesystem>

namespace xmc
{
	// RAII wrapper around a read-only memory-mapped file.
	//
	// Move-only. The mapping is released either by Unmap() or by the
	// destructor; either way the underlying OS handle is closed and
	// the mapped pages become invalid.
	//
	// Map() returns false on failure and logs to oserror; the object
	// remains in the unmapped state. No exceptions are thrown.
	class FileMapping
	{
	public:
		FileMapping() = default;
		~FileMapping();

		FileMapping(FileMapping&& other) noexcept;
		FileMapping& operator=(FileMapping&& other) noexcept;

		FileMapping(const FileMapping&) = delete;
		FileMapping& operator=(const FileMapping&) = delete;

		// Maps the file at `path` read-only. Replaces any existing
		// mapping on this object. Empty files succeed and yield
		// a zero-size mapping with Base() == nullptr.
		bool Map(const std::filesystem::path& path);

		// Releases the mapping. Idempotent.
		void Unmap();

		bool        IsMapped() const { return handle_ != nullptr; }
		const void* Base()     const { return base_; }
		uint64_t    Size()     const { return size_; }

		// Convenience for byte-oriented consumers (xmo deserialization).
		const uint8_t* Bytes() const { return static_cast<const uint8_t*>(base_); }

	private:
		void* base_ = nullptr;  // mapped view base address
		uint64_t size_ = 0;        // file size in bytes
		void* handle_ = nullptr;  // OS file handle (HANDLE on Win32)
		void* mapHandle_ = nullptr;  // OS mapping handle (Win32 only; nullptr elsewhere)
	};

} // namespace xmc