#pragma once

// compiler/Lexer.h
// Table-driven lexer for .xm source files.
// BNF reference: xcm_06.bnf

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "PipelineQueue.h"

namespace xmc {

	class Lexer
	{
	public:

		// ============================================================================
		// TokenType
		// ============================================================================

		enum class TokenType : uint16_t
		{
			// --- Literals ---
			LIT_INTEGER,        // 42  0xFF  0b1101
			LIT_FLOAT,          // 3.14  1.0e-9
			LIT_DECIMAL,        // 19.90d
			LIT_STRING,         // "hello {name}!"
			LIT_CHAR,           // 'a'  '\n'

			// --- Identifier / refinement mask ---
			IDENTIFIER,
			UGLY_MASK,          // ~ifmrhrss  (immediately follows an identifier, no space)

			// --- Primitive type keywords ---
			KW_B,
			KW_I8, KW_I16, KW_I32, KW_I64,
			KW_U8, KW_U16, KW_U32, KW_U64,
			KW_F32, KW_F64,
			KW_D16, KW_D32, KW_D64,
			KW_C8, KW_C16,
			KW_ZS8, KW_ZS16, KW_ZS32,
			KW_S8, KW_S16,
			KW_UTF8, KW_US,

			// --- Container type keywords ---
			KW_LIST, KW_QUEUE, KW_STACK,
			KW_BTREE, KW_HASHSET, KW_HASHMAP,
			KW_SPSC_QUEUE, KW_SPMC_QUEUE,
			KW_MPSC_QUEUE, KW_MPMC_QUEUE,

			// --- Declaration keywords ---
			KW_VAR, KW_LET, KW_REF,
			KW_NAMESPACE,
			KW_STRUCT, KW_CLASS,
			KW_ERROR, KW_EXTERN, KW_WEAK,
			KW_ARENA, KW_HEAP, KW_FUNCTION,

			// --- Value / builtin keywords ---
			KW_NULL, KW_ME, KW_IS,
			KW_TRUE, KW_FALSE,
			KW_MISSING_ATTRIBUTE,   // __missing_attribute
			KW_RANGE,

			// --- Control flow keywords ---
			KW_IF, KW_THEN, KW_ELSE,
			KW_FOR, KW_WHILE, KW_DO, KW_IN,
			KW_RETURN, KW_YIELD, KW_BREAK,
			KW_SPAWN,

			// --- Low refinement qualifier keywords (floor constraints) ---
			KW_IMMUTABLE, KW_FIXED, KW_MATERIALIZED, KW_RAW,
			KW_HOMOGENOUS, KW_RIGID, KW_SERIAL, KW_SINGULAR,

			// --- High refinement qualifier keywords (ceiling constraints) ---
			KW_MUTABLE, KW_RESIZABLE, KW_VIRTUAL, KW_ARC,
			KW_HETEROGENOUS, KW_FLUID, KW_CONCURRENT, KW_VARIANT,

			// --- Assignment operators ---
			OP_ASSIGN,          // =
			OP_ASSIGN_REF,      // :=

			// --- Arithmetic operators ---
			OP_PLUS,            // +
			OP_MINUS,           // -
			OP_STAR,            // *
			OP_SLASH,           // /
			OP_PERCENT,         // %

			// --- Compound assignment operators ---
			OP_PLUS_ASSIGN,     // +=
			OP_MINUS_ASSIGN,    // -=
			OP_STAR_ASSIGN,     // *=
			OP_SLASH_ASSIGN,    // /=
			OP_PERCENT_ASSIGN,  // %=
			OP_AMP_ASSIGN,      // &=
			OP_PIPE_ASSIGN,     // |=
			OP_CARET_ASSIGN,    // ^=
			OP_LSHIFT_ASSIGN,   // <<=
			OP_RSHIFT_ASSIGN,   // >>=

			// --- Comparison and shift operators ---
			OP_EQ,      // ==
			OP_NEQ,     // !=
			OP_LT,      // <
			OP_GT,      // >
			OP_LTE,     // <=
			OP_GTE,     // >=
			OP_LSHIFT,  // <<
			OP_RSHIFT,  // >>

			// --- Bitwise operators ---
			OP_AMP,     // &
			OP_PIPE,    // |
			OP_CARET,   // ^
			OP_TILDE,   // ~ (bitwise NOT in expression context)

