#pragma once

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include "StringFunctions.h"

using std::array;
using std::min;
using std::pair;
using std::string_view;
using std::vector;

class TextParser
{
private:
	string_view view_;

public:
	TextParser(string_view text)
	{
		view_ = text;
	}

	inline string_view View() { return view_; }

	inline void Skip(const char token)
	{
		view_.remove_prefix(min(view_.find_first_not_of(token), view_.size()));
	}

	inline void Skip(const string& tokens)
	{
		view_.remove_prefix(min(view_.find_first_not_of(tokens), view_.size()));
	}

	inline bool Empty()
	{
		return view_.empty();
	}

	inline vector<string_view> Split(char token) const
	{
		vector<string_view> result;

		uint64_t count = std::count(view_.begin(), view_.end(), token);
		result.reserve(count + 1);

		uint64_t start = 0;
		uint64_t end = view_.find(token);

		while (end != view_.npos) {
			result.emplace_back(view_.substr(start, end - start));
			start = end + 1;
			end = view_.find(token, start);
		}

		result.emplace_back(view_.substr(start));

		return result;
	}

	inline pair<string_view, string_view> ReadNameValuePair()
	{
		Skip(" \t\r\n");
		if (Empty()) return { "", "" };

		uint64_t eqPos = view_.find('=');
		if (eqPos == view_.npos) return { "", "" };

		string_view name = lrtrim(view_.substr(0, eqPos));
		view_.remove_prefix(eqPos + 1);
		Skip(" \t\r\n"); // Skip space after '='

		string_view value;
		// Check if the value is a bracketed list: [ path1, path2 ]
		if (!Empty() && view_[0] == '[') {
			uint64_t endBracket = view_.find(']');
			if (endBracket != view_.npos) {
				// Keep the brackets in the value for later processing
				value = view_.substr(0, endBracket + 1);
				view_.remove_prefix(endBracket + 1);
			}
			else {
				// Error case: missing closing bracket
				value = view_;
				view_ = {};
			}
		}
		else {
			auto [rawValue, delimiter] = ReadUntil(",\n\r#");
			value = rawValue;

			// If we stopped at a comment, we need to clear the rest of that line
			if (delimiter == '#') {
				ReadUntil("\n\r");
			}
		}

		return { name, lrtrim(value, " \t\r\n\"") };
	}

	inline unordered_map<string, string> ReadNameValuePairs()
	{
		unordered_map<string, string> result;
		pair<string_view, string_view> nvp;
		while (true)
		{
			nvp = ReadNameValuePair();
			if (nvp.first == "") break;
			result[string(nvp.first)] = nvp.second;
		}
		return result;
	}

	inline string_view ReadUntil(const char token)
	{
		string_view result;
		uint64_t end = view_.find(token);
		if (end == view_.npos)
		{
			result = view_;
			view_ = string_view();
		}
		else
		{
			result = view_.substr(0, end);
			view_ = view_.substr(end);
		}
		return result;
	}

	inline pair<string_view, char> ReadUntil(const string& tokens, bool skipDelimiter = false)
	{
		uint64_t end = view_.find_first_of(tokens);
		string_view result;
		char token;

		if (end == view_.npos)
		{
			result = view_;
			token = -1;
			view_.remove_prefix(view_.size());
		}
		else
		{
			result = view_.substr(0, end);
			token = view_[end];
			view_.remove_prefix(skipDelimiter ? end + 1 : end);
		}
		return { result, token };
	}

	inline bool CheckFor(const char token)
	{
		return !view_.empty() && view_[0] == token;
	}

	// Reads a value 
	template <typename T>
	inline T ReadValue(T defaultValue = T()) {
		Skip(" \t\r\n");
		T result;
		auto [ptr, ec] = std::from_chars(view_.data(), view_.data() + view_.size(), result);
		if (ec == std::errc()) {
			view_.remove_prefix(ptr - view_.data());
			return result;
		}
		return defaultValue;
	}

