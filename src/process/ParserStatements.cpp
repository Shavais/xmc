// process/ParserStatements.cpp
#include "pch.h"
#include "parser.h"
#include "ParserInternal.h"

namespace process
{

	// --- Internal Helper: Validate Ugly Mask ---
	// Grammar: "~" MASK-CHAR { MASK-CHAR } (up to 8)
	bool IsValidUglyMask(std::string_view mask) {
		if (mask.empty() || mask[0] != '~') return false;
		if (mask.size() > 9) return false; // '~' + 8 chars max

		const std::string_view validChars = "imfrvahes_"; // 'c' omitted in your zero-side list but present in one-side! Let's use the whole set:
		const std::string_view fullSet = "imfrvahesc_";

		for (size_t i = 1; i < mask.size(); ++i) {
			if (fullSet.find(mask[i]) == std::string_view::npos) {
				return false; // Found an illegal character
			}
		}
		return true;
	}

	// --- Internal Helper: Check for Min Refinement Qualifiers ---
	// Checks whether a word is any of the 16 refinement markers
	bool IsRefinementQualifier(std::string_view word) {
		static const std::vector<std::string_view> keywords = {
			"immutable", "mutable", "fixed", "resizable",
			"materialized", "virtual", "raw", "arc",
			"homogenous", "heterogenous", "rigid", "fluid",
			"serial", "concurrent", "singular", "variant",
			"*", "&" // shorthands
		};
		return std::find(keywords.begin(), keywords.end(), word) != keywords.end();
	}

