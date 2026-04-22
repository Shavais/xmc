// process/ParserDeclarations.cpp
#include "pch.h"
#include "Parser.h"
#include "ParserInternal.h"

namespace process
{
	// --- Parser: Namespace Declarations ---
	// Grammar: "namespace" IDENTIFIER "{" { top-level-declaration } "}"
	bool ParseNamespace(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx, Arena& arena) {
		string_view name = ParseIdentifier(p);
		if (name.empty()) {
			EmitError(ctx, p.View(), "IDENTIFIER after 'namespace'", (string)p.View().substr(0, 10));
			return false;
		}

		SkipWhitespaceAndComments(p);

		// 1. Consume the opening brace as we did in the working version
		if (!p.CheckFor('{')) {
			string_view gotStr = p.View().empty() ? "EOF" : p.View().substr(0, 1);
			EmitError(ctx, p.View(), "{", string(gotStr));
			return false;
		}
		// p.Consume(1); // Consume the '{'

		// 2. Read the interior content
		string_view blockContent = p.ReadBracedContent('{', '}');
		if (process::BraceMatchError) {
			EmitError(ctx, p.View(), "}", "unbalanced or missing closing brace in namespace");
			process::BraceMatchError = false; // reset
			return false;
		}

		// 3. Allocate child node representing this namespace boundary on the file arena
		ParseTreeNode* nsChild = arena.Construct<ParseTreeNode>();

		uint32_t nsScopeId = AllocScopeId();
		scopeStack.push_back(nsScopeId);

		std::vector<ParseTreeNode*> collectedChildren;

		TextParser blockParser(blockContent);
		bool success = true;

		SkipWhitespaceAndComments(blockParser);
		while (!blockParser.Empty() && success) {
			ParseTreeNode* nextNode = arena.Construct<ParseTreeNode>();

			// Recursively parse top-level items inside the namespace
			success = ParseTopLevelDeclaration(blockParser, scopeStack, nextNode, ctx, arena);

			if (success) {
				collectedChildren.push_back(nextNode);
			}
			SkipWhitespaceAndComments(blockParser);
		}

		scopeStack.pop_back();

		if (!success) return false;

		// 4. Map the temporary vector solidly onto fixed arena double-pointers
		if (!collectedChildren.empty()) {
			uint32_t count = static_cast<uint32_t>(collectedChildren.size());

			ParseTreeNode** arenaArray = (ParseTreeNode**)arena.Allocate(count * sizeof(ParseTreeNode*));
			for (uint32_t i = 0; i < count; ++i) {
				arenaArray[i] = collectedChildren[i];
			}

			nsChild->children = arenaArray;
			nsChild->childCount = count;

			ParseTreeNode** parentArray = (ParseTreeNode**)arena.Allocate(sizeof(ParseTreeNode*));
			parentArray[0] = nsChild;

			node->children = parentArray;
			node->childCount = 1;
			return true;
		}

		return false;
	}

	// --- Parser: Struct Declarations ---
	// Grammar: "struct" IDENTIFIER "{" { variable-declaration } "}"
	bool ParseStruct(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx, Arena& arena) {
		SkipWhitespaceAndComments(p);

		// 1. Parse the struct's name
		string_view structIdent = ParseIdentifier(p);
		if (structIdent.empty()) {
			EmitError(ctx, p.View(), "IDENTIFIER after 'struct'", (string)p.View().substr(0, 10));
			return false;
		}

		// Register the struct symbol in the file scope
		std::string structName(structIdent);
		InternSymbol(structName.c_str(), scopeStack);

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('{')) {
			string_view gotStr = p.View().empty() ? "EOF" : p.View().substr(0, 1);
			EmitError(ctx, p.View(), "{", string(gotStr));
			return false;
		}
		p.Consume(1); // consume the '{'

		// 2. Isolate the internal scope for struct fields
		uint32_t structScopeId = AllocScopeId();
		scopeStack.push_back(structScopeId);

		std::vector<ParseTreeNode*> structMembers;
		bool success = true;

		SkipWhitespaceAndComments(p);

		// 3. Parse sequential field variables until we hit the closing '}'
		while (!p.Empty() && !p.CheckFor('}')) {
			string_view firstWord = ParseIdentifier(p);
			SkipWhitespaceAndComments(p);

			// We delegate the hard variable parsing work straight to ParseVariableDeclaration!
			ParseTreeNode* memberVar = nullptr;
			if (firstWord == "var") {
				memberVar = ParseVariableDeclaration(p, scopeStack, ctx, arena, true);
			}
			else {
				// Treat as a typed member (e.g., `Point pos;`)
				memberVar = ParseVariableDeclaration(p, scopeStack, ctx, arena, false, firstWord);
			}

			if (!memberVar) {
				success = false;
				break;
			}
			structMembers.push_back(memberVar);
			SkipWhitespaceAndComments(p);
		}

