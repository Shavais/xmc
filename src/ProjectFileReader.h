#pragma once

#include <string>
#include <unordered_map>
#include <variant>

namespace xmc
{
	class ProjectFileReader
	{
	public:
		using ConfigSection = std::unordered_map<std::string, std::variant<std::string, int>>;

		// Parses [projectName].xmc and merges the active config section with "base".
		// If configName is empty, the section marked :default in the file is used.
		// Throws std::runtime_error if the file cannot be opened or no config can be resolved.
		static ProjectFileReader Load(const std::string& projectName, const std::string& configName);

		// Returns true if key exists in the effective project file and holds an int value.
		bool IsInt(const std::string& key) const;

		// Returns the int value for key, or defaultValue if absent or not an int.
		int GetInt(const std::string& key, int defaultValue = 0) const;

		// Returns the string value for key, or defaultValue if absent or not a string.
		std::string GetString(const std::string& key, std::string defaultValue = "") const;

		// Direct access to the merged effective project file (base + active config).
		const ConfigSection& GetProjectFile() const { return _projectFile; }

		// Paths and output file derived from the effective project file.
		const std::string& IntDir()  const { return _intdir; }
		const std::string& OutDir()  const { return _outdir; }
		const std::string& OutFile() const { return _outfile; }

		// Thread cap read from the "maxthreads" key (0 = unlimited, 1-255 = cap).
		// Defaults to 6 if the key is absent or invalid.
		uint8_t MaxThreads() const { return _maxThreads; }

	private:
		ProjectFileReader() = default;

		// Looks up key in section, falling back to "base" if not found there.
		// Returns nullptr if the key is absent in both.
		const std::variant<std::string, int>* FindRawValue(const std::string& section,
			const std::string& key) const;

		std::unordered_map<std::string, ConfigSection> _rawProjectFile;
		ConfigSection _projectFile;
		std::string   _defaultConfig;

		std::string _intdir = ".";
		std::string _outdir = ".";
		std::string _outfile;
		uint8_t     _maxThreads = 6;
	};

} // namespace xmc