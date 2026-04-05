#pragma once

namespace process
{

	void ParseProjectFile();

	bool IsInt(const std::string& key);
	int GetInt(const std::string& key, int defaultValue = 0);
	std::string GetString(const std::string& key, std::string defaultValue);

}