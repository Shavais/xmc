// process/ParserFunctions.cpp
#include "pch.h"
#include "Parser.h"
#include "ParserInternal.h"

using namespace data;

namespace process
{
	// --- Internal Helper: Variadic Parameters ---
	// Grammar: variadic-parameter ::= IDENTIFIER "..."
	static bool ParseVariadicParameter(TextParser& p, const ParseContext& ctx, std::string& outName) {
		string_view ident = ParseIdentifier(p);
		if (ident.empty()) {
			EmitError(ctx, p.View(), "IDENTIFIER before '...'", (string)p.View().substr(0, 10));
			return false;
		}

		SkipWhitespaceAndComments(p);
		if (p.View().starts_with("...")) {
			p.Consume(3);
			outName = std::string(ident);
			return true;
		}

		string_view gotStr = p.View().empty() ? "EOF" : p.View().substr(0, 1);
		EmitError(ctx, p.View(), "...", string(gotStr));
		return false;
	}

	// --- Internal Helper: Function Parameters ---
	// Grammar: parameter ::= [ type-specifier ] IDENTIFIER [ UGLY-MASK ] [ "=" expression ]
	bool ParseParameterList(TextParser& p, const ParseContext& ctx, std::vector<uint32_t>& scopeStack, Arena& arena) {
		SkipWhitespaceAndComments(p);
		if (p.CheckFor(')')) return true; // Empty parameter list

		bool expectingParam = true;
		while (!p.Empty() && !p.CheckFor(')')) {
			if (!expectingParam) {
				EmitError(ctx, p.View(), ", or )", "unexpected token");
				return false;
			}

			SkipWhitespaceAndComments(p);

			// Check if we hit a trailing variadic marker
			string_view lookahead = p.View();
			if (lookahead.find("...") != string_view::npos) {
				// Naive peek: if the next slice contains '...', treat as variadic
				std::string varName;
				if (ParseVariadicParameter(p, ctx, varName)) {
					// Register the variadic symbol
					InternSymbol(varName.c_str(), scopeStack);

					SkipWhitespaceAndComments(p);
					if (!p.CheckFor(')')) {
						EmitError(ctx, p.View(), ") after variadic parameter", "extra tokens");
						return false;
					}
					break;
				}
				return false;
			}

			// Parse standard parameter
			string_view firstWord = ParseIdentifier(p);
			if (firstWord.empty()) {
				EmitError(ctx, p.View(), "refinement qualifier, type or parameter identifier", (string)p.View().substr(0, 10));
				return false;
			}

			SkipWhitespaceAndComments(p);
			string_view typeSpec = "";
			string_view actualIdent = "";

			if (IsRefinementQualifier(firstWord)) {
				// It was a refinement qualifier. The next word is the type, and the word after is the identifier.
				typeSpec = ParseIdentifier(p);
				SkipWhitespaceAndComments(p);
				actualIdent = ParseIdentifier(p);

				// FUTURE SEMANTIC HOOK:
				// Map the minimum constraint to `paramSymbol->minrmask`
			}
			else {
				string_view actualIdent;
				// If followed by another identifier, the first was a type. Otherwise, it was just "var" or implicit.
				string_view nextToken = p.View();
				if (!nextToken.empty() && (isalpha(static_cast<unsigned char>(nextToken[0])) || nextToken[0] == '_')) {
					actualIdent = ParseIdentifier(p);
				}
				else {
					actualIdent = firstWord;
				}
			}

			// Handle UGLY-MASK ceiling constraint on the parameter
			if (p.CheckFor('~')) {
				auto [mask, delimiter] = p.ReadUntil(" \t\r\n,)= ");
				if (mask.size() > 9) {
					EmitError(ctx, p.View(), "UGLY-MASK (1-8 chars)", "Oversized mask");
					return false;
				}
			}

			SkipWhitespaceAndComments(p);

			// Handle default values: [ "=" expression ]
			if (p.CheckFor('=')) {
				p.Consume(1);
				// Temporarily skip the expression up to the next delimiter
				p.ReadUntil(",)");
			}

			// Register the parameter symbol into the function's scope
			std::string paramStr(actualIdent);
			InternSymbol(paramStr.c_str(), scopeStack);

			SkipWhitespaceAndComments(p);

			// Handle commas and loop conditions
			if (p.CheckFor(',')) {
				p.Consume(1);
				expectingParam = true;
			}
			else {
				expectingParam = false;
			}
		}

		return true;
	}

