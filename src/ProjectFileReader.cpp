#include "pch/pch.h"
#include "ProjectFileReader.h"

#include <fstream>

#include "tool/Logger.h"
#include "tool/TextParser.h"

namespace xmc
{
	// -------------------------------------------------------------------------
	// Load  (static factory)
	// -------------------------------------------------------------------------

	ProjectFileReader ProjectFileReader::Load(const std::string& projectName, const std::string& configName)
	{
		std::string filename = projectName + ".xmc";
		std::ifstream file(filename, std::ios::binary | std::ios::ate);

		if (!file.is_open()) {
			oserror << "Could not open project file: " << filename << std::endl;
			throw std::runtime_error("file not found");
		}

		// Read the entire file into a buffer for the TextParser.
		auto size = file.tellg();
		auto buffer = std::make_unique<char[]>(size);
		file.seekg(0);
		file.read(buffer.get(), size);

		ProjectFileReader reader;
		TextParser parser(std::string_view(buffer.get(), static_cast<uint64_t>(size)));

		std::string currentSection = "base"; // Fallback for settings before any header.

		while (!parser.Empty()) {
			parser.Skip(" \t\r\n");
			if (parser.Empty()) break;

			// Comments
			if (parser.CheckFor('#')) {
				parser.ReadUntil("\n", true);
				continue;
			}

			// Section headers: [name] or [name:trait]
			if (parser.CheckFor('[')) {
				auto header = parser.ReadBracedContent('[', ']');
				TextParser headerParser(header);
				auto [name, delimiter] = headerParser.ReadUntil(":", true);
				currentSection = Lowercase(lrtrim(name));
				if (delimiter == ':') {
					std::string trait = Lowercase(lrtrim(headerParser.View()));
					if (trait == "default") {
						reader._defaultConfig = currentSection;
					}
				}
				continue;
			}

			// Key = Value pairs
			auto [keyView, valueView] = parser.ReadNameValuePair();
			if (!keyView.empty()) {
				std::string key = Lowercase(lrtrim(keyView));
				std::string value = std::string(lrtrim(valueView));

				// Store as int if the entire value parses as one; otherwise store as string.
				int i = 0;
				auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), i);

				if (ec == std::errc{} && ptr == value.data() + value.size()) {
					reader._rawProjectFile[currentSection][key] = i;
				}
				else {
					reader._rawProjectFile[currentSection][key] = value;
				}
			}
		}

		// Resolve active config: prefer the caller-supplied name, then the :default marker.
		std::string activeConfig = configName.empty() ? reader._defaultConfig : configName;

		if (activeConfig.empty()) {
			oserror << "No configuration specified and no default found in .xmc" << std::endl;
			throw std::runtime_error("Missing configuration");
		}

		// Merge: start with "base", then overlay the active config section.
		auto baseIt = reader._rawProjectFile.find("base");
		if (baseIt != reader._rawProjectFile.end()) {
			for (const auto& [key, value] : baseIt->second)
				reader._projectFile[key] = value;
		}

		if (reader._rawProjectFile.contains(activeConfig)) {
			for (const auto& [key, value] : reader._rawProjectFile[activeConfig])
				reader._projectFile[key] = value;
		}

		// Apply maxconcurrentFiles if present and valid.
		if (reader._projectFile.contains("maxconcurrentfiles")) {
			const auto& val = reader._projectFile["maxconcurrentfiles"];

			if (const int* i = std::get_if<int>(&val)) {
				if (*i >= 0 && *i <= 255)
					reader._maxConcurrentFiles = static_cast<uint8_t>(*i);
				else
					oserror << "Error: 'maxconcurrentfiles' value " << *i
					<< " is out of range (0-255)." << std::endl;
			}
			else if (const std::string* s = std::get_if<std::string>(&val)) {
				oserror << "Error: 'maxconcurrentfiles' value (" << *s
					<< ") is not a valid integer." << std::endl;
			}
		}

		reader._intdir = reader.GetString("IntPath", ".");
		reader._outdir = reader.GetString("OutPath", ".");
		reader._outfile = projectName;
		if (!reader._outfile.ends_with(".obj")) reader._outfile += ".obj";

		return reader;
	}

	// -------------------------------------------------------------------------
	// Accessors
	// -------------------------------------------------------------------------

	bool ProjectFileReader::IsInt(const std::string& key) const
	{
		auto it = _projectFile.find(key);
		if (it == _projectFile.end()) return false;
		return std::holds_alternative<int>(it->second);
	}

	std::string ProjectFileReader::GetString(const std::string& key, std::string defaultValue) const
	{
		auto it = _projectFile.find(key);
		if (it != _projectFile.end()) {
			if (const auto* s = std::get_if<std::string>(&it->second)) return *s;
		}
		return defaultValue;
	}

	int ProjectFileReader::GetInt(const std::string& key, int defaultValue) const
	{
		auto it = _projectFile.find(key);
		if (it != _projectFile.end()) {
			if (const auto* i = std::get_if<int>(&it->second)) return *i;
		}
		return defaultValue;
	}

	// -------------------------------------------------------------------------
	// Private helpers
	// -------------------------------------------------------------------------

	const std::variant<std::string, int>*
		ProjectFileReader::FindRawValue(const std::string& section, const std::string& key) const
	{
		auto secIt = _rawProjectFile.find(section);
		if (secIt != _rawProjectFile.end()) {
			auto keyIt = secIt->second.find(key);
			if (keyIt != secIt->second.end()) return &keyIt->second;
		}

		// Fall back to "base" if the key wasn't in the requested section.
		if (section != "base") {
			auto baseIt = _rawProjectFile.find("base");
			if (baseIt != _rawProjectFile.end()) {
				auto keyIt = baseIt->second.find(key);
				if (keyIt != baseIt->second.end()) return &keyIt->second;
			}
		}

		return nullptr;
	}

} // namespace xmc