#include "pch.h"
#include "Parser.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

#include "data/GlobalData.h"
#include "data/ParserData.h"
#include "data/SourceFileData.h"

#include "process/ParserInternal.h"

#include "tool/BS_thread_pool.hpp"
#include "tool/Logger.h"
#include "tool/TextParser.h"
#include "tool/StringFunctions.h"

using namespace data;

namespace process
{

	// Context holder passed down stack depths


	// Counts line breaks starting from the origin buffer up to the current parser cursor
	int GetLineNumber(const ParseContext& ctx, std::string_view currentView) {
		if (currentView.empty()) return 1;

		const char* start = ctx.fullSource.data();
		const char* current = currentView.data();

		int line = 1;
		while (start < current) {
			if (*start == '\n') line++;
			start++;
		}
		return line;
	}

	// Dynamic error handler mapping to your requested output scheme
	void EmitError(const ParseContext& ctx, std::string_view currentView, const std::string& expected, const std::string& got) {
		int line = GetLineNumber(ctx, currentView);
		ctx.oserror << "syntax error on line " << line
			<< " of " << ctx.originPath
			<< ": expected one of [" << expected
			<< "] got [" << got << "]\n";
	}

	string_view ParseIdentifier(TextParser& p) {
		SkipWhitespaceAndComments(p);
		string_view v = p.View();

		// Check if empty or if the first character is invalid (must be a-z, A-Z, or _)
		if (v.empty() || (!isalpha(static_cast<unsigned char>(v[0])) && v[0] != '_')) {
			return {};
		}

		size_t len = 1;
		while (len < v.size() && (isalnum(static_cast<unsigned char>(v[len])) || v[len] == '_')) {
			len++;
		}

		string_view ident = v.substr(0, len);
		ConsumeLength(p, len);
		return ident;
	}

	// Validates syntax and parses XMC instructions.
	// sourceCode is passed directly from FileInfo::sourceText.
	// originPath serves as the label name during syntax tracebacks.
	bool ParseXmcSource(std::string_view sourceCode, const std::string& originPath, std::ostream& osdebug, Xmo** outXmo) {
		if (!outXmo) return false;

		Xmo* newXmo = new Xmo();
		newXmo->name = originPath;
		newXmo->dirty_ = true;

		// Allocate root node on the Xmo's internal Arena!
		newXmo->parseTree = newXmo->arena.Construct<ParseTreeNode>();

		TextParser parser(sourceCode);
		uint32_t fileScopeId = AllocScopeId();
		std::vector<uint32_t> scopeStack = { fileScopeId };

		ParseContext ctx{ originPath, osdebug, oserror, sourceCode };

		bool success = true;
		std::vector<ParseTreeNode*> topLevelNodes;

		// Perform main parsing loop
		while (!parser.Empty() && success) {
			ParseTreeNode* nextNode = newXmo->arena.Construct<ParseTreeNode>();
			success = ParseTopLevelDeclaration(parser, scopeStack, nextNode, ctx, newXmo->arena);
			if (success) {
				topLevelNodes.push_back(nextNode);
			}
			parser.Skip(" \t\r\n");
		}

		if (success) {
			// Tie the top-level nodes array straight onto the Xmo Arena
			uint32_t count = static_cast<uint32_t>(topLevelNodes.size());
			ParseTreeNode** arenaArray = (ParseTreeNode**)newXmo->arena.Allocate(count * sizeof(ParseTreeNode*));

			for (uint32_t i = 0; i < count; ++i) {
				arenaArray[i] = topLevelNodes[i];
			}

			newXmo->parseTree->children = arenaArray;
			newXmo->parseTree->childCount = count;

			Xmos.push_back(newXmo);
			*outXmo = newXmo;
			return true;
		}
		else {
			delete newXmo; // Frees everything seamlessly on failure
			*outXmo = nullptr;
			return false;
		}
	}

	void ParseModifiedSources()
	{
		auto xmos = data::Xmos;
		uint8_t maxThreads = data::MaxThreads;
		BS::thread_pool pool(maxThreads);
		
		// Iterate over the modified source files 
		for (const std::string& name : data::ModifiedSources) {

			auto it = data::SourceFiles.find(name);
			if (it == data::SourceFiles.end()) continue;

			const auto& fileInfo = it->second;
			if (fileInfo.sourceText.empty()) continue;

			// Detach a task for every file
			pool.detach_task(
				[name, &fileInfo]() {
					try {
						data::Xmo* outXmo = nullptr;

						// Invoking the API per-thread
						bool result = process::ParseXmcSource(
							fileInfo.sourceText,
							name,
							osdebug, // Ensure your custom osdebug stream is thread-safe!
							&outXmo
						);

						// Log or track the result
						if (result && outXmo) {
							osdebug << "Successfully parsed: " << name << " (" << fileInfo.fullPath.filename().string() << ")\n";
						}
						else {
							osdebug << "Failed to parse: " << name << "\n";
						}
					}
					catch (...) {
					}
				}
			);
			pool.wait();
		}
	}

	// --- Internal Helper: Skips Whitespace and BOTH types of comments ---
	void SkipWhitespaceAndComments(TextParser& p) {
		while (!p.Empty()) {
			// 1. Clear out standard whitespace first
			p.Skip(" \t\r\n\f\v");

			if (p.Empty()) break;

			string_view view = p.View();

			// 2. Handle Line Comments: // ...
			if (view.starts_with("//")) {
				p.ReadUntil('\n'); // Consume everything until the newline
				p.Skip("\n\r");    // Consume the newline itself
				continue;          // Loop again to catch more whitespace or comments
			}

			// 3. Handle Block Comments: /* ... */
			if (view.starts_with("/*")) {
				p.Consume(2); // Jump past "/*"

				// Read until we find the closing sequence
				while (!p.Empty()) {
					p.ReadUntil('*'); // Seek to the next star
					if (p.Empty()) break;

					p.Consume(1); // Consume the '*'
					if (p.CheckFor('/')) {
						p.Consume(1); // Consume the '/'
						break; // Block comment closed!
					}
				}
				continue; // Loop again
			}

			// If it's not whitespace and not a comment starter, we are done skipping!
			break;
		}
	}
}
