#include "pch.h"
#include "ParserInternal.h"

using namespace data;

namespace process
{
	// Forward declarations for mutually recursive expression rules
	ParseTreeNode* ParseExpression(TextParser& p, const ParseContext& ctx, Arena& arena);
	ParseTreeNode* ParseUnaryExpr(TextParser& p, const ParseContext& ctx, Arena& arena);

	// --- 1. Primary Expressions ---
	// Grammar: literal | IDENTIFIER | "me" | "null" | "(" expression ")" | list-comprehension | list-expr
	ParseTreeNode* ParsePrimaryExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		SkipWhitespaceAndComments(p);
		if (p.Empty()) return nullptr;

		string_view view = p.View();

		// Branch A: Handle Bracketed Expressions ([...])
		if (p.CheckFor('[')) {
			p.Consume(1);
			SkipWhitespaceAndComments(p);

			// Safe peek to see if it's a comprehension
			TextParser peekParser(p.View());

			int bracketCount = 0;
			bool foundFor = false;
			while (!peekParser.Empty()) {
				if (peekParser.CheckFor('[')) bracketCount++;
				else if (peekParser.CheckFor(']')) bracketCount--;

				if (bracketCount < 0) break; // Exited our block

				if (bracketCount == 0 && peekParser.View().starts_with("for")) {
					foundFor = true;
					break;
				}
				peekParser.Consume(1);
			}

			if (foundFor) {
				// It's a comprehension! Hand over tracking to our function
				return ParseListComprehension(p, ctx, arena);
			}

			// Fallback: Parse standard comma-separated array lists
			return ParseListExpr(p, ctx, arena);
		}

		// Branch B: Handle Parenthesized Expressions ((...))
		if (p.CheckFor('(')) {
			p.Consume(1);
			ParseTreeNode* exprNode = ParseExpression(p, ctx, arena);
			SkipWhitespaceAndComments(p);

			if (!p.CheckFor(')')) {
				EmitError(ctx, p.View(), ")", "missing closing parenthesis on expression");
				return nullptr;
			}
			p.Consume(1);
			return exprNode;
		}

		// Branch C: Handle Identifiers
		TextParser p2(p);
		string_view ident = ParseIdentifier(p2);
		if (!ident.empty()) {
			ParseTreeNode* node = arena.Construct<ParseTreeNode>();

			node->patchSymbolCount = 1;
			node->patchSymbols = arena.ConstructArray<InternedString>(1);
			node->patchSymbols[0] = InternString(std::string(ident).c_str());
			p = p2;
			return node;
		}

		// Branch D: Handle literals, "me", "null"
		if (view.starts_with("null")) {
			p.Consume(4);
			ParseTreeNode* node = arena.Construct<ParseTreeNode>();
			node->patchSymbolCount = 1;
			node->patchSymbols = arena.ConstructArray<InternedString>(1);
			node->patchSymbols[0] = InternString("null");
			return node;
		}

		if (view.starts_with("me")) {
			p.Consume(2);
			ParseTreeNode* node = arena.Construct<ParseTreeNode>();
			node->patchSymbolCount = 1;
			node->patchSymbols = arena.ConstructArray<InternedString>(1);
			node->patchSymbols[0] = InternString("me");
			return node;
		}

		// Handle String Literals: "text"
		if (p.CheckFor('"')) {
			p.Consume(1); // consume starting '"'

			// Extract everything until the closing quote
			auto [strContent, delimiter] = p.ReadUntil("\"", true);

			ParseTreeNode* node = arena.Construct<ParseTreeNode>();
			node->patchSymbolCount = 1;
			node->patchSymbols = arena.ConstructArray<InternedString>(1);
			node->patchSymbols[0] = InternString(std::string(strContent).c_str());

			// FUTURE HOOK: Flag this as a literal string so the emitter 
			// drops it in the static data section and gives it an export ID.

			return node;
		}