	// Takes a string like "{1, 2, 3}" or "{3.14, 0.29, 3, 9.6}" and attempts to return a vector<TargetType>.		
	template <typename TargetType>
	static std::vector<TargetType> GetVector(const std::string& s, TargetType defaultValue = TargetType()) {
		std::vector<TargetType> result;
		TextParser p(s);
		p.Skip(" \t\r\n{");
		while (!p.Empty() && !p.CheckFor('}')) {
			result.push_back(p.ReadValue<TargetType>(defaultValue));
			p.Skip(" \t\r\n,");
		}
		return result;
	}

	// Takes a string like "{1, 2, 3}" or "{3.14, 0.29, 3, 9.6}" and returns an array<TargetType, N>.
	// If fewer than N values are represented in the string, default values are used.
	template <typename TargetType, uint64_t N>
	static array<TargetType, N> GetFixedArray(std::string_view s, TargetType defaultValue = TargetType()) {
		std::array<TargetType, N> result;
		result.fill(defaultValue);

		TextParser p(s);
		p.Skip(" \t\r\n{");

		for (uint64_t i = 0; i < N && !p.Empty() && !p.CheckFor('}'); ++i) {
			result[i] = p.ReadValue<TargetType>(defaultValue);
			p.Skip(" \t\r\n,");
		}
		return result;
	}

	// Takes a string like "{900, 600}" and populates a given struct-like object that has 2 values.
	// (usefull for vec4's and rgba colors, for example)
	template <typename TargetType, typename StructType>
	static void FillStruct2(StructType& out, string_view s, TargetType defaultValue = TargetType()) {
		auto vals = TextParser::GetFixedArray<TargetType, 2>(s, defaultValue);
		out = { vals[0], vals[1] };
	}

	// Takes a string like "{0.6, 0.3, 0.9, 1.0}" and populates a given struct-like object that has 4 values.
	// (usefull for vec4's and rgba colors, for example)
	template <typename TargetType, typename StructType>
	static void FillStruct4(StructType& out, string_view s, TargetType defaultValue = TargetType()) {
		auto vals = TextParser::GetFixedArray<TargetType, 4>(s, defaultValue);
		out = { vals[0], vals[1], vals[2], vals[3] };
	}

	// Fills the given struct of 2 TargetType values from a string_view retrieved from the given map if the given key is found in it.
	template <typename TargetType, typename StructType, typename Map, typename Key>
	static void UpdateStruct2FromMap(StructType& out, Map& map, const Key& key)
	{
		auto itr = map.find(key);
		if (itr != map.end())
		{
			FillStruct2<TargetType>(out, itr->second);
		}
	}

	// Fills the given struct of 4 TargetType values from a string_view retrieved from the given map if the given key is found in it.
	template <typename TargetType, typename StructType, typename Map, typename Key>
	static void UpdateStruct4FromMap(StructType& out, Map& map, const Key& key)
	{
		auto itr = map.find(key);
		if (itr != map.end())
		{
			FillStruct4<TargetType>(out, itr->second);
		}
	}

	// returns a stringview containing the content of a curly brace pair which is at the current parser position.
	// advances the parser view's start pointer just past the ending brace.
	string_view ReadBracedContent(char startbrace = '{', char endbrace = '}') {
		uint64_t start = view_.find(startbrace);
		if (start == string_view::npos) return {};

		start += 1;
		uint64_t end = start;
		int bracecount = 1;
		char search_set_array[] = { startbrace, endbrace };
		string_view search_set(search_set_array, 2);
		while (bracecount > 0)
		{
			end = view_.find_first_of(search_set, end);
			if (end == string_view::npos) break;

			if (view_[end] == startbrace) bracecount++;
			else bracecount--;

			end++;
		}
		string_view result;
		if (bracecount == 0)
		{
			result = view_.substr(start, (end - 1) - start);
			view_.remove_prefix(end);
		}
		else
		{
			result = view_.substr(start + 1);
			view_.remove_prefix(view_.size());
		}

		return result;
	}

};
