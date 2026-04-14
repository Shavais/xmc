#include "pch.h"
#include "ProjectFile.h"

#include <sstream>
#include <fstream>

#include "data/CmdLineData.h"
#include "data/GlobalData.h"
#include "data/ProjectFileData.h"

#include "tool/Logger.h"
#include "tool/TextParser.h"

namespace process
{
	void ParseProjectFile() {
		std::string filename = data::CmdLineArgs.ProjectName + ".xmc";
		std::ifstream file(filename, std::ios::binary | std::ios::ate);

		if (!file.is_open()) {
			oserror << "Could not open project file: " << filename << std::endl;
			throw std::runtime_error("file not found");
		}

		// Read the whole file into a string buffer the TextParser
		auto size = file.tellg();
		auto buffer = std::make_unique<char[]>(size);
		file.seekg(0);
		file.read(buffer.get(), size);

		TextParser parser(std::string_view(buffer.get(), static_cast<uint64_t>(size)));

		std::string currentSection = "base"; // Default to base if settings appear before a header

		while (!parser.Empty()) {
			parser.Skip(" \t\r\n");
			if (parser.Empty()) break;

			// Handle Comments
			if (parser.CheckFor('#')) {
				parser.ReadUntil("\n", true);
				continue;
			}

			// Handle Section Headers: [name:type]
			if (parser.CheckFor('[')) {
				auto header = parser.ReadBracedContent('[', ']');
				TextParser headerParser(header);
				auto [name, delimiter] = headerParser.ReadUntil(":", true);
				currentSection = Lowercase(lrtrim(name));
				if (delimiter == ':') {
					std::string trait = Lowercase(lrtrim(headerParser.View()));
					if (trait == "default") {
						data::DefaultConfig = currentSection;
					}
				}
				continue;
			}

			// Handle Key = Value pairs
			auto [keyView, valueView] = parser.ReadNameValuePair();
			if (!keyView.empty()) {
				std::string key = Lowercase(lrtrim(keyView));
				std::string value = std::string(lrtrim(valueView));

				// if it looks like a number store as int, otherwise store as string
				int i = 0;
				auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), i);

				// Check if the entire string was consumed (ptr == end) 
				// and if no error occurred (ec == success)
				if (ec == std::errc{} && ptr == value.data() + value.size()) {
					data::RawProjectFile[currentSection][key] = i;
				}
				else {
					data::RawProjectFile[currentSection][key] = value;
				}
			}
		}

		// Post-parsing: If no config was specified on cmd line, use default
		if (data::CmdLineArgs.ConfigName.empty()) {
			data::CmdLineArgs.ConfigName = data::DefaultConfig;
		}

		if (data::CmdLineArgs.ConfigName.empty()) {
			oserror << "No configuration specified and no default found in .xmc" << std::endl;
			throw std::runtime_error("Missing configuration");
		}

		// write out config file contents:
		//osdebug << "--- SVL Project Data Dump ---" << std::endl;
		//for (auto const& [section, keys] : data::RawProjectFile) {
		//	osdebug << "[" << section << "]" << std::endl;
		//	for (auto const& [key, value] : keys) {
		//		osdebug << "  " << key << " = ";

		//		// Peek at the variant type
		//		if (auto pInt = std::get_if<int>(&value)) {
		//			osdebug << *pInt << " (int)";
		//		}
		//		else if (auto pStr = std::get_if<std::string>(&value)) {
		//			osdebug << "\"" << *pStr << "\" (string)";
		//		}
		//		osdebug << std::endl;
		//	}
		//}
		//osdebug << "Active Config: " << data::CmdLineArgs.ConfigName << std::endl;
		//osdebug << "-----------------------------" << std::endl;
		//

		// merge the active section with the base section from the raw project file into the effective project file
		auto& section = data::RawProjectFile.find("base")->second;

		for (const auto& [key, value]: section) data::ProjectFile[key] = value;
		if (data::RawProjectFile.contains(data::CmdLineArgs.ConfigName))
		{
			section = data::RawProjectFile[data::CmdLineArgs.ConfigName];
			for (const auto& [key, value] : section) data::ProjectFile[key] = value;
		}

		if (data::ProjectFile.contains("maxthreads"))
		{
			auto& val = data::ProjectFile["maxthreads"];

			if (const int* i = std::get_if<int>(&val)) {
				if (*i >= 0 && *i <= 255) data::MaxThreads = static_cast<uint8_t>(*i);
				else oserror << "Error: 'maxthreads' value " << *i << " is out of range (0-255)." << std::endl;
			}
			else if (const std::string* s = std::get_if<std::string>(&val)) {
				oserror << "Error: 'maxthreads' value (" << *s << ") is not a valid integer." << std::endl;
			}
		}

		// write out effective ProjectFile contents:
		//osdebug << "\nmerged ProjectFile: " << std::endl;
		//osdebug << "-----------------------------" << std::endl;
		//for (auto const& [key, value] : data::ProjectFile) {
		//	osdebug << "  " << key << " = ";

		//	// Peek at the variant type
		//	if (auto pInt = std::get_if<int>(&value)) {
		//		osdebug << *pInt << " (int)";
		//	}
		//	else if (auto pStr = std::get_if<std::string>(&value)) {
		//		osdebug << "\"" << *pStr << "\" (string)";
		//	}
		//	osdebug << std::endl;
		//}

	}

	bool IsInt(const std::string& key) {

		auto projFileIt = data::ProjectFile.find(key);
		if (projFileIt == data::ProjectFile.end()) return false;

		return std::holds_alternative<int>(projFileIt->second);
	}

	const std::variant<std::string, int>* FindRawProjectValue(const std::string& section, const std::string& key) {
		auto secIt = data::RawProjectFile.find(section);

		if (secIt != data::RawProjectFile.end()) {
			auto keyIt = secIt->second.find(key);
			if (keyIt != secIt->second.end()) {
				return &keyIt->second;
			}
		}

		if (section != "base") {
			auto baseIt = data::RawProjectFile.find("base");
			if (baseIt != data::RawProjectFile.end()) {
				auto keyIt = baseIt->second.find(key);
				if (keyIt != baseIt->second.end()) {
					return &keyIt->second;
				}
			}
		}

		return nullptr;
	}

	const std::variant<std::string, int>* FindRawValue(const std::string& section, const std::string& key) {
		auto secIt = data::RawProjectFile.find(section);

		if (secIt != data::RawProjectFile.end()) {
			auto keyIt = secIt->second.find(key);
			if (keyIt != secIt->second.end()) {
				return &keyIt->second;
			}
		}

		if (section != "base") {
			auto baseIt = data::RawProjectFile.find("base");
			if (baseIt != data::RawProjectFile.end()) {
				auto keyIt = baseIt->second.find(key);
				if (keyIt != baseIt->second.end()) {
					return &keyIt->second;
				}
			}
		}

		return nullptr;
	}

	std::string GetString(const std::string& key, std::string defaultValue) {
		auto projFileIt = data::ProjectFile.find(key);
		if (projFileIt != data::ProjectFile.end())
		{
			if (auto s = std::get_if<std::string>(&projFileIt->second)) return *s;
		}
		return defaultValue;
	}

	int GetInt(const std::string& key, int defaultValue) {
		auto projFileIt = data::ProjectFile.find(key);
		if (projFileIt != data::ProjectFile.end())
		{
			if (auto s = std::get_if<int>(&projFileIt->second)) return *s;
		}
		return defaultValue;
	}
}