		// Handle Numeric Literals (Int or Float)
		// If the very next character is a digit, we ask ReadValue to parse it
		if (!view.empty() && isdigit(static_cast<unsigned char>(view[0]))) {
			ParseTreeNode* node = arena.Construct<ParseTreeNode>();
			node->patchSymbolCount = 1;
			node->patchSymbols = arena.ConstructArray<InternedString>(1);

			// 1. Handle Hex (0x)
			if (view.starts_with("0x") || view.starts_with("0X")) {
				p.Consume(2);
				long long val = 0;

				auto [ptr, ec] = std::from_chars(p.View().data(), p.View().data() + p.View().size(), val, 16);
				p.Consume(ptr - p.View().data());

				node->patchSymbols[0] = InternString(std::to_string(val).c_str());
				return node;
			}

			// 2. Handle Binary (0b)
			if (view.starts_with("0b") || view.starts_with("0B")) {
				p.Consume(2);
				long long val = 0;

				auto [ptr, ec] = std::from_chars(p.View().data(), p.View().data() + p.View().size(), val, 2);
				p.Consume(ptr - p.View().data());

				node->patchSymbols[0] = InternString(std::to_string(val).c_str());
				return node;
			}
			// 3. Handle standard numbers
			p.Skip(" \t\r\n");
			string_view currentView = p.View();

			// Pre-calculate the span of the number
			size_t len = 0;
			while (len < currentView.size() &&
				(isdigit(static_cast<unsigned char>(currentView[len])) || currentView[len] == '.')) {
				len++;
			}

			string_view numSpan = currentView.substr(0, len);

			node->patchSymbolCount = 1;
			node->patchSymbols = arena.ConstructArray<InternedString>(1);

			if (numSpan.find('.') != string_view::npos) {
				// Floating Point
				double val = 0;

				// Parse from the safe isolated string slice
				auto [ptr, ec] = std::from_chars(numSpan.data(), numSpan.data() + numSpan.size(), val);

				// Advance the parser past the ENTIRE read number span
				p.Consume(static_cast<size_t>(ptr - numSpan.data()));

				// Handle your custom "d" suffix!
				if (p.CheckFor('d') || p.CheckFor('D')) {
					p.Consume(1);
				}
				node->patchSymbols[0] = InternString(std::to_string(val).c_str());
			}
			else {
				// Integer
				long long val = 0;
				auto [ptr, ec] = std::from_chars(numSpan.data(), numSpan.data() + numSpan.size(), val);

				p.Consume(static_cast<size_t>(ptr - numSpan.data()));

				node->patchSymbols[0] = InternString(std::to_string(val).c_str());
			}

			return node;
		}

		// Branch E: Handle Dict Expressions ({...})
		if (p.CheckFor('{')) {
			p.Consume(1);
			return ParseDictExpr(p, ctx, arena);
		}

