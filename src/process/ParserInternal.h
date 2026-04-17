#pragma once

#include<vector>

#include "data/ParserData.h"

#include "process/Parser.h"

#include "tool/StringFunctions.h"
#include "tool/TextParser.h"
using namespace data;

namespace process
{
	// ParseDeclarations.cpp
	bool ParseNamespace(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx, Arena& arena);
	bool ParseTopLevelDeclaration(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx, Arena& arena);
	bool ParseFunction(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx, Arena& arena, string_view inferredType);
	bool ParseClassDeclaration(TextParser& p, std::vector<uint32_t>& scopeStack, const ParseContext& ctx, data::Arena& arena, data::ParseTreeNode* node);
	ParseTreeNode* ParseErrorDeclaration(TextParser& p, std::vector<uint32_t>& scopeStack, const ParseContext& ctx, Arena& arena);
	bool ParseStruct(TextParser& p, std::vector<uint32_t>& scopeStack, ParseTreeNode* node, const ParseContext& ctx, Arena& arena);

	// ParserFunctions.cpp
	ParseTreeNode* ParseExpression(TextParser& p, const ParseContext& ctx, Arena& arena);
	bool ParseExternDeclaration(TextParser& p, const ParseContext& ctx, data::Arena& arena, data::ParseTreeNode* node);
	bool ParseExternParameterList(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<InternedString>& symbols);
	bool ParseParameterList(TextParser& p, const ParseContext& ctx, std::vector<uint32_t>& scopeStack, Arena& arena);

	// ParserStatements.cpp
	bool IsValidUglyMask(std::string_view mask);
	bool IsRefinementQualifier(std::string_view word);
	ParseTreeNode* ParseStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack);
	ParseTreeNode* ParseAssignmentStatement(TextParser& p, const ParseContext& ctx, Arena& arena);
	ParseTreeNode* ParseIfStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack);
	ParseTreeNode* ParseForStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack);
	ParseTreeNode* ParseWhileStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack);
	ParseTreeNode* ParseDoStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack);
	ParseTreeNode* ParseReturnStatement(TextParser& p, const ParseContext& ctx, Arena& arena);
	ParseTreeNode* ParseLabeledBlock(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack);
	ParseTreeNode* ParseStatement(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack);
	ParseTreeNode* ParseVariableDeclaration(TextParser& p, std::vector<uint32_t>& scopeStack, const ParseContext& ctx, Arena& arena, bool isExplicitVar, std::string_view inferredType = "");	
	ParseTreeNode* ParseErrorBlock(TextParser& p, const ParseContext& ctx, Arena& arena, std::vector<uint32_t>& scopeStack);

	// ParserExpressions.cpp
	ParseTreeNode* ParseListExpr(TextParser& p, const ParseContext& ctx, Arena& arena);
	ParseTreeNode* ParseListComprehension(TextParser& p, const ParseContext& ctx, Arena& arena);
	ParseTreeNode* ParseDictExpr(TextParser& p, const ParseContext& ctx, Arena& arena);

	inline thread_local bool BraceMatchError;
}