	// --- General Statement Dispatcher stub ---
	ParseTreeNode* ParseStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack) {
		SkipWhitespaceAndComments(p);
		if (p.Empty()) return nullptr;

		string_view v = p.View();

		// Detect labeled block start '{' followed by ':'
		// Detect block start '{'
		if (p.CheckFor('{')) {
			TextParser peekParser(v);
			peekParser.Consume(1);
			peekParser.Skip(" \t\r\n");

			// A: Check if it's a labeled block '{:'
			if (peekParser.CheckFor(':')) {
				p.Consume(1); // consume the '{'
				return ParseLabeledBlock(p, ctx, arena, scopeStack);
			}
			// B: Check if it's an error block '{?'
			else if (peekParser.CheckFor('?')) {
				p.Consume(1); // consume the '{'
				p.Consume(1); // consume the '?'
				return ParseErrorBlock(p, ctx, arena, scopeStack);
			}
		}

		if (v.starts_with("if")) {
			p.Consume(2);
			return ParseIfStatement(p, ctx, arena, scopeStack);
		}
		else if (v.starts_with("return")) {
			p.Consume(6);
			return ParseReturnStatement(p, ctx, arena);
		}
		else if (v.starts_with("while")) {
			p.Consume(5);
			return ParseWhileStatement(p, ctx, arena, scopeStack);
		}
		else if (v.starts_with("do")) {
			p.Consume(2);
			return ParseDoStatement(p, ctx, arena, scopeStack);
		}
		else if (v.starts_with("for")) {
			p.Consume(3);
			return ParseForStatement(p, ctx, arena, scopeStack);
		}
		else if (v.starts_with("error")) {
			p.Consume(5);
			// Removed "node" from the parameter list!
			return ParseErrorDeclaration(p, scopeStack, ctx, arena);
		}
		else if (v.starts_with("var")) {
			p.Consume(3);
			return ParseVariableDeclaration(p, scopeStack, ctx, arena, true);
		}

		// Fallback to checking for variable assignments or expressions
		return ParseAssignmentStatement(p, ctx, arena);
	}

	bool ParseVariable(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx) {
		string_view ident = ParseIdentifier(p);
		if (ident.empty()) {
			EmitError(ctx, p.View(), "IDENTIFIER after 'var'", (string)p.View().substr(0, 10));
			return false;
		}

		SkipWhitespaceAndComments(p);

		if (p.CheckFor('~')) {
			auto [mask, delimiter] = p.ReadUntil(" \t\r\n;=: ");
			if (mask.size() > 9) {
				EmitError(ctx, p.View(), "UGLY-MASK (1-8 chars)", "Oversized mask string");
				return false;
			}
		}

		SkipWhitespaceAndComments(p);

		if (p.CheckFor(':')) {
			ConsumeLength(p, 1);
			string_view arenaName = ParseIdentifier(p);
			if (arenaName.empty()) {
				EmitError(ctx, p.View(), "IDENTIFIER for arena name", (string)p.View().substr(0, 10));
				return false;
			}
			SkipWhitespaceAndComments(p);
		}

		if (p.CheckFor('=')) {
			ConsumeLength(p, 1);
			p.ReadUntil(';');
		}
		else if (p.View().starts_with(":=")) {
			ConsumeLength(p, 2);
			p.ReadUntil(';');
		}

		p.Skip(" \t\r\n;");

		std::string tempStr(ident);
		InternSymbol(tempStr.c_str(), scopeStack);

		return true;
	}

	bool ParseTopLevelDeclaration(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx, Arena& arena) {
		SkipWhitespaceAndComments(p);
		if (p.Empty()) return true;

		string_view view = p.View();

		// 1. Peek at the first word without moving the parser cursor yet
		TextParser peekParser(view);
		string_view firstWord = ParseIdentifier(peekParser);

		if (firstWord == "namespace") {
			p.Consume(9); // safely consume "namespace"
			return ParseNamespace(p, scopeStack, node, ctx, arena);
		}
		else if (firstWord == "var") {
			p.Consume(3); // safely consume "var"
			return ParseVariableDeclaration(p, scopeStack, ctx, arena, true);
		}
		else if (firstWord == "arena") {
			p.Consume(5); // safely consume "arena"
			string_view ident = ParseIdentifier(p);
			p.ReadUntil(';');
			return true;
		}
		else if (firstWord == "struct") {
			p.Consume(6); // safely consume "struct"

			// We already have a ParseStruct written in ParserDeclarations.cpp
			// We'll call it directly here.
			return ParseStruct(p, scopeStack, node, ctx, arena);
		}

		// If it's not a known structural keyword, it must be a type for a function or variable!
		if (!firstWord.empty()) {
			// We consume the identifier since it was a type name!
			p.Consume(firstWord.size());

			SkipWhitespaceAndComments(p);
			string_view lookahead = p.View();

			// Peek ahead to see if a parenthesis follows the next identifier.
			// (e.g., `i32 calculate()`)
			TextParser peekParser(lookahead);
			ParseIdentifier(peekParser); // skip identifier
			SkipWhitespaceAndComments(peekParser);

			if (peekParser.CheckFor('(')) {
				// It's a typed function declaration
				return ParseFunction(p, scopeStack, node, ctx, arena, firstWord);
			}
			else {
				// It's a typed variable declaration
				return ParseVariableDeclaration(p, scopeStack, ctx, arena, false);
			}
		}
		else if (firstWord == "var") {
			p.Consume(3); // safely consume "var"

			// FIX APPLIED HERE: Strip the space after "var" so the next 
			// function can immediately see the identifier 'global_speed'
			SkipWhitespaceAndComments(p);

			return ParseVariableDeclaration(p, scopeStack, ctx, arena, true);
		}

		string_view gotStr = view.empty() ? "EOF" : view.substr(0, 5);
		EmitError(ctx, p.View(), "namespace, var, or arena keyword", string(gotStr));
		return false;
	}
	// --- Parser: Return Statements ---
	// Grammar: "return" [ expression ] ";"
	ParseTreeNode* ParseReturnStatement(TextParser& p, const ParseContext& ctx, Arena& arena) {
		SkipWhitespaceAndComments(p);

		// Create the return operation node
		ParseTreeNode* returnNode = arena.Construct<ParseTreeNode>();

		// FUTURE HOOK: This is where we would eventually inject the instruction 
		// block ID to pop the stack and trigger the ret machine code (e.g., `bid.epilogue`).

		// If it's a void return, it will hit the semicolon directly
		if (p.CheckFor(';')) {
			p.Consume(1);
			return returnNode; // Returns an empty return node (leaf)
		}

		// Otherwise, parse the expression being returned
		ParseTreeNode* returnExpr = ParseExpression(p, ctx, arena);
		if (!returnExpr) {
			EmitError(ctx, p.View(), "valid expression or ';'", "invalid token");
			return nullptr;
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(';')) {
			EmitError(ctx, p.View(), "; at end of return", "missing semicolon");
			return nullptr;
		}
		p.Consume(1); // Consume the ';'

		// Attach the expression as a child of the return node
		returnNode->childCount = 1;
		returnNode->children = arena.ConstructArray<ParseTreeNode*>(1);
		returnNode->children[0] = returnExpr;

		return returnNode;
	}

	// --- Internal Helper: Parse LValue ---
	// Grammar: IDENTIFIER { "." IDENTIFIER | "[" expression "]" }
	static ParseTreeNode* ParseLValue(TextParser& p, const ParseContext& ctx, Arena& arena) {
		string_view ident = ParseIdentifier(p);
		if (ident.empty()) {
			EmitError(ctx, p.View(), "IDENTIFIER at start of lvalue", (string)p.View().substr(0, 10));
			return nullptr;
		}

		ParseTreeNode* lvalueNode = arena.Construct<ParseTreeNode>();
		lvalueNode->patchSymbolCount = 1;
		lvalueNode->patchSymbols = arena.ConstructArray<InternedString>(1);
		lvalueNode->patchSymbols[0] = InternString(std::string(ident).c_str());

		SkipWhitespaceAndComments(p);

		// Handle trailing member access or subscripts
		while (!p.Empty() && (p.CheckFor('.') || p.CheckFor('['))) {
			if (p.CheckFor('.')) {
				p.Consume(1);
				string_view memberIdent = ParseIdentifier(p);
				if (memberIdent.empty()) {
					EmitError(ctx, p.View(), "IDENTIFIER after '.'", (string)p.View().substr(0, 10));
					return nullptr;
				}
				// FUTURE HOOK: Chain these as children or append to patch symbols
			}
			else if (p.CheckFor('[')) {
				p.Consume(1);
				ParseTreeNode* subscriptExpr = ParseExpression(p, ctx, arena);
				if (!subscriptExpr) return nullptr;

				SkipWhitespaceAndComments(p);
				if (!p.CheckFor(']')) {
					EmitError(ctx, p.View(), "]", "missing closing bracket");
					return nullptr;
				}
				p.Consume(1);
				// FUTURE HOOK: Chain subscript expression as a child
			}
			SkipWhitespaceAndComments(p);
		}

		return lvalueNode;
	}

	// --- Parser: Assignment Statements ---
	// Grammar: lvalue ( "=" | ":=" | compound-assign-op ) expression ";"
	ParseTreeNode* ParseAssignmentStatement(TextParser& p, const ParseContext& ctx, Arena& arena) {
		ParseTreeNode* lvalue = ParseLValue(p, ctx, arena);
		if (!lvalue) return nullptr;

		SkipWhitespaceAndComments(p);
		string_view v = p.View();

		int opLen = 0;
		bool isReference = false;

		// Detect the specific operator
		if (v.starts_with(":=")) { opLen = 2; isReference = true; }
		else if (v.starts_with("+=")) opLen = 2;
		else if (v.starts_with("-=")) opLen = 2;
		else if (v.starts_with("*=")) opLen = 2;
		else if (v.starts_with("/=")) opLen = 2;
		else if (v.starts_with("%=")) opLen = 2;
		else if (v.starts_with("&=")) opLen = 2;
		else if (v.starts_with("|=")) opLen = 2;
		else if (v.starts_with("^=")) opLen = 2;
		else if (v.starts_with("="))  opLen = 1;

		if (opLen == 0) {
			EmitError(ctx, p.View(), "assignment operator (=, :=, +=, etc.)", "invalid token");
			return nullptr;
		}

		p.Consume(opLen);

		// Parse the right-hand side expression
		ParseTreeNode* rhsExpr = ParseExpression(p, ctx, arena);
		if (!rhsExpr) {
			EmitError(ctx, p.View(), "expression after assignment operator", (string)p.View().substr(0, 10));
			return nullptr;
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(';')) {
			EmitError(ctx, p.View(), ";", "missing semicolon at end of assignment");
			return nullptr;
		}
		p.Consume(1);

		// Create the parent operator node
		ParseTreeNode* assignNode = arena.Construct<ParseTreeNode>();
		assignNode->childCount = 2;
		assignNode->children = arena.ConstructArray<ParseTreeNode*>(2);

		assignNode->children[0] = lvalue;
		assignNode->children[1] = rhsExpr;

		// FUTURE HOOK: Here we will inject block IDs for move instructions 
		// (like `bid.mov_rel_eax`) or specific arithmetic assignments based on `opLen`.

		return assignNode;
	}

	// --- Parser: If Statements ---
	// Grammar: "if" "(" expression ")" statement { "else" "if" "(" expression ")" statement } [ "else" statement ]
	ParseTreeNode* ParseIfStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack) {
		SkipWhitespaceAndComments(p);

		std::vector<ParseTreeNode*> branches;
		branches.reserve(8);

		// Helper lambda to parse a condition + statement branch pair
		auto parseBranch = [&](bool requireCondition) -> bool {
			ParseTreeNode* condition = nullptr;
			if (requireCondition) {
				SkipWhitespaceAndComments(p);
				if (!p.CheckFor('(')) {
					EmitError(ctx, p.View(), "(", "missing opening parenthesis in if/else if");
					return false;
				}
				p.Consume(1);

				condition = ParseExpression(p, ctx, arena);
				if (!condition) return false;

				SkipWhitespaceAndComments(p);
				if (!p.CheckFor(')')) {
					EmitError(ctx, p.View(), ")", "missing closing parenthesis in if/else if");
					return false;
				}
				p.Consume(1);
			}

			ParseTreeNode* body = ParseStatement(p, ctx, arena, scopeStack);
			if (!body) return false;

			// Bundle condition and body together as a branch node
			ParseTreeNode* branchBundle = arena.Construct<ParseTreeNode>();
			branchBundle->childCount = condition ? 2 : 1;
			branchBundle->children = arena.ConstructArray<ParseTreeNode*>(branchBundle->childCount);

			if (condition) {
				branchBundle->children[0] = condition;
				branchBundle->children[1] = body;
			}
			else {
				branchBundle->children[0] = body; // Pure else branch
			}

			branches.push_back(branchBundle);
			return true;
			};

		// Parse the initial 'if'
		if (!parseBranch(true)) return nullptr;

		SkipWhitespaceAndComments(p);

		// Parse zero or more 'else if' chains
		while (!p.Empty() && p.View().starts_with("else")) {
			string_view v = p.View();

			// Peek ahead past "else" to see if it's an "else if"
			TextParser peekParser(v);
			peekParser.Consume(4); // skip "else"
			peekParser.Skip(" \t\r\n");

			if (peekParser.View().starts_with("if")) {
				p.Consume(4); // consume "else"
				SkipWhitespaceAndComments(p);
				p.Consume(2); // consume "if"

				if (!parseBranch(true)) return nullptr;
			}
			else {
				// 3. It's a final isolated 'else'
				p.Consume(4); // consume "else"
				if (!parseBranch(false)) return nullptr;
				break; // An 'else' is always terminal
			}
			SkipWhitespaceAndComments(p);
		}

		// Map the collected list of branches onto the arena array
		ParseTreeNode* ifNode = arena.Construct<ParseTreeNode>();
		ifNode->childCount = static_cast<uint32_t>(branches.size());
		ifNode->children = arena.ConstructArray<ParseTreeNode*>(ifNode->childCount);

		if (ifNode->childCount > 0) {
			ifNode->children = arena.ConstructArray<ParseTreeNode*>(ifNode->childCount);
			memcpy(ifNode->children, branches.data(), ifNode->childCount * sizeof(ParseTreeNode*));
		}

		// FUTURE HOOK: This is where we would map block IDs for conditional jumps 
		// like `bid.jne` mapping to label offsets at the bottom of the execution.

		return ifNode;
	}

	// --- Parser: While Statements ---
	// Grammar: "while" "(" expression ")" statement
	ParseTreeNode* ParseWhileStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack) {
		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('(')) {
			EmitError(ctx, p.View(), "(", "missing opening parenthesis in while");
			return nullptr;
		}
		p.Consume(1);

		ParseTreeNode* condition = ParseExpression(p, ctx, arena);
		if (!condition) return nullptr;

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(')')) {
			EmitError(ctx, p.View(), ")", "missing closing parenthesis in while");
			return nullptr;
		}
		p.Consume(1);

		ParseTreeNode* body = ParseStatement(p, ctx, arena, scopeStack);
		if (!body) return nullptr;

		// Create the parent while node
		ParseTreeNode* whileNode = arena.Construct<ParseTreeNode>();
		whileNode->childCount = 2;
		whileNode->children = arena.ConstructArray<ParseTreeNode*>(2);

		whileNode->children[0] = condition;
		whileNode->children[1] = body;

		// FUTURE HOOK: This maps directly to a jump-to-top loop pattern.
		// `cmp`, jump-if-false to end, loop body, and unconditional `jmp` back to the top.

		return whileNode;
	}

	// --- Parser: Do-While Statements ---
	// Grammar: "do" statement "while" "(" expression ")" ";"
	ParseTreeNode* ParseDoStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack) {
		ParseTreeNode* body = ParseStatement(p, ctx, arena, scopeStack);
		if (!body) return nullptr;

		SkipWhitespaceAndComments(p);
		if (!p.View().starts_with("while")) {
			EmitError(ctx, p.View(), "while", "missing while keyword after do body");
			return nullptr;
		}
		p.Consume(5); // consume "while"

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('(')) {
			EmitError(ctx, p.View(), "(", "missing opening parenthesis on do-while condition");
			return nullptr;
		}
		p.Consume(1);

		ParseTreeNode* condition = ParseExpression(p, ctx, arena);
		if (!condition) return nullptr;

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(')')) {
			EmitError(ctx, p.View(), ")", "missing closing parenthesis on do-while condition");
			return nullptr;
		}
		p.Consume(1);

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(';')) {
			EmitError(ctx, p.View(), ";", "missing semicolon at the end of do-while");
			return nullptr;
		}
		p.Consume(1);

		ParseTreeNode* doNode = arena.Construct<ParseTreeNode>();
		doNode->childCount = 2;
		doNode->children = arena.ConstructArray<ParseTreeNode*>(2);

		doNode->children[0] = body;
		doNode->children[1] = condition;

		// FUTURE HOOK: This is a simpler jump pattern than while loops.
		// Body executes, condition is evaluated, and a conditional jump fires back to top.

		return doNode;
	}

	// --- Parser: For Statements ---
	// Grammar: "for" "(" for-init ";" [ expression ] ";" [ expression ] ")" statement
	//        | "for" "(" [ type-specifier ] IDENTIFIER "in" expression ")" statement
	ParseTreeNode* ParseForStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack) {
		SkipWhitespaceAndComments(p);
		if (!p.CheckFor('(')) {
			EmitError(ctx, p.View(), "(", "missing opening parenthesis on for loop");
			return nullptr;
		}
		p.Consume(1);

		// Peek to see if it's a range-based "in" loop
		string_view v = p.View();
		TextParser peekParser(v);
		peekParser.ReadUntil(" )");

		if (peekParser.View().starts_with("in")) {
			// 1. Handle "for ( identifier in expression )"
			string_view ident = ParseIdentifier(p);
			SkipWhitespaceAndComments(p);
			p.Consume(2); // consume "in"

			ParseTreeNode* collectionExpr = ParseExpression(p, ctx, arena);
			SkipWhitespaceAndComments(p);
			p.Consume(1); // consume ')'

			ParseTreeNode* body = ParseStatement(p, ctx, arena, scopeStack);
			if (!body) return nullptr;

			ParseTreeNode* forInNode = arena.Construct<ParseTreeNode>();
			forInNode->childCount = 2;
			forInNode->children = arena.ConstructArray<ParseTreeNode*>(2);

			// We can carry the loop variable name as a patch symbol in the node
			forInNode->patchSymbolCount = 1;
			forInNode->patchSymbols = arena.ConstructArray<InternedString>(1);
			forInNode->patchSymbols[0] = InternString(std::string(ident).c_str());

			forInNode->children[0] = collectionExpr;
			forInNode->children[1] = body;

			return forInNode;
		}

		// 2. Handle traditional C-style for loop: for ( init ; cond ; inc )
		ParseTreeNode* init = nullptr;
		if (!p.CheckFor(';')) {
			init = ParseStatement(p, ctx, arena, scopeStack); // handles expressions or assignments
		}
		else {
			p.Consume(1); // empty init
		}

		ParseTreeNode* condition = nullptr;
		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(';')) {
			condition = ParseExpression(p, ctx, arena);
		}
		p.Skip(" \t\r\n;");
		p.Consume(1); // Consume the ';'

		ParseTreeNode* increment = nullptr;
		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(')')) {
			increment = ParseExpression(p, ctx, arena);
		}
		p.Skip(" \t\r\n)");
		p.Consume(1); // Consume the ')'

		ParseTreeNode* body = ParseStatement(p, ctx, arena, scopeStack);
		if (!body) return nullptr;

		// Map out the 4 components of a traditional for loop
		ParseTreeNode* forNode = arena.Construct<ParseTreeNode>();
		forNode->childCount = 4;
		forNode->children = arena.ConstructArray<ParseTreeNode*>(4);

		forNode->children[0] = init;
		forNode->children[1] = condition;
		forNode->children[2] = increment;
		forNode->children[3] = body;

		return forNode;
	}

	// --- Parser: Labeled Blocks ---
	// Grammar: "{" ":" IDENTIFIER { statement } "}"
	ParseTreeNode* ParseLabeledBlock(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack) {
		SkipWhitespaceAndComments(p);

		// At this point, the initial '{' has already been consumed by the dispatcher.
		// We expect the colon next.
		if (!p.CheckFor(':')) {
			EmitError(ctx, p.View(), ": in labeled block", "missing colon");
			return nullptr;
		}
		p.Consume(1);

		string_view labelIdent = ParseIdentifier(p);
		if (labelIdent.empty()) {
			EmitError(ctx, p.View(), "IDENTIFIER for block label", (string)p.View().substr(0, 10));
			return nullptr;
		}

		// 1. Create a scoped sub-environment for the block
		uint32_t blockScopeId = AllocScopeId();
		scopeStack.push_back(blockScopeId);

		std::vector<ParseTreeNode*> blockStatements;
		bool success = true;

		SkipWhitespaceAndComments(p);

		// 2. Parse sequential statements until we hit the closing '}'
		while (!p.Empty() && !p.CheckFor('}')) {
			ParseTreeNode* stmt = ParseStatement(p, ctx, arena, scopeStack);
			if (!stmt) {
				success = false;
				break;
			}
			blockStatements.push_back(stmt);
			SkipWhitespaceAndComments(p);
		}

		scopeStack.pop_back();

		if (!success) return nullptr;

		if (!p.CheckFor('}')) {
			EmitError(ctx, p.View(), "}", "missing closing brace on labeled block");
			return nullptr;
		}
		p.Consume(1);

		// 3. Map statements onto the Arena array
		ParseTreeNode* blockNode = arena.Construct<ParseTreeNode>();
		blockNode->childCount = static_cast<uint32_t>(blockStatements.size());

		if (blockNode->childCount > 0) {
			blockNode->children = arena.ConstructArray<ParseTreeNode*>(blockNode->childCount);
			for (uint32_t i = 0; i < blockNode->childCount; ++i) {
				blockNode->children[i] = blockStatements[i];
			}
		}

		// Save the label name on the node for future reference when handling breaks
		blockNode->patchSymbolCount = 1;
		blockNode->patchSymbols = arena.ConstructArray<InternedString>(1);
		blockNode->patchSymbols[0] = InternString(std::string(labelIdent).c_str());

		return blockNode;
	}

	// --- Parser: Variable Declarations ---
	// Grammar: [ type-specifier | "var" ] IDENTIFIER [ UGLY-MASK ] [ arena-qualifier ] [ ( "=" | ":=" ) expression ] ";"
	ParseTreeNode* ParseVariableDeclaration(TextParser& p, std::vector<uint32_t>& scopeStack, const ParseContext& ctx, Arena& arena, bool isExplicitVar, std::string_view inferredType) {
		SkipWhitespaceAndComments(p);

		string_view word = ParseIdentifier(p);
		if (word.empty()) {
			EmitError(ctx, p.View(), "Type or 'var'", (string)p.View().substr(0, 10));
			return nullptr;
		}

		uint32_t maxrmask = 0; // Accumulated ceiling bits for this variable

		// --- 1. HANDLE QUALIFIER CHAINING ---
		while (IsRefinementQualifier(word)) {
			// FUTURE SEMANTIC HOOK: Map 'word' to its specific bit position 
			// and update maxrmask. For variables, these strictly cap the ceiling.
			// e.g., if (word == "raw") maxrmask |= bit_for_raw;

			SkipWhitespaceAndComments(p);
			word = ParseIdentifier(p);
		}

		string_view typeSpec = "";
		string_view ident = "";

		// --- 2. EXTRACT TYPE AND IDENTIFIER ---
		// If we entered here with isExplicitVar = true, we know 'var' was already 
		// consumed by the dispatcher. That means the word we just read right now is the identifier.
		if (isExplicitVar) {
			typeSpec = "var";
			ident = word; 
		} 
		// If a type was already parsed by a parent (like ParseStruct), use it!
		else if (!inferredType.empty()) {
			typeSpec = inferredType;
			ident = word; // The word we extracted was the actual identifier!
		}
		// If it's a typed declaration without the 'var' keyword (e.g., `i32 global_speed;`)
		else if (word != "var" && !IsRefinementQualifier(word)) {
			// 'word' holds the type (e.g., "i32"). We must read the NEXT token to get the identifier.
			typeSpec = word;
			SkipWhitespaceAndComments(p);
			ident = ParseIdentifier(p);
		}
		// If the word we just read is literally "var", it means it wasn't stripped by a dispatcher.
		else if (word == "var") {
			typeSpec = "var";
			SkipWhitespaceAndComments(p);
			ident = ParseIdentifier(p);
		}
		else {
			// This covers the case where we had a qualifier chain but no explicit type.
			// The held word is the identifier.
			typeSpec = "var"; // implicit type
			ident = word;
		}

		if (ident.empty()) {
			std::string gotStr = p.View().empty() ? "EOF" : std::string(p.View().substr(0, 8));
			EmitError(ctx, p.View(), "variable identifier following type", "token '" + gotStr + "'");
			return nullptr;
		}
		// Allocate node on the Arena
		ParseTreeNode* varNode = arena.Construct<ParseTreeNode>();

		// Map the variable's symbol in the current scope
		std::string tempStr(ident);
		Symbol* sym = InternSymbol(tempStr.c_str(), scopeStack);

		// FUTURE SEMANTIC HOOK: Commit the gathered ceiling mask to the symbol
		// sym->maxrmask = maxrmask;

		SkipWhitespaceAndComments(p);

		// --- 3. HANDLE UGLY MASK (Alternative syntax for ceilings) ---
		if (p.CheckFor('~')) {
			auto [mask, delimiter] = p.ReadUntil(" \t\r\n;=: ");
			if (!IsValidUglyMask(mask)) {
				EmitError(ctx, p.View(), "Valid UGLY-MASK ceiling constraint", "Invalid mask character or length");
				return nullptr;
			}
			// Map the positional mask override onto `sym->maxrmask`
		}

		SkipWhitespaceAndComments(p);

		// 3. Handle Arena Qualifiers
		if (p.CheckFor(':')) {
			p.Consume(1);
			string_view arenaName = ParseIdentifier(p);
			if (arenaName.empty()) {
				EmitError(ctx, p.View(), "IDENTIFIER for arena name after ':'", (string)p.View().substr(0, 10));
				return nullptr;
			}
			SkipWhitespaceAndComments(p);
		}

		ParseTreeNode* rhsExpr = nullptr;

		// 4. Handle Assignment
		if (p.CheckFor('=')) {
			p.Consume(1);
			rhsExpr = ParseExpression(p, ctx, arena);
			if (!rhsExpr) return nullptr;
		}
		else if (p.View().starts_with(":=")) {
			p.Consume(2);
			rhsExpr = ParseExpression(p, ctx, arena);
			if (!rhsExpr) return nullptr;
		}

		SkipWhitespaceAndComments(p);
		if (!p.CheckFor(';')) {
			EmitError(ctx, p.View(), "; at end of declaration", "missing semicolon");
			return nullptr;
		}
		p.Consume(1); // Consume the ';'

		// Commit states
		if (rhsExpr) {
			varNode->childCount = 1;
			varNode->children = arena.ConstructArray<ParseTreeNode*>(1);
			varNode->children[0] = rhsExpr;
		}

		// Save the name into the node for future machine code binding
		varNode->patchSymbolCount = 1;
		varNode->patchSymbols = arena.ConstructArray<InternedString>(1);
		varNode->patchSymbols[0] = InternString(tempStr.c_str());

		return varNode;
	}


	// --- Parser: Error Blocks ---
	// Grammar: "{?" { statement } "?}"
	ParseTreeNode* ParseErrorBlock(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack) {
		SkipWhitespaceAndComments(p);

		// At this point, the initial "{" and "?" have been consumed by the dispatcher.
		std::vector<ParseTreeNode*> blockStatements;
		bool success = true;

		SkipWhitespaceAndComments(p);

		// Parse sequential statements until we hit the closing "?}"
		while (!p.Empty() && !p.View().starts_with("?}")) {
			ParseTreeNode* stmt = ParseStatement(p, ctx, arena, scopeStack);
			if (!stmt) {
				success = false;
				break;
			}
			blockStatements.push_back(stmt);
			SkipWhitespaceAndComments(p);
		}

		if (!success) return nullptr;

		if (!p.View().starts_with("?}")) {
			EmitError(ctx, p.View(), "?}", "missing closing sequence on error block");
			return nullptr;
		}
		p.Consume(2); // consume the "?}"

		// Map statements safely onto the Arena array
		ParseTreeNode* errorBlockNode = arena.Construct<ParseTreeNode>();
		errorBlockNode->childCount = static_cast<uint32_t>(blockStatements.size());

		if (errorBlockNode->childCount > 0) {
			errorBlockNode->children = arena.ConstructArray<ParseTreeNode*>(errorBlockNode->childCount);
			for (uint32_t i = 0; i < errorBlockNode->childCount; ++i) {
				errorBlockNode->children[i] = blockStatements[i];
			}
		}

		// Save a tag identifier on the node to let the emitter recognize this as a guarded zone
		errorBlockNode->patchSymbolCount = 1;
		errorBlockNode->patchSymbols = arena.ConstructArray<InternedString>(1);
		errorBlockNode->patchSymbols[0] = InternString("ERROR_GUARD_BLOCK");

		// FUTURE HOOK: This translates to injecting an exception-handling frame on entry
		// and clearing it on exit. If an expression fails, it jumps directly past the `?}` boundary.

		return errorBlockNode;
	}

}