		EmitError(ctx, p.View(), "primary expression target", "unrecognized token");
		return nullptr;
	}

	// --- 2. Postfix Expressions ---
	// Grammar: primary-expr { postfix-op }
	ParseTreeNode* ParsePostfixExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		ParseTreeNode* leftNode = ParsePrimaryExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);

		// Loop through trailing operations like member access '.' or subscripts '[]'
		while (!p.Empty() && (p.CheckFor('.') || p.CheckFor('['))) {

			// Handle Member Access or Namespace Resolution: . IDENTIFIER
			if (p.CheckFor('.')) {
				string_view v = p.View();
				if (v.size() > 1 && isdigit(static_cast<unsigned char>(v[1]))) {
					break; // members can't start with digits, this must be a floating point number.
				}
				p.Consume(1);
				string_view memberIdent = ParseIdentifier(p);
				if (memberIdent.empty()) {
					EmitError(ctx, p.View(), "IDENTIFIER after '.'", (string)p.View().substr(0, 10));
					return nullptr;
				}

				// Create the operator node
				ParseTreeNode* dotNode = arena.Construct<ParseTreeNode>();
				dotNode->childCount = 1;
				dotNode->children = arena.ConstructArray<ParseTreeNode*>(1);
				dotNode->children[0] = leftNode;

				// Store the member name on the node for future symbol reference
				dotNode->patchSymbolCount = 1;
				dotNode->patchSymbols = arena.ConstructArray<InternedString>(1);
				dotNode->patchSymbols[0] = InternString(std::string(memberIdent).c_str());

				// FUTURE HOOK: This is where the compiler determines if '.' is a 
				// namespace step or a physical struct member access.

				leftNode = dotNode; // Cascade left-to-right (e.g. Core.Math.pi)
			}
			// Handle Subscripts: [ expression ]
			else if (p.CheckFor('[')) {
				p.Consume(1);
				ParseTreeNode* subscriptExpr = ParseExpression(p, ctx, arena);
				if (!subscriptExpr) return nullptr;

				SkipWhitespaceAndComments(p);
				if (!p.CheckFor(']')) {
					EmitError(ctx, p.View(), "]", "missing closing bracket on subscript");
					return nullptr;
				}
				p.Consume(1);

				ParseTreeNode* subNode = arena.Construct<ParseTreeNode>();
				subNode->childCount = 2;
				subNode->children = arena.ConstructArray<ParseTreeNode*>(2);
				subNode->children[0] = leftNode;
				subNode->children[1] = subscriptExpr;

				leftNode = subNode;
			}

			SkipWhitespaceAndComments(p);
		}

		return leftNode;
	}

	// --- 3. Unary Expressions ---
	// Grammar: postfix-expr | "-" unary-expr | "!" unary-expr | etc.
	ParseTreeNode* ParseUnaryExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		return ParsePostfixExpr(p, ctx, arena);
	}

	// --- 4. Multiplicative Expressions ---
	// Grammar: unary-expr { ( "*" | "/" | "%" ) unary-expr }
	ParseTreeNode* ParseMultiplicativeExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		ParseTreeNode* leftNode = ParseUnaryExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);
		while (!p.Empty() && (p.CheckFor('*') || p.CheckFor('/') || p.CheckFor('%'))) {
			char op = p.View()[0];
			p.Consume(1);

			ParseTreeNode* rightNode = ParseUnaryExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after operator", (string)p.View().substr(0, 10));
				return nullptr;
			}

			// Create the parent operator node
			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();

			// FUTURE HOOK: This is exactly where we will eventually inject 
			// the block ID for the operation, e.g., `bid.imul_eax_ebx` or `bid.idiv`
			opNode->childCount = 2;
			opNode->children = arena.NewArray<ParseTreeNode*>(2);
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			leftNode = opNode; // Cascade left-to-right
			SkipWhitespaceAndComments(p);
		}

		return leftNode;
	}


	// --- 5. Additive Expressions ---
	// Grammar: multiplicative-expr { ( "+" | "-" ) multiplicative-expr }
	ParseTreeNode* ParseAdditiveExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		// Grab the left side using the higher-precedence multiplier parser
		ParseTreeNode* leftNode = ParseMultiplicativeExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);

		// Loop through contiguous + or - chain
		while (!p.Empty() && (p.CheckFor('+') || p.CheckFor('-'))) {
			char op = p.View()[0];
			p.Consume(1);

			// Grab the right side
			ParseTreeNode* rightNode = ParseMultiplicativeExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after additive operator", (string)p.View().substr(0, 10));
				return nullptr;
			}

			// Create the operator node
			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();

			// Map out children
			opNode->childCount = 2;
			opNode->children = arena.NewArray<ParseTreeNode*>(2);
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: This is where we will inject instructions like `bid.add_eax_ebx` 
			// or `bid.sub`. We can inspect 'op' to know whether it was '+' or '-'.

			leftNode = opNode; // Cascade left-to-right to handle things like "a + b - c"
			SkipWhitespaceAndComments(p);
		}

		return leftNode;
	}
	// --- 6. Shift Expressions ---
	// Grammar: additive-expr { ( "<<" | ">>" ) additive-expr }
	ParseTreeNode* ParseShiftExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		// Grab the left side using the higher-precedence additive parser
		ParseTreeNode* leftNode = ParseAdditiveExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);

		// Loop through contiguous << or >> chains
		while (!p.Empty() && (p.View().starts_with("<<") || p.View().starts_with(">>"))) {
			string_view opView = p.View().substr(0, 2);
			p.Consume(2); // Jump past both characters at once

			// Grab the right side
			ParseTreeNode* rightNode = ParseAdditiveExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after shift operator", (string)p.View().substr(0, 10));
				return nullptr;
			}

			// Create the operator node
			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();

			// Map out children
			opNode->childCount = 2;
			opNode->children = arena.NewArray<ParseTreeNode*>(2);
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: This is where we will inject instructions like `bid.shl` 
			// or `bid.shr` after we evaluate operands onto registers later on.

			leftNode = opNode; // Cascade left-to-right
			SkipWhitespaceAndComments(p);
		}

		return leftNode;
	}

	// --- 7. Relational Expressions ---
		// Grammar: shift-expr { ( "<" | ">" | "<=" | ">=" ) shift-expr }
	ParseTreeNode* ParseRelationalExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		// Grab the left side using the higher-precedence shift parser
		ParseTreeNode* leftNode = ParseShiftExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);

		// Loop through contiguous comparison chains
		while (!p.Empty()) {
			string_view v = p.View();
			bool matched = false;
			size_t consumeLen = 0;

			// Detect 2-character operators first to prevent greedily matching just '<' or '>'
			if (v.starts_with("<=")) {
				consumeLen = 2;
				matched = true;
			}
			else if (v.starts_with(">=")) {
				consumeLen = 2;
				matched = true;
			}
			else if (v.starts_with("<")) {
				consumeLen = 1;
				matched = true;
			}
			else if (v.starts_with(">")) {
				consumeLen = 1;
				matched = true;
			}

			if (!matched) break;

			p.Consume(consumeLen);

			// Grab the right side
			ParseTreeNode* rightNode = ParseShiftExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after comparison operator", (string)p.View().substr(0, 10));
				return nullptr;
			}

			// Create the operator node
			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();

			// Map out children
			opNode->childCount = 2;
			opNode->children = (ParseTreeNode**)arena.Allocate(2 * sizeof(ParseTreeNode*));
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: This is where we will inject comparison codes like `bid.cmp` 
			// combined with condition codes during the future code block generation pass.

			leftNode = opNode; // Cascade left-to-right
			SkipWhitespaceAndComments(p);
		}

		return leftNode;
	}

	// --- 8. Equality Expressions ---
	// Grammar: relational-expr { ( "==" | "!=" ) relational-expr }
	ParseTreeNode* ParseEqualityExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		// Grab the left side using the higher-precedence relational parser
		ParseTreeNode* leftNode = ParseRelationalExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);

		// Loop through contiguous == or != chains
		while (!p.Empty() && (p.View().starts_with("==") || p.View().starts_with("!="))) {
			string_view opView = p.View().substr(0, 2);
			p.Consume(2); // Jump past both characters at once

			// Grab the right side using the relational parser
			ParseTreeNode* rightNode = ParseRelationalExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after equality operator", (string)p.View().substr(0, 10));
				return nullptr;
			}

			// Create the operator node
			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();

			// Map out children on the file's arena
			opNode->childCount = 2;
			opNode->children = (ParseTreeNode**)arena.Allocate(2 * sizeof(ParseTreeNode*));
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: This is where we will inject comparison blocks like `bid.cmp` 
			// combined with set-if-equal/set-if-not-equal machine codes.

			leftNode = opNode; // Cascade left-to-right
			SkipWhitespaceAndComments(p);
		}

		return leftNode;
	}

	// --- 9. Bitwise AND Expressions ---
	// Grammar: equality-expr { "&" equality-expr }
	ParseTreeNode* ParseBitwiseAndExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		ParseTreeNode* leftNode = ParseEqualityExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);
		while (!p.Empty() && p.CheckFor('&')) {
			// Guard against logical AND "&&" by peeking ahead
			string_view v = p.View();
			if (v.size() > 1 && v[1] == '&') {
				break; // It's logical AND, exit this loop
			}

			p.Consume(1); // Consume the single '&'

			ParseTreeNode* rightNode = ParseEqualityExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after '&'", (string)p.View().substr(0, 10));
				return nullptr;
			}

			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();
			opNode->childCount = 2;
			opNode->children = (ParseTreeNode**)arena.Allocate(2 * sizeof(ParseTreeNode*));
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: Emit a bitwise AND instruction (e.g., `bid.and_eax_ebx`)

			leftNode = opNode;
			SkipWhitespaceAndComments(p);
		}
		return leftNode;
	}

	// --- 10. Bitwise XOR Expressions ---
	// Grammar: bitwise-and-expr { "^" bitwise-and-expr }
	ParseTreeNode* ParseBitwiseXorExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		ParseTreeNode* leftNode = ParseBitwiseAndExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);
		while (!p.Empty() && p.CheckFor('^')) {
			p.Consume(1);

			ParseTreeNode* rightNode = ParseBitwiseAndExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after '^'", (string)p.View().substr(0, 10));
				return nullptr;
			}

			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();
			opNode->childCount = 2;
			opNode->children = (ParseTreeNode**)arena.Allocate(2 * sizeof(ParseTreeNode*));
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: Emit a bitwise XOR instruction (e.g., `bid.xor_eax_ebx`)

			leftNode = opNode;
			SkipWhitespaceAndComments(p);
		}
		return leftNode;
	}

	// --- 11. Bitwise OR Expressions ---
	// Grammar: bitwise-xor-expr { "|" bitwise-xor-expr }
	ParseTreeNode* ParseBitwiseOrExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		ParseTreeNode* leftNode = ParseBitwiseXorExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);
		while (!p.Empty() && p.CheckFor('|')) {
			// Guard against logical OR "||" by peeking ahead
			string_view v = p.View();
			if (v.size() > 1 && v[1] == '|') {
				break; // It's logical OR, exit this loop
			}

			p.Consume(1); // Consume the single '|'

			ParseTreeNode* rightNode = ParseBitwiseXorExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after '|'", (string)p.View().substr(0, 10));
				return nullptr;
			}

			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();
			opNode->childCount = 2;
			opNode->children = (ParseTreeNode**)arena.Allocate(2 * sizeof(ParseTreeNode*));
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: Emit a bitwise OR instruction (e.g., `bid.or_eax_ebx`)

			leftNode = opNode;
			SkipWhitespaceAndComments(p);
		}
		return leftNode;
	}
	// --- 12. Logical AND Expressions ---
	// Grammar: bitwise-or-expr { "&&" bitwise-or-expr }
	ParseTreeNode* ParseLogicalAndExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		// Pulls left side from the higher-precedence Bitwise OR
		ParseTreeNode* leftNode = ParseBitwiseOrExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);
		while (!p.Empty() && p.View().starts_with("&&")) {
			p.Consume(2); // Consume the dual character "&&"

			ParseTreeNode* rightNode = ParseBitwiseOrExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after '&&'", (string)p.View().substr(0, 10));
				return nullptr;
			}

			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();
			opNode->childCount = 2;
			opNode->children = (ParseTreeNode**)arena.Allocate(2 * sizeof(ParseTreeNode*));

			// Placed safely in indices [0] and [1] to avoid the previous conversion error
			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: This is where we will emit short-circuiting jump blocks. 
			// If the left side fails, execution skips evaluating the right side entirely!

			leftNode = opNode;
			SkipWhitespaceAndComments(p);
		}
		return leftNode;
	}

	// --- 13. Logical OR Expressions ---
	// Grammar: logical-and-expr { "||" logical-and-expr }
	ParseTreeNode* ParseLogicalOrExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		// Pulls left side from the higher-precedence Logical AND
		ParseTreeNode* leftNode = ParseLogicalAndExpr(p, ctx, arena);
		if (!leftNode) return nullptr;

		SkipWhitespaceAndComments(p);
		while (!p.Empty() && p.View().starts_with("||")) {
			p.Consume(2); // Consume the dual character "||"

			ParseTreeNode* rightNode = ParseLogicalAndExpr(p, ctx, arena);
			if (!rightNode) {
				EmitError(ctx, p.View(), "expression after '||'", (string)p.View().substr(0, 10));
				return nullptr;
			}

			ParseTreeNode* opNode = arena.Construct<ParseTreeNode>();
			opNode->childCount = 2;
			opNode->children = (ParseTreeNode**)arena.Allocate(2 * sizeof(ParseTreeNode*));

			opNode->children[0] = leftNode;
			opNode->children[1] = rightNode;

			// FUTURE HOOK: Emit short-circuiting jumps. If the left side is already true, 
			// execution skips assessing the right side and maps true instantly.

			leftNode = opNode;
			SkipWhitespaceAndComments(p);
		}
		return leftNode;
	}

	// --- Parser: List Expressions ---
	// Grammar: "[" [ expression { "," expression } ] "]"
	ParseTreeNode* ParseListExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		SkipWhitespaceAndComments(p);

		// If the list is empty [...]
		if (p.CheckFor(']')) {
			p.Consume(1);
			ParseTreeNode* emptyListNode = arena.Construct<ParseTreeNode>();
			return emptyListNode;
		}

		std::vector<ParseTreeNode*> elements;
		bool expectingElement = true;

		while (!p.Empty() && !p.CheckFor(']')) {
			if (!expectingElement) {
				EmitError(ctx, p.View(), ", or ]", "unexpected token in list");
				return nullptr;
			}

			ParseTreeNode* elem = ParseExpression(p, ctx, arena);
			if (!elem) return nullptr;

			elements.push_back(elem);

			SkipWhitespaceAndComments(p);
			if (p.CheckFor(',')) {
				p.Consume(1);
				expectingElement = true;
			}
			else {
				expectingElement = false;
			}
			SkipWhitespaceAndComments(p);
		}

		if (!p.CheckFor(']')) {
			EmitError(ctx, p.View(), "]", "missing closing bracket on list");
			return nullptr;
		}
		p.Consume(1); // consume the ']'

		// Map mapped items safely onto the file's Arena
		ParseTreeNode* listNode = arena.Construct<ParseTreeNode>();
		listNode->childCount = static_cast<uint32_t>(elements.size());

		if (listNode->childCount > 0) {
			listNode->children = arena.ConstructArray<ParseTreeNode*>(listNode->childCount);
			for (uint32_t i = 0; i < listNode->childCount; ++i) {
				listNode->children[i] = elements[i];
			}
		}

		return listNode;
	}

	// --- Parser: List Comprehensions ---
	// Grammar: "[" expression "for" [ type-specifier ] IDENTIFIER "in" expression [ "if" expression ] "]"
	ParseTreeNode* ParseListComprehension(TextParser& p, const ParseContext& ctx, Arena& arena) {
		SkipWhitespaceAndComments(p);

		// 1. The mapped projection expression
		ParseTreeNode* projectionExpr = ParseExpression(p, ctx, arena);
		if (!projectionExpr) return nullptr;

		SkipWhitespaceAndComments(p);
		if (!p.View().starts_with("for")) {
			EmitError(ctx, p.View(), "for keyword in list comprehension", "missing token");
			return nullptr;
		}
		p.Consume(3); // consume "for"

		SkipWhitespaceAndComments(p);

		// 2. Handle optional type specifier or "var" before identifier
		string_view firstIdent = ParseIdentifier(p);
		SkipWhitespaceAndComments(p);

		string_view iteratorIdent;
		string_view nextToken = p.View();

		if (!nextToken.empty() && (isalpha(static_cast<unsigned char>(nextToken[0])) || nextToken[0] == '_')) {
			iteratorIdent = ParseIdentifier(p);
		}
		else {
			iteratorIdent = firstIdent;
		}

		SkipWhitespaceAndComments(p);
		if (!p.View().starts_with("in")) {
			EmitError(ctx, p.View(), "in keyword in list comprehension", "missing token");
			return nullptr;
		}
		p.Consume(2); // consume "in"

		// 3. The source collection/range expression
		ParseTreeNode* collectionExpr = ParseExpression(p, ctx, arena);
		if (!collectionExpr) return nullptr;

		ParseTreeNode* filterExpr = nullptr;
		SkipWhitespaceAndComments(p);

		// 4. Handle optional filter condition: [ "if" expression ]
		if (p.View().starts_with("if")) {
			p.Consume(2); // consume "if"
			filterExpr = ParseExpression(p, ctx, arena);
			if (!filterExpr) return nullptr;
			SkipWhitespaceAndComments(p);
		}

		if (!p.CheckFor(']')) {
			EmitError(ctx, p.View(), "]", "missing closing bracket on list comprehension");
			return nullptr;
		}
		p.Consume(1); // consume the ']'

		// Create the comprehension node
		ParseTreeNode* compNode = arena.Construct<ParseTreeNode>();

		compNode->childCount = filterExpr ? 3 : 2;
		compNode->children = arena.ConstructArray<ParseTreeNode*>(compNode->childCount);

		compNode->children[0] = projectionExpr;
		compNode->children[1] = collectionExpr;
		if (filterExpr) {
			compNode->children[2] = filterExpr;
		}

		// Save the iterator variable name on the node for future symbol reference
		compNode->patchSymbolCount = 1;
		compNode->patchSymbols = arena.ConstructArray<InternedString>(1);
		compNode->patchSymbols[0] = InternString(std::string(iteratorIdent).c_str());

		return compNode;
	}



	// --- Parser: Dict Expressions ---
	// Grammar: "{" [ expression ":" expression { "," expression ":" expression } ] "}"
	ParseTreeNode* ParseDictExpr(TextParser& p, const ParseContext& ctx, Arena& arena) {
		SkipWhitespaceAndComments(p);

		// If the dictionary is empty {}
		if (p.CheckFor('}')) {
			p.Consume(1);
			ParseTreeNode* emptyDictNode = arena.Construct<ParseTreeNode>();
			return emptyDictNode;
		}

		std::vector<ParseTreeNode*> elements;
		bool expectingElement = true;

		while (!p.Empty() && !p.CheckFor('}')) {
			if (!expectingElement) {
				EmitError(ctx, p.View(), ", or }", "unexpected token in dictionary");
				return nullptr;
			}

			// 1. Parse the Key
			ParseTreeNode* keyNode = ParseExpression(p, ctx, arena);
			if (!keyNode) return nullptr;

			SkipWhitespaceAndComments(p);
			if (!p.CheckFor(':')) {
				EmitError(ctx, p.View(), ": between key and value", "missing colon");
				return nullptr;
			}
			p.Consume(1); // consume the ':'

			// 2. Parse the Value
			ParseTreeNode* valueNode = ParseExpression(p, ctx, arena);
			if (!valueNode) return nullptr;

			// Bundle the key and value as a pair node
			ParseTreeNode* pairNode = arena.Construct<ParseTreeNode>();
			pairNode->childCount = 2;
			pairNode->children = arena.ConstructArray<ParseTreeNode*>(2);
			pairNode->children[0] = keyNode;
			pairNode->children[1] = valueNode;

			elements.push_back(pairNode);

			SkipWhitespaceAndComments(p);
			if (p.CheckFor(',')) {
				p.Consume(1);
				expectingElement = true;
			}
			else {
				expectingElement = false;
			}
			SkipWhitespaceAndComments(p);
		}

		if (!p.CheckFor('}')) {
			EmitError(ctx, p.View(), "}", "missing closing brace on dictionary");
			return nullptr;
		}
		p.Consume(1); // consume the '}'

		// Map mapped items safely onto the file's Arena
		ParseTreeNode* dictNode = arena.Construct<ParseTreeNode>();
		dictNode->childCount = static_cast<uint32_t>(elements.size());

		if (dictNode->childCount > 0) {
			dictNode->children = arena.ConstructArray<ParseTreeNode*>(dictNode->childCount);
			for (uint32_t i = 0; i < dictNode->childCount; ++i) {
				dictNode->children[i] = elements[i];
			}
		}

		return dictNode;
	}

	// Updated bridge to target the lowest precedence handled so far
	ParseTreeNode* ParseExpression(TextParser& p, const ParseContext& ctx, Arena& arena) {
		return ParseLogicalOrExpr(p, ctx, arena);
	}
}