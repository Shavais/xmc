#pragma once

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "data/ParserData.h"
#include "tool/Logger.h"
#include "tool/StringFunctions.h"
#include "tool/TextParser.h"

namespace process
{
	struct ParseContext {
		const std::string& originPath;
		std::ostream& osdebug;
		std::ostream& oserror;
		std::string_view fullSource;
	};

	void ParseModifiedSources();

	string_view ParseIdentifier(TextParser& p);
	void EmitError(const ParseContext& ctx, std::string_view currentView, const std::string& expected, const std::string& got);

	void ConsumeLength(TextParser& p, size_t length);

	void SkipWhitespaceAndComments(TextParser& p);

} // namespace process
