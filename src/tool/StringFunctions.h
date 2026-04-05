// tool/StringFunctions.h

#pragma once

#include <algorithm>
#include <string_view>
#include <string>
#include <unordered_map> // Added: Fixes the C7568 error
#include <type_traits>   // Added: Fixes is_convertible_v

using std::string;
using std::string_view;
using std::unordered_map; // Added

string DateTimeStamp();
string GetErrorMessage(int errval);

constexpr string_view TrimTrailingNewlines(string_view s) {
	auto pos = s.find_last_not_of("\r\n");
	if (pos != s.npos)
	{
		s.remove_suffix(s.size() - pos - 1);
	}
	else
	{
		s.remove_suffix(s.size());
	}
	return s;
}

constexpr string_view ltrim(string_view s, const string_view tokens = " \t\n\r\f\v")
{
	s.remove_prefix(std::min(s.find_first_not_of(tokens), s.size()));
	return s;
}

constexpr string_view rtrim(string_view s, const string_view tokens = " \t\n\r\f\v")
{
	auto pos = s.find_last_not_of(tokens);
	if (pos != s.npos) {
		s.remove_suffix(s.size() - pos - 1);
	}
	else {
		s.remove_suffix(s.size());
	}
	return s;
}

constexpr string_view lrtrim(string_view s, const string_view tokens = " \t\n\r\f\v")
{
	return ltrim(rtrim(s, tokens), tokens);
}

// Helper to convert various types to string
template <typename T>
string to_str(const T& val)
{
	if constexpr (std::is_convertible_v<T, string>)
	{
		return static_cast<string>(val);
	}
	else
	{
		return std::to_string(val);
	}
}

template <typename... Args>
string ReplacePlaceholders(string text, Args... args)
{
	int index = 0;
	([&] {
		string placeholder = "[" + std::to_string(index++) + "]";
		string value = to_str(args);

		size_t pos = 0;
		while ((pos = text.find(placeholder, pos)) != string::npos)
		{
			text.replace(pos, placeholder.length(), value);
			pos += value.length();
		}
		}(),
			...);

	return text;
}

inline string ReplacePlaceholders(string text, unordered_map<string, string> replacements)
{
	for (const auto& [key, value] : replacements)
	{
		size_t pos = 0;
		while ((pos = text.find(key, pos)) != string::npos)
		{
			text.replace(pos, key.length(), value);
			pos += value.length();
		}
	}
	return text;
}

inline string Lowercase(string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

inline string Lowercase(string_view sv)
{
	return Lowercase(string(sv));
}
