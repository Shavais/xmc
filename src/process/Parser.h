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

	// Interns a raw C string into SymbolArena.
	// Computes and caches hash and length. Returns an InternedString
	// whose lifetime is tied to SymbolArena.
	data::InternedString InternString(const char* str);

	// Registers a symbol definition encountered during parsing.
	// namespacePath is the full scope stack at the point of declaration,
	// outermost (file) scope ID first.
	data::Symbol* InternSymbol(const char* tempName, std::vector<uint32_t> namespacePath);

	// Resolves a symbol reference from a given call site.
	// callerPath is the full namespace path of the reference site, outermost first.
	// Returns the best-matching symbol, or nullptr if not found or ambiguous.
	data::Symbol* ResolveSymbol(const char* name, const std::vector<uint32_t>& callerPath);

	void ParseModifiedSources();

	string_view ParseIdentifier(TextParser& p);
	void EmitError(const ParseContext& ctx, std::string_view currentView, const std::string& expected, const std::string& got);

	void ConsumeLength(TextParser& p, size_t length);

	void SkipWhitespaceAndComments(TextParser& p);

} // namespace process
