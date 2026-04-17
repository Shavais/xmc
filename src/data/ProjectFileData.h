#pragma once

#include <unordered_map>
#include <string>
#include <variant>

namespace data
{
	using ConfigSection = std::unordered_map<std::string, std::variant<std::string, int>>;

	inline std::unordered_map<std::string, ConfigSection> RawProjectFile;
	inline ConfigSection ProjectFile;
	inline std::string DefaultConfig;

	inline string intdir = ".";
	inline string outdir = ".";
	inline string outfile = "";
}