	// --- Parser: Functions ---
	// Grammar: [ type-specifier ] IDENTIFIER "(" [ parameter-list ] ")" ( function-body | ";" )
	// Note: Added `Arena& arena` to the parameter list!
	bool ParseFunction(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx, Arena& arena, string_view inferredType) {
		string_view ident = ParseIdentifier(p);
		if (ident.empty()) {
			EmitError(ctx, p.View(), "IDENTIFIER for function name", (string)p.View().substr(0, 10));
			return false;
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('(')) {
			EmitError(ctx, p.View(), "(", "missing opening parenthesis on function");
			return false;
		}

		string_view paramContent = p.ReadBracedContent('(', ')');
		SkipWhitespaceAndComments(p);

		uint32_t funcScopeId = AllocScopeId();
		scopeStack.push_back(funcScopeId);

		TextParser paramParser(paramContent);
		bool paramSuccess = ParseParameterList(paramParser, ctx, scopeStack, arena);
		if (!paramSuccess) {
			scopeStack.pop_back();
			return false;
		}

		std::string funcName(ident);
		InternSymbol(funcName.c_str(), scopeStack);

		if (p.CheckFor(';')) {
			p.Consume(1);
			scopeStack.pop_back();
			return true;
		}

		if (!p.CheckFor('{')) {
			EmitError(ctx, p.View(), "{ or ;", "invalid function termination");
			scopeStack.pop_back();
			return false;
		}

		string_view bodyContent = p.ReadBracedContent('{', '}');

		// 1. Emit the logical parse node
		ParseTreeNode* funcNode = arena.Construct<ParseTreeNode>();
		funcNode->funcData = arena.Construct<FunctionNodeData>();
		funcNode->funcData->name = funcName;
		funcNode->funcData->isLeaf = true;

		// 2. Fix the pointer array mismatch: Create a pointer array of size 1 on the arena
		ParseTreeNode** parentArray = (ParseTreeNode**)arena.Allocate(sizeof(ParseTreeNode*));

		// 3. Assign funcNode directly to index 0 of the array!
		parentArray[0] = funcNode;

		// 4. Correctly assign the double pointer and count
		node->children = parentArray;
		node->childCount = 1;

		scopeStack.pop_back();
		return true;
	}


	// --- Internal Helper: Extern Parameter List ---
	// Grammar: extern-parameter ::= IDENTIFIER ":" type-specifier [ "->" IDENTIFIER ]
	bool ParseExternParameterList(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<InternedString>& symbols) {
		SkipWhitespaceAndComments(p);
		if (p.CheckFor(')')) return true; // Empty parameter list

		bool expectingParam = true;
		while (!p.Empty() && !p.CheckFor(')')) {
			if (!expectingParam) {
				EmitError(ctx, p.View(), ", or )", "unexpected token");
				return false;
			}

			SkipWhitespaceAndComments(p);
			string_view paramIdent = ParseIdentifier(p);
			if (paramIdent.empty()) {
				EmitError(ctx, p.View(), "parameter identifier", (string)p.View().substr(0, 10));
				return false;
			}

			SkipWhitespaceAndComments(p);
			if (!p.CheckFor(':')) {
				EmitError(ctx, p.View(), ":", "missing colon in extern parameter");
				return false;
			}
			p.Consume(1);

			// Parse type specifier
			string_view typeSpec = ParseIdentifier(p);
			if (typeSpec.empty()) {
				EmitError(ctx, p.View(), "type specifier", (string)p.View().substr(0, 10));
				return false;
			}

			SkipWhitespaceAndComments(p);

			// Check for register mapping: [ "->" IDENTIFIER ]
			if (p.View().starts_with("->")) {
				p.Consume(2);
				string_view regIdent = ParseIdentifier(p);
				if (regIdent.empty()) {
					EmitError(ctx, p.View(), "register identifier after ->", (string)p.View().substr(0, 10));
					return false;
				}
				// Save parameter name and mapped register
				symbols.push_back(InternString(std::string(paramIdent).c_str()));
				symbols.push_back(InternString(std::string(regIdent).c_str()));
			}
			else {
				// Stack passed parameter
				symbols.push_back(InternString(std::string(paramIdent).c_str()));
				symbols.push_back(InternString("STACK"));
			}

			SkipWhitespaceAndComments(p);
			if (p.CheckFor(',')) {
				p.Consume(1);
				expectingParam = true;
			}
			else {
				expectingParam = false;
			}
		}
		return true;
	}

