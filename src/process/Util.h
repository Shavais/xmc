#pragma once

#include <filesystem>

namespace fs = std::filesystem;

namespace process
{
	void RemoveFile(const std::string& pathname);

	std::string GetWin32ErrorString(DWORD errorCode); 

	template<typename T>
	T ReadPod(const std::vector<uint8_t>& buf, uint64_t& offset) {
		T data;
		if (offset + sizeof(T) <= buf.size()) {
			std::memcpy(&data, &buf[offset], sizeof(T));
			offset += sizeof(T);
		}
		return data;
	}

	std::string ReadString(const std::vector<uint8_t>& buf, uint64_t& offset); 

	// Helper to read POD types directly from the mmap'd pointer
	template<typename T>
	T ReadPodFromPtr(const uint8_t* base, uint64_t& offset) {
		T val;
		std::memcpy(&val, base + offset, sizeof(T));
		offset += sizeof(T);
		return val;
	}

	// Helper to read strings (stored as [uint32_t length][char...])
	std::string ReadStringFromPtr(const uint8_t* base, uint64_t& offset);

	template<typename T>
	void WritePod(std::vector<uint8_t>& buf, const T& data) {
		const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&data);
		buf.insert(buf.end(), ptr, ptr + sizeof(T));
	}

	void WriteString(std::vector<uint8_t>& buf, const std::string& s);
	std::vector<uint8_t> FastReadBinaryFile(const fs::path& path);
	void FastWriteBinaryFile(const fs::path& outPath, const std::vector<uint8_t>& buf);


}