		scopeStack.pop_back();

		if (!success) return false;

		if (!p.CheckFor('}')) {
			EmitError(ctx, p.View(), "}", "missing closing brace on struct");
			return false;
		}
		p.Consume(1); // consume the '}'

		// 4. Map field nodes solid onto the Arena's fixed array
		node->childCount = static_cast<uint32_t>(structMembers.size());
		if (node->childCount > 0) {
			node->children = arena.ConstructArray<ParseTreeNode*>(node->childCount);
			for (uint32_t i = 0; i < node->childCount; ++i) {
				node->children[i] = structMembers[i];
			}
		}

		// Save the name into the node for future symbol lookups
		node->patchSymbolCount = 1;
		node->patchSymbols = arena.ConstructArray<InternedString>(1);
		node->patchSymbols[0] = InternString(structName.c_str());

		return true;
	}
	// --- Internal Helper: Base Class List ---
	// Grammar: qualified-name { "," qualified-name }
	static bool ParseBaseClassList(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<InternedString>& baseClasses) {
		SkipWhitespaceAndComments(p);

		bool expectingBase = true;
		while (!p.Empty() && !p.CheckFor('{')) {
			if (!expectingBase) {
				EmitError(ctx, p.View(), ", or {", "unexpected token");
				return false;
			}

			string_view baseIdent = ParseIdentifier(p);
			if (baseIdent.empty()) {
				EmitError(ctx, p.View(), "base class identifier", (string)p.View().substr(0, 10));
				return false;
			}

			// We treat qualified names as interned strings for now
			baseClasses.push_back(InternString(std::string(baseIdent).c_str()));

			SkipWhitespaceAndComments(p);
			if (p.CheckFor(',')) {
				p.Consume(1);
				expectingBase = true;
			}
			else {
				expectingBase = false;
			}
			SkipWhitespaceAndComments(p);
		}
		return true;
	}

	// --- Parser: Class Declarations ---
	// Grammar: "class" IDENTIFIER [ ":" base-class-list ] "{" { class-member } "}"
	bool ParseClassDeclaration(TextParser& p, std::vector<uint32_t>& scopeStack, const ParseContext& ctx, Arena& arena, ParseTreeNode* node) {
		SkipWhitespaceAndComments(p);

		// 1. Parse Class Name
		string_view classIdent = ParseIdentifier(p);
		if (classIdent.empty()) {
			EmitError(ctx, p.View(), "class identifier", (string)p.View().substr(0, 10));
			return false;
		}

		// Register the class in the current scope
		std::string classStr(classIdent);
		InternSymbol(classStr.c_str(), scopeStack);

		SkipWhitespaceAndComments(p);

		std::vector<InternedString> baseClasses;

		// 2. Parse Inheritance [ ":" base-class-list ]
		if (p.CheckFor(':')) {
			p.Consume(1);
			if (!ParseBaseClassList(p, ctx, arena, baseClasses)) {
				return false;
			}
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('{')) {
			string_view gotStr = p.View().empty() ? "EOF" : p.View().substr(0, 1);
			EmitError(ctx, p.View(), "{", string(gotStr));
			return false;
		}
		p.Consume(1); // Consume the '{'

		// Create a scope specifically for the class body
		uint32_t classScopeId = AllocScopeId();
		scopeStack.push_back(classScopeId);

		std::vector<ParseTreeNode*> members;
		bool success = true;

		SkipWhitespaceAndComments(p);

		// 3. Parse Class Members
		while (!p.Empty() && !p.CheckFor('}')) {
			string_view v = p.View();

			// Detect Destructor: "~" IDENTIFIER
			if (p.CheckFor('~')) {
				p.Consume(1);
				string_view dtorName = ParseIdentifier(p);

				SkipWhitespaceAndComments(p);
				if (!p.CheckFor('{')) {
					EmitError(ctx, p.View(), "{ on destructor", "missing body");
					return false;
				}
				string_view bodyContent = p.ReadBracedContent('{', '}');

				ParseTreeNode* dtorNode = arena.Construct<ParseTreeNode>();
				dtorNode->patchSymbolCount = 1;
				dtorNode->patchSymbols = arena.ConstructArray<InternedString>(1);
				dtorNode->patchSymbols[0] = InternString("DESTRUCTOR");

				members.push_back(dtorNode);
			}
			else {
				// We peek ahead to distinguish methods from simple fields
				string_view firstIdent = ParseIdentifier(p);
				SkipWhitespaceAndComments(p);

				if (p.CheckFor('(')) {
					// 1. It is a Constructor (or return-less method): IDENTIFIER "("
					p.Consume(1); // take the '('

					// Re-evaluate that keyword as a function name
					SkipWhitespaceAndComments(p);
					string_view paramContent = p.ReadBracedContent('(', ')');

					// Scope setup
					uint32_t ctorScopeId = AllocScopeId();
					scopeStack.push_back(ctorScopeId);

					TextParser paramParser(paramContent);
					ParseParameterList(paramParser, ctx, scopeStack, arena);

					SkipWhitespaceAndComments(p);
					if (p.CheckFor('{')) {
						p.ReadBracedContent('{', '}');
					}
					else {
						p.ReadUntil(';');
						p.Consume(1);
					}

					scopeStack.pop_back();

					ParseTreeNode* ctorNode = arena.Construct<ParseTreeNode>();
					ctorNode->patchSymbolCount = 1;
					ctorNode->patchSymbols = arena.ConstructArray<InternedString>(1);
					ctorNode->patchSymbols[0] = InternString("CONSTRUCTOR");

					members.push_back(ctorNode);
				}
				else if (p.CheckFor('{') || p.View().starts_with("->") || (!p.Empty() && p.View().find('(') != string_view::npos)) {
					// 2. It is a Typed Method! "type" IDENTIFIER "("
					// We step backwards and hand the pointer straight to ParseFunction
					// which knows how to process signatures and body content perfectly.

					ParseTreeNode* methodNode = arena.Construct<ParseTreeNode>();

					// Fall back and let your master function parse it 
					bool methodSuccess = ParseFunction(p, scopeStack, methodNode, ctx, arena, firstIdent);
					if (!methodSuccess) return false;

					members.push_back(methodNode);
				}
				else {
					// 3. Member Variable (Field)
					p.ReadUntil(';');
					p.Consume(1); // consume the ';'

					ParseTreeNode* varNode = arena.Construct<ParseTreeNode>();
					members.push_back(varNode);
				}
			}
			SkipWhitespaceAndComments(p);
		}

		scopeStack.pop_back(); // Pop class scope

		if (!p.CheckFor('}')) {
			EmitError(ctx, p.View(), "}", "missing closing brace on class");
			return false;
		}
		p.Consume(1);

		// 4. Commit bases and members onto the isolated Arena block
		node->patchSymbolCount = static_cast<uint32_t>(baseClasses.size());
		if (node->patchSymbolCount > 0) {
			node->patchSymbols = arena.ConstructArray<InternedString>(node->patchSymbolCount);
			for (uint32_t i = 0; i < node->patchSymbolCount; ++i) {
				node->patchSymbols[i] = baseClasses[i];
			}
		}

		node->childCount = static_cast<uint32_t>(members.size());
		if (node->childCount > 0) {
			node->children = arena.ConstructArray<ParseTreeNode*>(node->childCount);
			for (uint32_t i = 0; i < node->childCount; ++i) {
				node->children[i] = members[i];
			}
		}

		return true;
	}

	// --- Parser: Error Declarations ---
	// Grammar: "error" IDENTIFIER "(" [ parameter-list ] ")" ";"
	// Update signature: returns ParseTreeNode* and has no trailing node parameter
	ParseTreeNode* ParseErrorDeclaration(TextParser& p, std::vector<uint32_t>& scopeStack, const ParseContext& ctx, Arena& arena) {
		SkipWhitespaceAndComments(p);

		string_view errIdent = ParseIdentifier(p);
		if (errIdent.empty()) {
			EmitError(ctx, p.View(), "IDENTIFIER after 'error'", (string)p.View().substr(0, 10));
			return nullptr;
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('(')) {
			EmitError(ctx, p.View(), "(", "missing opening parenthesis on error declaration");
			return nullptr;
		}

		string_view paramContent = p.ReadBracedContent('(', ')');

		std::string errName(errIdent);
		InternSymbol(errName.c_str(), scopeStack);

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(';')) {
			EmitError(ctx, p.View(), ";", "missing terminating semicolon on error declaration");
			return nullptr;
		}
		p.Consume(1);

		uint32_t errScopeId = AllocScopeId();
		scopeStack.push_back(errScopeId);

		TextParser paramParser(paramContent);
		bool paramSuccess = ParseParameterList(paramParser, ctx, scopeStack, arena);

		scopeStack.pop_back();

		if (!paramSuccess) return nullptr;

		// 1. Create the node right here on the arena
		ParseTreeNode* errorNode = arena.Construct<ParseTreeNode>();
		errorNode->patchSymbolCount = 1;
		errorNode->patchSymbols = arena.ConstructArray<InternedString>(1);
		errorNode->patchSymbols[0] = InternString(errName.c_str());

		// 2. Return it back to ParseStatement
		return errorNode;
	}

}