			// --- Logical operators ---
			OP_AND,     // &&
			OP_OR,      // ||
			OP_BANG,    // !

			// --- Increment / decrement ---
			OP_PLUSPLUS,    // ++
			OP_MINUSMINUS,  // --

			// --- Pointer / address operators ---
			// FLAG_PRECEDED_BY_SPACE is set on @ and $ tokens when whitespace
			// appeared before them; the parser uses this to enforce the
			// no-whitespace rule described in BNF sections 21b.
			OP_AT,      // @
			OP_DOLLAR,  // $
			OP_ARROW,   // ->

			// --- Punctuation ---
			PUNCT_DOT,          // .
			PUNCT_COMMA,        // ,
			PUNCT_SEMICOLON,    // ;
			PUNCT_COLON,        // :
			PUNCT_LPAREN,       // (
			PUNCT_RPAREN,       // )
			PUNCT_LBRACKET,     // [
			PUNCT_RBRACKET,     // ]
			PUNCT_LBRACE,       // {
			PUNCT_RBRACE,       // }
			PUNCT_ERROR_OPEN,   // {?
			PUNCT_ERROR_CLOSE,  // ?}

			// --- Trivia: present in Result::tokens and the log, not in parserQueue ---
			TRIVIA_LINE_COMMENT,
			TRIVIA_BLOCK_COMMENT,

			// --- Diagnostics ---
			TOK_ERROR,
			TOK_EOF,

			TOK_COUNT
		};

		// ============================================================================
		// Token
		// ============================================================================

		struct Token
		{
			TokenType type;
			uint32_t  line;         // 1-based line of token's first character
			uint32_t  col;          // 1-based column of token's first character
			uint32_t  srcStart;     // byte offset into the source buffer
			uint32_t  srcLen;       // byte length of the raw token text
			uint8_t   flags;

			// Set when one or more whitespace characters appeared between the
			// previous non-whitespace token and this one.  Relevant to the parser
			// for enforcing the no-whitespace-before-operand rule on @ and $.
			static constexpr uint8_t FLAG_PRECEDED_BY_SPACE = 0x01;

			bool PrecededBySpace() const { return (flags & FLAG_PRECEDED_BY_SPACE) != 0; }

			// Slice of the original source that produced this token.
			std::string_view Text(std::string_view src) const
			{
				return src.substr(srcStart, srcLen);
			}
		};

		// ============================================================================
		// Result
		// ============================================================================

		struct Result
		{
			std::vector<Token> tokens;   // every token in source order, including trivia
			bool               hadErrors = false;
		};

		// ============================================================================
		// Public API
		// ============================================================================

		// Human-readable token type name for logging and error messages.
		static const char* TokenTypeName(TokenType tt);

		// One-time table initialization.  Called automatically by the first Lex()
		// invocation; safe to call manually for startup pre-warming.
		static void InitTables();

		// Tokenize source.
		//
		//   filename    - used only for log output and error message prefixes
		//   source      - the complete source text; need not be NUL-terminated
		//   parserQueue - caller-owned concurrent queue; every parser-visible token
		//                 (no trivia, no errors) is enqueued here as it is produced.
		//                 The final enqueued token is always TOK_EOF.
		//   logStream   - if non-null, one line per token is written in the format:
		//
		//                   filename:line:col  TOKEN_TYPE_NAME  raw_text
		//
		//                 Comments appear in the log even though they are not
		//                 forwarded to the parser.
		//
		// Returns a Result whose tokens vector contains every token (including
		// trivia) in source order.  Check hadErrors for lex-level diagnostics.
		static void Lex(std::string_view filename, std::string_view source, xmc::PipelineQueue<Token>& out, bool writeLog);

	private:

		// Returns true for tokens that should be forwarded to the parser.
		// Trivia (comments) and errors are excluded; callers can examine
		// Result::tokens and Result::hadErrors to handle them.
		static bool IsParserToken(TokenType tt)
		{
			return tt != TokenType::TRIVIA_LINE_COMMENT
				&& tt != TokenType::TRIVIA_BLOCK_COMMENT
				&& tt != TokenType::TOK_ERROR;
		}
	};

} // namespace xmc