	// --- Parser: Extern Declarations ---
	// Grammar: "extern" IDENTIFIER "." IDENTIFIER "(" [ extern-parameter-list ] ")" [ "->" type-specifier ":" IDENTIFIER ] ";"
	bool ParseExternDeclaration(TextParser& p, const ParseContext& ctx, Arena& arena, ParseTreeNode* node) {
		SkipWhitespaceAndComments(p);

		// 1. Parse Library Name
		string_view libIdent = ParseIdentifier(p);
		if (libIdent.empty()) {
			EmitError(ctx, p.View(), "library identifier", (string)p.View().substr(0, 10));
			return false;
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('.')) {
			EmitError(ctx, p.View(), ".", "missing dot separator in extern");
			return false;
		}
		p.Consume(1);

		// 2. Parse Function Name
		string_view funcIdent = ParseIdentifier(p);
		if (funcIdent.empty()) {
			EmitError(ctx, p.View(), "function identifier", (string)p.View().substr(0, 10));
			return false;
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('(')) {
			EmitError(ctx, p.View(), "(", "missing opening parenthesis on extern");
			return false;
		}
		p.Consume(1);

		std::vector<InternedString> symbolBuffer;
		symbolBuffer.push_back(InternString(std::string(libIdent).c_str()));
		symbolBuffer.push_back(InternString(std::string(funcIdent).c_str()));

		// 3. Parse Parameters
		if (!ParseExternParameterList(p, ctx, arena, symbolBuffer)) {
			return false;
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(')')) {
			EmitError(ctx, p.View(), ")", "missing closing parenthesis on extern");
			return false;
		}
		p.Consume(1);

		SkipWhitespaceAndComments(p);

		// 4. Handle Return Mapping: [ "->" type-specifier ":" IDENTIFIER ]
		if (p.View().starts_with("->")) {
			p.Consume(2);
			string_view typeSpec = ParseIdentifier(p);
			SkipWhitespaceAndComments(p);

			if (!p.CheckFor(':')) {
				EmitError(ctx, p.View(), ":", "missing colon in return register mapping");
				return false;
			}
			p.Consume(1);

			string_view regIdent = ParseIdentifier(p);
			if (regIdent.empty()) {
				EmitError(ctx, p.View(), "register identifier after ':'", (string)p.View().substr(0, 10));
				return false;
			}

			symbolBuffer.push_back(InternString("RETURN_REG"));
			symbolBuffer.push_back(InternString(std::string(regIdent).c_str()));
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(';')) {
			EmitError(ctx, p.View(), ";", "missing terminating semicolon on extern");
			return false;
		}
		p.Consume(1);

		// 5. Commit state to the node on the arena
		node->patchSymbolCount = static_cast<uint32_t>(symbolBuffer.size());
		if (node->patchSymbolCount > 0) {
			node->patchSymbols = arena.ConstructArray<InternedString>(node->patchSymbolCount);
			for (uint32_t i = 0; i < node->patchSymbolCount; ++i) {
				node->patchSymbols[i] = symbolBuffer[i];
			}
		}

		// FUTURE HOOK: Flag this parse tree node as an external bind site so the 
		// machine code emitter hooks it into standard library COFF calls!

		return true;
	}

}
	