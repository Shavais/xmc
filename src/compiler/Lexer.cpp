// compiler/Lexer.cpp
// Table-driven lexer for .xm source files.
// BNF reference: xcm_06.bnf

#include "pch.h"
#include "Lexer.h"

#include <cassert>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <unordered_map>

namespace xmc {

	// ============================================================================
	// Internal types
	//
	// These are implementation details private to this translation unit.
	// They are not exposed in Lexer.h.
	// ============================================================================

	// Every distinct state the lexer DFA can be in.
	enum class LexState : uint16_t
	{
		Start = 0,

		// Identifiers and refinement masks
		InIdent,        // accumulating an identifier or keyword
		PostIdent,      // just emitted an identifier; checking for immediately-adjacent ~
		InUglyMask,     // accumulating the mask chars after ~

		// Numeric literals
		InZero,             // just consumed '0'; next char determines 0x / 0b / 0.xxx / integer
		InHexX,             // consumed '0x'; waiting for first hex digit
		InHex,              // accumulating hex digits
		InBinB,             // consumed '0b'; waiting for first binary digit
		InBinary,           // accumulating binary digits
		InInteger,          // accumulating decimal digits (non-zero leading)
		InFloat,            // consumed '.'; accumulating fractional digits
		InFloatExp,         // consumed 'e'/'E'; waiting for optional sign or first digit
		InFloatExpSign,     // consumed sign after 'e'/'E'; waiting for first digit
		InFloatExpDigit,    // accumulating exponent digits

		// String and character literals
		InString,           // inside "..."
		InStringEscape,     // just consumed \ inside a string
		InStringInterp,     // inside {identifier} inside a string
		InChar,             // inside '...'
		InCharEscape,       // just consumed \ inside a char literal

		// Comments
		InLineComment,          // after //
		InBlockComment,         // inside /* ... */
		InBlockCommentStar,     // just consumed * inside block comment; watching for /

		// Multi-character operator prefix states
		InColon,        // :    -> :=  or bare :
		InMinus,        // -    -> ->  --  -=  or bare -
		InAssign,       // =    -> ==  or bare =
		InBang,         // !    -> !=  or bare !
		InLt,           // <    -> <<  <=  or bare <
		InLtLt,         // <<   -> <<= or bare <<
		InGt,           // >    -> >>  >=  or bare >
		InGtGt,         // >>   -> >>= or bare >>
		InAmpersand,    // &    -> &&  &=  or bare &
		InPipe,         // |    -> ||  |=  or bare |
		InPlus,         // +    -> ++  +=  or bare +
		InStar,         // *    -> *=  or bare *
		InSlash,        // /    -> //  /*  /=  or bare /
		InPercent,      // %    -> %=  or bare %
		InCaret,        // ^    -> ^=  or bare ^
		InLBrace,       // {    -> {?  or bare {
		InQuestion,     // ?    -> ?}  (standalone ? is an error)

		NUM_STATES
	};

	// ============================================================================
	// Character classes
	//
	// 128 ASCII characters collapse to this many distinct behaviours.
	// The split among letter sub-classes is driven by numeric literal rules:
	//   x/X  - introduces hex prefix  0x
	//   b/B  - introduces binary prefix 0b; also a valid hex digit
	//   d/D  - terminates a decimal literal with the 'd' suffix; also hex digit
	//   e/E  - introduces float exponent; also hex digit
	//   a/c/f and A/C/F - only used as hex digits among letters
	//   all other letters and _ - general alpha
	// In identifier and ugly-mask states these sub-classes all behave identically.
	// ============================================================================

	enum class CharClass : uint8_t
	{
		Alpha = 0,  // letters and _ not listed below
		X_,         // 'x', 'X'
		B_,         // 'b', 'B'
		D_,         // 'd', 'D'
		E_,         // 'e', 'E'
		HexAlpha,   // 'a','c','f','A','C','F'
		Zero,       // '0'
		Digit19,    // '1'-'9'
		Dot,        // '.'
		Space,      // ' ', '\t'
		Newline,    // '\n', '\r'
		DQuote,     // '"'
		SQuote,     // '\''
		Backslash,  // '\\'
		LParen,     // '('
		RParen,     // ')'
		LBracket,   // '['
		RBracket,   // ']'
		LBrace,     // '{'
		RBrace,     // '}'
		Semicolon,  // ';'
		Comma,      // ','
		Colon,      // ':'
		Equals,     // '='
		Plus,       // '+'
		Minus,      // '-'
		Star,       // '*'
		Slash,      // '/'
		Percent,    // '%'
		Amp,        // '&'
		Pipe,       // '|'
		Caret,      // '^'
		Bang,       // '!'
		Lt,         // '<'
		Gt,         // '>'
		Tilde,      // '~'
		At,         // '@'
		Dollar,     // '$'
		Question,   // '?'
		Eof,        // synthetic: NUL byte (i == len sentinel)
		Other,      // anything else

		NUM_CLASSES
	};

	// ============================================================================
	// Lexer actions
	// ============================================================================

	enum class LexAction : uint8_t
	{
		// Accumulate current char into buffer, advance.
		Acc,

		// Advance without accumulating; mark that whitespace was seen.
		// tokStart is reset to the next position.
		Skip,

		// Accumulate current char, emit as tokenType, advance.
		// tokStart is reset to next position.
		EmitA,

		// Emit buffer as tokenType WITHOUT consuming current char (reconsume).
		// tokStart is reset to current position.
		EmitR,

		// Like EmitR but run buffer through keyword map first;
		// emits IDENTIFIER if not found.
		EmitKwR,

		// Transition to nextState and reconsume current char.
		// No accumulation, no emission.  Used by PostIdent fallback.
		TransR,

		// Emit buffer (if any) as TOK_ERROR, accumulating current char, advance.
		// Always resets state to Start.
		Error,
	};

	// ============================================================================
	// Transition table entry
	// ============================================================================

	struct Trans
	{
		uint16_t  nextState;   // cast to LexState
		LexAction action;
		Lexer::TokenType tokenType;   // meaningful only for EmitA and EmitR/EmitKwR
	};

	// ============================================================================
	// File-local static tables
	// (shared across all Lexer instances; initialised once by InitTables)
	// ============================================================================

	static constexpr int kNumStates = (int)LexState::NUM_STATES;
	static constexpr int kNumClasses = (int)CharClass::NUM_CLASSES;

	static uint8_t  s_charClass[128];
	static Trans    s_trans[kNumStates][kNumClasses];
	static bool     s_tablesBuilt = false;
	static std::mutex s_tablesMutex;

	static std::unordered_map<std::string, Lexer::TokenType> s_keywords;

	// ============================================================================
	// Table builder helpers
	// ============================================================================

	using S = LexState;
	using C = CharClass;
	using A = LexAction;
	using T = Lexer::TokenType;

	static void SetTrans(S from, C cc, S next, A act, T tt = T::TOK_ERROR)
	{
		s_trans[(int)from][(int)cc] = { (uint16_t)next, act, tt };
	}

	template<typename... Cs>
	static void SetTransMany(S from, S next, A act, T tt, Cs... ccs)
	{
		for (C cc : { ccs... })
			SetTrans(from, cc, next, act, tt);
	}

	// ============================================================================
	// Lexer::InitTables
	// ============================================================================

	void Lexer::InitTables()
	{
		std::lock_guard<std::mutex> lock(s_tablesMutex);
		if (s_tablesBuilt) return;

		// -----------------------------------------------------------------------
		// 1.  charClass table
		// -----------------------------------------------------------------------

		memset(s_charClass, (int)C::Other, sizeof(s_charClass));
		s_charClass[0] = (uint8_t)C::Eof;

		// Whitespace
		s_charClass[(uint8_t)' '] = (uint8_t)C::Space;
		s_charClass[(uint8_t)'\t'] = (uint8_t)C::Space;
		s_charClass[(uint8_t)'\n'] = (uint8_t)C::Newline;
		s_charClass[(uint8_t)'\r'] = (uint8_t)C::Newline;

		// General alpha (will be overridden for special letters below)
		for (int c = 'A'; c <= 'Z'; ++c) s_charClass[c] = (uint8_t)C::Alpha;
		for (int c = 'a'; c <= 'z'; ++c) s_charClass[c] = (uint8_t)C::Alpha;
		s_charClass[(uint8_t)'_'] = (uint8_t)C::Alpha;

		// Special letter classes
		s_charClass[(uint8_t)'x'] = (uint8_t)C::X_;
		s_charClass[(uint8_t)'X'] = (uint8_t)C::X_;
		s_charClass[(uint8_t)'b'] = (uint8_t)C::B_;
		s_charClass[(uint8_t)'B'] = (uint8_t)C::B_;
		s_charClass[(uint8_t)'d'] = (uint8_t)C::D_;
		s_charClass[(uint8_t)'D'] = (uint8_t)C::D_;
		s_charClass[(uint8_t)'e'] = (uint8_t)C::E_;
		s_charClass[(uint8_t)'E'] = (uint8_t)C::E_;
		// hex-only letters (not x, b, d, e which are already overridden)
		s_charClass[(uint8_t)'a'] = (uint8_t)C::HexAlpha;
		s_charClass[(uint8_t)'A'] = (uint8_t)C::HexAlpha;
		s_charClass[(uint8_t)'c'] = (uint8_t)C::HexAlpha;
		s_charClass[(uint8_t)'C'] = (uint8_t)C::HexAlpha;
		s_charClass[(uint8_t)'f'] = (uint8_t)C::HexAlpha;
		s_charClass[(uint8_t)'F'] = (uint8_t)C::HexAlpha;

		// Digits
		s_charClass[(uint8_t)'0'] = (uint8_t)C::Zero;
		for (int c = '1'; c <= '9'; ++c) s_charClass[c] = (uint8_t)C::Digit19;

		// Punctuation and operators
		s_charClass[(uint8_t)'.'] = (uint8_t)C::Dot;
		s_charClass[(uint8_t)'"'] = (uint8_t)C::DQuote;
		s_charClass[(uint8_t)'\''] = (uint8_t)C::SQuote;
		s_charClass[(uint8_t)'\\'] = (uint8_t)C::Backslash;
		s_charClass[(uint8_t)'('] = (uint8_t)C::LParen;
		s_charClass[(uint8_t)')'] = (uint8_t)C::RParen;
		s_charClass[(uint8_t)'['] = (uint8_t)C::LBracket;
		s_charClass[(uint8_t)']'] = (uint8_t)C::RBracket;
		s_charClass[(uint8_t)'{'] = (uint8_t)C::LBrace;
		s_charClass[(uint8_t)'}'] = (uint8_t)C::RBrace;
		s_charClass[(uint8_t)';'] = (uint8_t)C::Semicolon;
		s_charClass[(uint8_t)','] = (uint8_t)C::Comma;
		s_charClass[(uint8_t)':'] = (uint8_t)C::Colon;
		s_charClass[(uint8_t)'='] = (uint8_t)C::Equals;
		s_charClass[(uint8_t)'+'] = (uint8_t)C::Plus;
		s_charClass[(uint8_t)'-'] = (uint8_t)C::Minus;
		s_charClass[(uint8_t)'*'] = (uint8_t)C::Star;
		s_charClass[(uint8_t)'/'] = (uint8_t)C::Slash;
		s_charClass[(uint8_t)'%'] = (uint8_t)C::Percent;
		s_charClass[(uint8_t)'&'] = (uint8_t)C::Amp;
		s_charClass[(uint8_t)'|'] = (uint8_t)C::Pipe;
		s_charClass[(uint8_t)'^'] = (uint8_t)C::Caret;
		s_charClass[(uint8_t)'!'] = (uint8_t)C::Bang;
		s_charClass[(uint8_t)'<'] = (uint8_t)C::Lt;
		s_charClass[(uint8_t)'>'] = (uint8_t)C::Gt;
		s_charClass[(uint8_t)'~'] = (uint8_t)C::Tilde;
		s_charClass[(uint8_t)'@'] = (uint8_t)C::At;
		s_charClass[(uint8_t)'$'] = (uint8_t)C::Dollar;
		s_charClass[(uint8_t)'?'] = (uint8_t)C::Question;

		// -----------------------------------------------------------------------
		// 2.  transTable - default every cell to Error -> Start
		// -----------------------------------------------------------------------

		for (int s = 0; s < kNumStates; ++s)
			for (int c = 0; c < kNumClasses; ++c)
				s_trans[s][c] = { (uint16_t)S::Start, A::Error, T::TOK_ERROR };

		// -----------------------------------------------------------------------
		// Helper: set every class NOT in the keep-set to a given transition.
		// Call this after the per-class overrides for a state.
		// -----------------------------------------------------------------------
		auto FillRest = [](S from, S next, A act, T tt,
			std::initializer_list<C> keepClasses)
			{
				for (int c = 0; c < kNumClasses; ++c) {
					bool keep = false;
					for (C k : keepClasses) if ((int)k == c) { keep = true; break; }
					if (!keep) SetTrans(from, (C)c, next, act, tt);
				}
			};

		// -----------------------------------------------------------------------
		// 3.  Start
		// -----------------------------------------------------------------------

		// Identifier start
		SetTransMany(S::Start, S::InIdent, A::Acc, T::TOK_ERROR,
			C::Alpha, C::X_, C::B_, C::D_, C::E_, C::HexAlpha);

		// Numeric start
		SetTrans(S::Start, C::Zero, S::InZero, A::Acc);
		SetTrans(S::Start, C::Digit19, S::InInteger, A::Acc);

		// Whitespace
		SetTrans(S::Start, C::Space, S::Start, A::Skip);
		SetTrans(S::Start, C::Newline, S::Start, A::Skip);

		// String / character literals
		SetTrans(S::Start, C::DQuote, S::InString, A::Acc);
		SetTrans(S::Start, C::SQuote, S::InChar, A::Acc);

		// Single-character punctuation (accumulate the char, emit, advance)
		SetTrans(S::Start, C::LParen, S::Start, A::EmitA, T::PUNCT_LPAREN);
		SetTrans(S::Start, C::RParen, S::Start, A::EmitA, T::PUNCT_RPAREN);
		SetTrans(S::Start, C::LBracket, S::Start, A::EmitA, T::PUNCT_LBRACKET);
		SetTrans(S::Start, C::RBracket, S::Start, A::EmitA, T::PUNCT_RBRACKET);
		SetTrans(S::Start, C::RBrace, S::Start, A::EmitA, T::PUNCT_RBRACE);
		SetTrans(S::Start, C::Semicolon, S::Start, A::EmitA, T::PUNCT_SEMICOLON);
		SetTrans(S::Start, C::Comma, S::Start, A::EmitA, T::PUNCT_COMMA);
		SetTrans(S::Start, C::Dot, S::Start, A::EmitA, T::PUNCT_DOT);
		SetTrans(S::Start, C::At, S::Start, A::EmitA, T::OP_AT);
		SetTrans(S::Start, C::Dollar, S::Start, A::EmitA, T::OP_DOLLAR);
		// ~ in Start (expression context, not after identifier) is bitwise NOT
		SetTrans(S::Start, C::Tilde, S::Start, A::EmitA, T::OP_TILDE);

		// Multi-character operator prefixes
		SetTrans(S::Start, C::LBrace, S::InLBrace, A::Acc);
		SetTrans(S::Start, C::Question, S::InQuestion, A::Acc);
		SetTrans(S::Start, C::Colon, S::InColon, A::Acc);
		SetTrans(S::Start, C::Equals, S::InAssign, A::Acc);
		SetTrans(S::Start, C::Plus, S::InPlus, A::Acc);
		SetTrans(S::Start, C::Minus, S::InMinus, A::Acc);
		SetTrans(S::Start, C::Star, S::InStar, A::Acc);
		SetTrans(S::Start, C::Slash, S::InSlash, A::Acc);
		SetTrans(S::Start, C::Percent, S::InPercent, A::Acc);
		SetTrans(S::Start, C::Amp, S::InAmpersand, A::Acc);
		SetTrans(S::Start, C::Pipe, S::InPipe, A::Acc);
		SetTrans(S::Start, C::Caret, S::InCaret, A::Acc);
		SetTrans(S::Start, C::Bang, S::InBang, A::Acc);
		SetTrans(S::Start, C::Lt, S::InLt, A::Acc);
		SetTrans(S::Start, C::Gt, S::InGt, A::Acc);

		// EOF in Start: emit TOK_EOF token
		SetTrans(S::Start, C::Eof, S::Start, A::EmitA, T::TOK_EOF);

		// -----------------------------------------------------------------------
		// 4.  Identifiers
		// -----------------------------------------------------------------------

		// Continuing chars: all alpha sub-classes and digits
		SetTransMany(S::InIdent, S::InIdent, A::Acc, T::TOK_ERROR,
			C::Alpha, C::X_, C::B_, C::D_, C::E_, C::HexAlpha, C::Zero, C::Digit19);

		// Tilde immediately adjacent (no space): emit ident, enter PostIdent
		// so that PostIdent can consume the tilde as ugly-mask opener.
		SetTrans(S::InIdent, C::Tilde, S::PostIdent, A::EmitKwR);

		// Everything else terminates the identifier
		FillRest(S::InIdent, S::Start, A::EmitKwR, T::IDENTIFIER,
			{ C::Alpha, C::X_, C::B_, C::D_, C::E_, C::HexAlpha,
			  C::Zero, C::Digit19, C::Tilde });

		// PostIdent: the ident has already been emitted; now decide about ~
		SetTrans(S::PostIdent, C::Tilde, S::InUglyMask, A::Acc);
		FillRest(S::PostIdent, S::Start, A::TransR, T::TOK_ERROR, { C::Tilde });

		// InUglyMask: accumulate alpha chars (parser validates which are legal)
		SetTransMany(S::InUglyMask, S::InUglyMask, A::Acc, T::TOK_ERROR,
			C::Alpha, C::X_, C::B_, C::D_, C::E_, C::HexAlpha);
		FillRest(S::InUglyMask, S::Start, A::EmitR, T::UGLY_MASK,
			{ C::Alpha, C::X_, C::B_, C::D_, C::E_, C::HexAlpha });

		// -----------------------------------------------------------------------
		// 5.  Numeric literals
		// -----------------------------------------------------------------------

		// --- InZero: just consumed '0' ---
		SetTrans(S::InZero, C::X_, S::InHexX, A::Acc);          // 0x...
		SetTrans(S::InZero, C::B_, S::InBinB, A::Acc);          // 0b...
		SetTrans(S::InZero, C::Dot, S::InFloat, A::Acc);          // 0....
		SetTrans(S::InZero, C::Zero, S::InInteger, A::Acc);          // 00 (unusual; parser warns)
		SetTrans(S::InZero, C::Digit19, S::InInteger, A::Acc);          // 01 etc.
		FillRest(S::InZero, S::Start, A::EmitR, T::LIT_INTEGER,
			{ C::X_, C::B_, C::Dot, C::Zero, C::Digit19 });

		// --- InHexX: consumed '0x'; need at least one hex digit ---
		SetTransMany(S::InHexX, S::InHex, A::Acc, T::TOK_ERROR,
			C::Zero, C::Digit19, C::HexAlpha, C::B_, C::D_, C::E_);
		FillRest(S::InHexX, S::Start, A::Error, T::TOK_ERROR,
			{ C::Zero, C::Digit19, C::HexAlpha, C::B_, C::D_, C::E_ });

		// --- InHex: accumulating hex digits ---
		SetTransMany(S::InHex, S::InHex, A::Acc, T::TOK_ERROR,
			C::Zero, C::Digit19, C::HexAlpha, C::B_, C::D_, C::E_);
		FillRest(S::InHex, S::Start, A::EmitR, T::LIT_INTEGER,
			{ C::Zero, C::Digit19, C::HexAlpha, C::B_, C::D_, C::E_ });

		// --- InBinB: consumed '0b'; need at least one binary digit ---
		SetTrans(S::InBinB, C::Zero, S::InBinary, A::Acc);
		SetTrans(S::InBinB, C::Digit19, S::InBinary, A::Acc);  // parser validates 2-9
		FillRest(S::InBinB, S::Start, A::Error, T::TOK_ERROR,
			{ C::Zero, C::Digit19 });

		// --- InBinary ---
		SetTrans(S::InBinary, C::Zero, S::InBinary, A::Acc);
		SetTrans(S::InBinary, C::Digit19, S::InBinary, A::Acc);
		FillRest(S::InBinary, S::Start, A::EmitR, T::LIT_INTEGER,
			{ C::Zero, C::Digit19 });

		// --- InInteger: accumulating decimal digits ---
		SetTrans(S::InInteger, C::Zero, S::InInteger, A::Acc);
		SetTrans(S::InInteger, C::Digit19, S::InInteger, A::Acc);
		SetTrans(S::InInteger, C::Dot, S::InFloat, A::Acc);
		FillRest(S::InInteger, S::Start, A::EmitR, T::LIT_INTEGER,
			{ C::Zero, C::Digit19, C::Dot });

		// --- InFloat: after the decimal point ---
		// 'e'/'E' introduces exponent; 'd' suffix makes this a decimal literal.
		SetTrans(S::InFloat, C::Zero, S::InFloat, A::Acc);
		SetTrans(S::InFloat, C::Digit19, S::InFloat, A::Acc);
		SetTrans(S::InFloat, C::E_, S::InFloatExp, A::Acc);
		SetTrans(S::InFloat, C::D_, S::Start, A::EmitA, T::LIT_DECIMAL);
		FillRest(S::InFloat, S::Start, A::EmitR, T::LIT_FLOAT,
			{ C::Zero, C::Digit19, C::E_, C::D_ });

		// --- InFloatExp: after 'e'/'E' ---
		SetTrans(S::InFloatExp, C::Zero, S::InFloatExpDigit, A::Acc);
		SetTrans(S::InFloatExp, C::Digit19, S::InFloatExpDigit, A::Acc);
		SetTrans(S::InFloatExp, C::Plus, S::InFloatExpSign, A::Acc);
		SetTrans(S::InFloatExp, C::Minus, S::InFloatExpSign, A::Acc);
		FillRest(S::InFloatExp, S::Start, A::Error, T::TOK_ERROR,
			{ C::Zero, C::Digit19, C::Plus, C::Minus });

		// --- InFloatExpSign: after sign in exponent ---
		SetTrans(S::InFloatExpSign, C::Zero, S::InFloatExpDigit, A::Acc);
		SetTrans(S::InFloatExpSign, C::Digit19, S::InFloatExpDigit, A::Acc);
		FillRest(S::InFloatExpSign, S::Start, A::Error, T::TOK_ERROR,
			{ C::Zero, C::Digit19 });

		// --- InFloatExpDigit: accumulating exponent digits ---
		SetTrans(S::InFloatExpDigit, C::Zero, S::InFloatExpDigit, A::Acc);
		SetTrans(S::InFloatExpDigit, C::Digit19, S::InFloatExpDigit, A::Acc);
		FillRest(S::InFloatExpDigit, S::Start, A::EmitR, T::LIT_FLOAT,
			{ C::Zero, C::Digit19 });

		// -----------------------------------------------------------------------
		// 6.  String literals
		// -----------------------------------------------------------------------

		// The opening '"' has already been accumulated.  Accumulate everything
		// up to and including the closing '"'.  '{' begins an interpolation.
		// Newline and EOF inside a string are errors (terminate the token).

		SetTrans(S::InString, C::DQuote, S::Start, A::EmitA, T::LIT_STRING);
		SetTrans(S::InString, C::Backslash, S::InStringEscape, A::Acc);
		SetTrans(S::InString, C::LBrace, S::InStringInterp, A::Acc);
		SetTrans(S::InString, C::Newline, S::Start, A::Error);
		SetTrans(S::InString, C::Eof, S::Start, A::Error);
		FillRest(S::InString, S::InString, A::Acc, T::TOK_ERROR,
			{ C::DQuote, C::Backslash, C::LBrace, C::Newline, C::Eof });

		// After backslash: any char (including another backslash) continues string.
		SetTrans(S::InStringEscape, C::Eof, S::Start, A::Error);
		FillRest(S::InStringEscape, S::InString, A::Acc, T::TOK_ERROR, { C::Eof });

		// Inside {identifier[.member]} interpolation: accumulate ident chars and
		// dot; '}' closes the interpolation and returns to InString.
		// The whole interpolated string becomes a single LIT_STRING token;
		// the typer resolves the interpolated identifiers.
		SetTransMany(S::InStringInterp, S::InStringInterp, A::Acc, T::TOK_ERROR,
			C::Alpha, C::X_, C::B_, C::D_, C::E_, C::HexAlpha, C::Zero, C::Digit19);
		SetTrans(S::InStringInterp, C::Dot, S::InStringInterp, A::Acc);
		SetTrans(S::InStringInterp, C::RBrace, S::InString, A::Acc);
		SetTrans(S::InStringInterp, C::Newline, S::Start, A::Error);
		SetTrans(S::InStringInterp, C::Eof, S::Start, A::Error);
		FillRest(S::InStringInterp, S::Start, A::Error, T::TOK_ERROR,
			{ C::Alpha, C::X_, C::B_, C::D_, C::E_, C::HexAlpha,
			  C::Zero, C::Digit19, C::Dot, C::RBrace, C::Newline, C::Eof });

		// -----------------------------------------------------------------------
		// 7.  Character literals
		// -----------------------------------------------------------------------

		// The opening '\'' has already been accumulated.  Accumulate everything
		// up to and including the closing '\''.  The lexer does not enforce that
		// exactly one character (or one escape sequence) appears - that is a
		// parser/typer concern.

		SetTrans(S::InChar, C::SQuote, S::Start, A::EmitA, T::LIT_CHAR);
		SetTrans(S::InChar, C::Backslash, S::InCharEscape, A::Acc);
		SetTrans(S::InChar, C::Newline, S::Start, A::Error);
		SetTrans(S::InChar, C::Eof, S::Start, A::Error);
		FillRest(S::InChar, S::InChar, A::Acc, T::TOK_ERROR,
			{ C::SQuote, C::Backslash, C::Newline, C::Eof });

		SetTrans(S::InCharEscape, C::Eof, S::Start, A::Error);
		FillRest(S::InCharEscape, S::InChar, A::Acc, T::TOK_ERROR, { C::Eof });

		// -----------------------------------------------------------------------
		// 8.  Comments
		// -----------------------------------------------------------------------

		// Line comment: accumulate everything until newline or EOF (don't
		// consume the newline so it is processed in Start for line counting).
		SetTrans(S::InLineComment, C::Newline, S::Start, A::EmitR, T::TRIVIA_LINE_COMMENT);
		SetTrans(S::InLineComment, C::Eof, S::Start, A::EmitR, T::TRIVIA_LINE_COMMENT);
		FillRest(S::InLineComment, S::InLineComment, A::Acc, T::TOK_ERROR,
			{ C::Newline, C::Eof });

		// Block comment: accumulate until '*/' sequence.
		SetTrans(S::InBlockComment, C::Star, S::InBlockCommentStar, A::Acc);
		SetTrans(S::InBlockComment, C::Eof, S::Start, A::Error);
		FillRest(S::InBlockComment, S::InBlockComment, A::Acc, T::TOK_ERROR,
			{ C::Star, C::Eof });

		// After '*' inside block comment:
		//   '/'  -> close the comment (accumulate '/', emit)
		//   '*'  -> stay (another * in sequence)
		//   EOF  -> unterminated
		//   else -> back to InBlockComment
		SetTrans(S::InBlockCommentStar, C::Slash, S::Start, A::EmitA, T::TRIVIA_BLOCK_COMMENT);
		SetTrans(S::InBlockCommentStar, C::Star, S::InBlockCommentStar, A::Acc);
		SetTrans(S::InBlockCommentStar, C::Eof, S::Start, A::Error);
		FillRest(S::InBlockCommentStar, S::InBlockComment, A::Acc, T::TOK_ERROR,
			{ C::Slash, C::Star, C::Eof });

		// -----------------------------------------------------------------------
		// 9.  Multi-character operators
		// -----------------------------------------------------------------------

		// :    -> :=  or bare :
		SetTrans(S::InColon, C::Equals, S::Start, A::EmitA, T::OP_ASSIGN_REF);
		FillRest(S::InColon, S::Start, A::EmitR, T::PUNCT_COLON, { C::Equals });

		// =    -> ==  or bare =
		SetTrans(S::InAssign, C::Equals, S::Start, A::EmitA, T::OP_EQ);
		FillRest(S::InAssign, S::Start, A::EmitR, T::OP_ASSIGN, { C::Equals });

		// !    -> !=  or bare !
		SetTrans(S::InBang, C::Equals, S::Start, A::EmitA, T::OP_NEQ);
		FillRest(S::InBang, S::Start, A::EmitR, T::OP_BANG, { C::Equals });

		// <    -> <<  <=  or bare <
		SetTrans(S::InLt, C::Lt, S::InLtLt, A::Acc);
		SetTrans(S::InLt, C::Equals, S::Start, A::EmitA, T::OP_LTE);
		FillRest(S::InLt, S::Start, A::EmitR, T::OP_LT, { C::Lt, C::Equals });

		// <<   -> <<= or bare <<
		SetTrans(S::InLtLt, C::Equals, S::Start, A::EmitA, T::OP_LSHIFT_ASSIGN);
		FillRest(S::InLtLt, S::Start, A::EmitR, T::OP_LSHIFT, { C::Equals });

		// >    -> >>  >=  or bare >
		SetTrans(S::InGt, C::Gt, S::InGtGt, A::Acc);
		SetTrans(S::InGt, C::Equals, S::Start, A::EmitA, T::OP_GTE);
		FillRest(S::InGt, S::Start, A::EmitR, T::OP_GT, { C::Gt, C::Equals });

		// >>   -> >>= or bare >>
		SetTrans(S::InGtGt, C::Equals, S::Start, A::EmitA, T::OP_RSHIFT_ASSIGN);
		FillRest(S::InGtGt, S::Start, A::EmitR, T::OP_RSHIFT, { C::Equals });

		// &    -> &&  &=  or bare &
		SetTrans(S::InAmpersand, C::Amp, S::Start, A::EmitA, T::OP_AND);
		SetTrans(S::InAmpersand, C::Equals, S::Start, A::EmitA, T::OP_AMP_ASSIGN);
		FillRest(S::InAmpersand, S::Start, A::EmitR, T::OP_AMP, { C::Amp, C::Equals });

		// |    -> ||  |=  or bare |
		SetTrans(S::InPipe, C::Pipe, S::Start, A::EmitA, T::OP_OR);
		SetTrans(S::InPipe, C::Equals, S::Start, A::EmitA, T::OP_PIPE_ASSIGN);
		FillRest(S::InPipe, S::Start, A::EmitR, T::OP_PIPE, { C::Pipe, C::Equals });

		// +    -> ++  +=  or bare +
		SetTrans(S::InPlus, C::Plus, S::Start, A::EmitA, T::OP_PLUSPLUS);
		SetTrans(S::InPlus, C::Equals, S::Start, A::EmitA, T::OP_PLUS_ASSIGN);
		FillRest(S::InPlus, S::Start, A::EmitR, T::OP_PLUS, { C::Plus, C::Equals });

		// *    -> *=  or bare *
		SetTrans(S::InStar, C::Equals, S::Start, A::EmitA, T::OP_STAR_ASSIGN);
		FillRest(S::InStar, S::Start, A::EmitR, T::OP_STAR, { C::Equals });

		// /    -> //  /*  /=  or bare /
		SetTrans(S::InSlash, C::Slash, S::InLineComment, A::Acc);
		SetTrans(S::InSlash, C::Star, S::InBlockComment, A::Acc);
		SetTrans(S::InSlash, C::Equals, S::Start, A::EmitA, T::OP_SLASH_ASSIGN);
		FillRest(S::InSlash, S::Start, A::EmitR, T::OP_SLASH,
			{ C::Slash, C::Star, C::Equals });

		// %    -> %=  or bare %
		SetTrans(S::InPercent, C::Equals, S::Start, A::EmitA, T::OP_PERCENT_ASSIGN);
		FillRest(S::InPercent, S::Start, A::EmitR, T::OP_PERCENT, { C::Equals });

		// ^    -> ^=  or bare ^
		SetTrans(S::InCaret, C::Equals, S::Start, A::EmitA, T::OP_CARET_ASSIGN);
		FillRest(S::InCaret, S::Start, A::EmitR, T::OP_CARET, { C::Equals });

		// -    -> ->  --  -=  or bare -
		SetTrans(S::InMinus, C::Gt, S::Start, A::EmitA, T::OP_ARROW);
		SetTrans(S::InMinus, C::Minus, S::Start, A::EmitA, T::OP_MINUSMINUS);
		SetTrans(S::InMinus, C::Equals, S::Start, A::EmitA, T::OP_MINUS_ASSIGN);
		FillRest(S::InMinus, S::Start, A::EmitR, T::OP_MINUS,
			{ C::Gt, C::Minus, C::Equals });

		// {    -> {?  or bare {
		SetTrans(S::InLBrace, C::Question, S::Start, A::EmitA, T::PUNCT_ERROR_OPEN);
		FillRest(S::InLBrace, S::Start, A::EmitR, T::PUNCT_LBRACE, { C::Question });

		// ?    -> ?}  or error (standalone ? is not a valid xmc operator)
		SetTrans(S::InQuestion, C::RBrace, S::Start, A::EmitA, T::PUNCT_ERROR_CLOSE);
		FillRest(S::InQuestion, S::Start, A::Error, T::TOK_ERROR, { C::RBrace });

		// -----------------------------------------------------------------------
		// 10.  Keyword map
		// -----------------------------------------------------------------------

		s_keywords = {
			// Primitive types
			{ "b",     T::KW_B   },
			{ "i8",    T::KW_I8  }, { "i16",  T::KW_I16  }, { "i32",  T::KW_I32  }, { "i64",  T::KW_I64  },
			{ "u8",    T::KW_U8  }, { "u16",  T::KW_U16  }, { "u32",  T::KW_U32  }, { "u64",  T::KW_U64  },
			{ "f32",   T::KW_F32 }, { "f64",  T::KW_F64  },
			{ "d16",   T::KW_D16 }, { "d32",  T::KW_D32  }, { "d64",  T::KW_D64  },
			{ "c8",    T::KW_C8  }, { "c16",  T::KW_C16  },
			{ "zs8",   T::KW_ZS8 }, { "zs16", T::KW_ZS16 }, { "zs32", T::KW_ZS32 },
			{ "s8",    T::KW_S8  }, { "s16",  T::KW_S16  },
			{ "utf8",  T::KW_UTF8}, { "us",   T::KW_US   },
			// Container types
			{ "list",       T::KW_LIST       }, { "queue",      T::KW_QUEUE      },
			{ "stack",      T::KW_STACK      }, { "btree",      T::KW_BTREE      },
			{ "hashset",    T::KW_HASHSET    }, { "hashmap",    T::KW_HASHMAP    },
			{ "spsc_queue", T::KW_SPSC_QUEUE }, { "spmc_queue", T::KW_SPMC_QUEUE },
			{ "mpsc_queue", T::KW_MPSC_QUEUE }, { "mpmc_queue", T::KW_MPMC_QUEUE },
			// Declaration
			{ "var",       T::KW_VAR       }, { "let",       T::KW_LET       },
			{ "ref",       T::KW_REF       }, { "namespace", T::KW_NAMESPACE },
			{ "struct",    T::KW_STRUCT    }, { "class",     T::KW_CLASS     },
			{ "error",     T::KW_ERROR     }, { "extern",    T::KW_EXTERN    },
			{ "weak",      T::KW_WEAK      }, { "arena",     T::KW_ARENA     },
			{ "heap",      T::KW_HEAP      }, { "function",  T::KW_FUNCTION  },
			// Values / builtins
			{ "null",  T::KW_NULL  }, { "me",    T::KW_ME    }, { "is",    T::KW_IS    },
			{ "true",  T::KW_TRUE  }, { "false", T::KW_FALSE },
			{ "__missing_attribute", T::KW_MISSING_ATTRIBUTE },
			{ "range", T::KW_RANGE },
			// Control flow
			{ "if",     T::KW_IF     }, { "then",   T::KW_THEN   }, { "else",  T::KW_ELSE  },
			{ "for",    T::KW_FOR    }, { "while",  T::KW_WHILE  }, { "do",    T::KW_DO    },
			{ "in",     T::KW_IN     }, { "return", T::KW_RETURN }, { "yield", T::KW_YIELD },
			{ "break",  T::KW_BREAK  }, { "spawn",  T::KW_SPAWN  },
			// Low refinement qualifiers
			{ "immutable",    T::KW_IMMUTABLE    }, { "fixed",       T::KW_FIXED       },
			{ "materialized", T::KW_MATERIALIZED }, { "raw",         T::KW_RAW         },
			{ "homogenous",   T::KW_HOMOGENOUS   }, { "rigid",       T::KW_RIGID       },
			{ "serial",       T::KW_SERIAL       }, { "singular",    T::KW_SINGULAR    },
			// High refinement qualifiers
			{ "mutable",      T::KW_MUTABLE      }, { "resizable",   T::KW_RESIZABLE   },
			{ "virtual",      T::KW_VIRTUAL      }, { "arc",         T::KW_ARC         },
			{ "heterogenous", T::KW_HETEROGENOUS }, { "fluid",       T::KW_FLUID       },
			{ "concurrent",   T::KW_CONCURRENT   }, { "variant",     T::KW_VARIANT     },
		};

		s_tablesBuilt = true;
	}

	// ============================================================================
	// Lexer::TokenTypeName
	// ============================================================================

	const char* Lexer::TokenTypeName(TokenType tt)
	{
		switch (tt) {
		case T::LIT_INTEGER:            return "INTEGER_LITERAL";
		case T::LIT_FLOAT:              return "FLOAT_LITERAL";
		case T::LIT_DECIMAL:            return "DECIMAL_LITERAL";
		case T::LIT_STRING:             return "STRING_LITERAL";
		case T::LIT_CHAR:               return "CHAR_LITERAL";
		case T::IDENTIFIER:             return "IDENTIFIER";
		case T::UGLY_MASK:              return "UGLY_MASK";
			// Primitive types
		case T::KW_B:                   return "KW_B";
		case T::KW_I8:                  return "KW_I8";
		case T::KW_I16:                 return "KW_I16";
		case T::KW_I32:                 return "KW_I32";
		case T::KW_I64:                 return "KW_I64";
		case T::KW_U8:                  return "KW_U8";
		case T::KW_U16:                 return "KW_U16";
		case T::KW_U32:                 return "KW_U32";
		case T::KW_U64:                 return "KW_U64";
		case T::KW_F32:                 return "KW_F32";
		case T::KW_F64:                 return "KW_F64";
		case T::KW_D16:                 return "KW_D16";
		case T::KW_D32:                 return "KW_D32";
		case T::KW_D64:                 return "KW_D64";
		case T::KW_C8:                  return "KW_C8";
		case T::KW_C16:                 return "KW_C16";
		case T::KW_ZS8:                 return "KW_ZS8";
		case T::KW_ZS16:                return "KW_ZS16";
		case T::KW_ZS32:                return "KW_ZS32";
		case T::KW_S8:                  return "KW_S8";
		case T::KW_S16:                 return "KW_S16";
		case T::KW_UTF8:                return "KW_UTF8";
		case T::KW_US:                  return "KW_US";
			// Container types
		case T::KW_LIST:                return "KW_LIST";
		case T::KW_QUEUE:               return "KW_QUEUE";
		case T::KW_STACK:               return "KW_STACK";
		case T::KW_BTREE:               return "KW_BTREE";
		case T::KW_HASHSET:             return "KW_HASHSET";
		case T::KW_HASHMAP:             return "KW_HASHMAP";
		case T::KW_SPSC_QUEUE:          return "KW_SPSC_QUEUE";
		case T::KW_SPMC_QUEUE:          return "KW_SPMC_QUEUE";
		case T::KW_MPSC_QUEUE:          return "KW_MPSC_QUEUE";
		case T::KW_MPMC_QUEUE:          return "KW_MPMC_QUEUE";
			// Declaration
		case T::KW_VAR:                 return "KW_VAR";
		case T::KW_LET:                 return "KW_LET";
		case T::KW_REF:                 return "KW_REF";
		case T::KW_NAMESPACE:           return "KW_NAMESPACE";
		case T::KW_STRUCT:              return "KW_STRUCT";
		case T::KW_CLASS:               return "KW_CLASS";
		case T::KW_ERROR:               return "KW_ERROR";
		case T::KW_EXTERN:              return "KW_EXTERN";
		case T::KW_WEAK:                return "KW_WEAK";
		case T::KW_ARENA:               return "KW_ARENA";
		case T::KW_HEAP:                return "KW_HEAP";
		case T::KW_FUNCTION:            return "KW_FUNCTION";
			// Values
		case T::KW_NULL:                return "KW_NULL";
		case T::KW_ME:                  return "KW_ME";
		case T::KW_IS:                  return "KW_IS";
		case T::KW_TRUE:                return "KW_TRUE";
		case T::KW_FALSE:               return "KW_FALSE";
		case T::KW_MISSING_ATTRIBUTE:   return "KW_MISSING_ATTRIBUTE";
		case T::KW_RANGE:               return "KW_RANGE";
			// Control flow
		case T::KW_IF:                  return "KW_IF";
		case T::KW_THEN:                return "KW_THEN";
		case T::KW_ELSE:                return "KW_ELSE";
		case T::KW_FOR:                 return "KW_FOR";
		case T::KW_WHILE:               return "KW_WHILE";
		case T::KW_DO:                  return "KW_DO";
		case T::KW_IN:                  return "KW_IN";
		case T::KW_RETURN:              return "KW_RETURN";
		case T::KW_YIELD:               return "KW_YIELD";
		case T::KW_BREAK:               return "KW_BREAK";
		case T::KW_SPAWN:               return "KW_SPAWN";
			// Low refinement qualifiers
		case T::KW_IMMUTABLE:           return "KW_IMMUTABLE";
		case T::KW_FIXED:               return "KW_FIXED";
		case T::KW_MATERIALIZED:        return "KW_MATERIALIZED";
		case T::KW_RAW:                 return "KW_RAW";
		case T::KW_HOMOGENOUS:          return "KW_HOMOGENOUS";
		case T::KW_RIGID:               return "KW_RIGID";
		case T::KW_SERIAL:              return "KW_SERIAL";
		case T::KW_SINGULAR:            return "KW_SINGULAR";
			// High refinement qualifiers
		case T::KW_MUTABLE:             return "KW_MUTABLE";
		case T::KW_RESIZABLE:           return "KW_RESIZABLE";
		case T::KW_VIRTUAL:             return "KW_VIRTUAL";
		case T::KW_ARC:                 return "KW_ARC";
		case T::KW_HETEROGENOUS:        return "KW_HETEROGENOUS";
		case T::KW_FLUID:               return "KW_FLUID";
		case T::KW_CONCURRENT:          return "KW_CONCURRENT";
		case T::KW_VARIANT:             return "KW_VARIANT";
			// Assignment
		case T::OP_ASSIGN:              return "OP_ASSIGN";
		case T::OP_ASSIGN_REF:          return "OP_ASSIGN_REF";
			// Arithmetic
		case T::OP_PLUS:                return "OP_PLUS";
		case T::OP_MINUS:               return "OP_MINUS";
		case T::OP_STAR:                return "OP_STAR";
		case T::OP_SLASH:               return "OP_SLASH";
		case T::OP_PERCENT:             return "OP_PERCENT";
			// Compound assignment
		case T::OP_PLUS_ASSIGN:         return "OP_PLUS_ASSIGN";
		case T::OP_MINUS_ASSIGN:        return "OP_MINUS_ASSIGN";
		case T::OP_STAR_ASSIGN:         return "OP_STAR_ASSIGN";
		case T::OP_SLASH_ASSIGN:        return "OP_SLASH_ASSIGN";
		case T::OP_PERCENT_ASSIGN:      return "OP_PERCENT_ASSIGN";
		case T::OP_AMP_ASSIGN:          return "OP_AMP_ASSIGN";
		case T::OP_PIPE_ASSIGN:         return "OP_PIPE_ASSIGN";
		case T::OP_CARET_ASSIGN:        return "OP_CARET_ASSIGN";
		case T::OP_LSHIFT_ASSIGN:       return "OP_LSHIFT_ASSIGN";
		case T::OP_RSHIFT_ASSIGN:       return "OP_RSHIFT_ASSIGN";
			// Comparison / shift
		case T::OP_EQ:                  return "OP_EQ";
		case T::OP_NEQ:                 return "OP_NEQ";
		case T::OP_LT:                  return "OP_LT";
		case T::OP_GT:                  return "OP_GT";
		case T::OP_LTE:                 return "OP_LTE";
		case T::OP_GTE:                 return "OP_GTE";
		case T::OP_LSHIFT:              return "OP_LSHIFT";
		case T::OP_RSHIFT:              return "OP_RSHIFT";
			// Bitwise
		case T::OP_AMP:                 return "OP_AMP";
		case T::OP_PIPE:                return "OP_PIPE";
		case T::OP_CARET:               return "OP_CARET";
		case T::OP_TILDE:               return "OP_TILDE";
			// Logical
		case T::OP_AND:                 return "OP_AND";
		case T::OP_OR:                  return "OP_OR";
		case T::OP_BANG:                return "OP_BANG";
			// Increment / decrement
		case T::OP_PLUSPLUS:            return "OP_PLUSPLUS";
		case T::OP_MINUSMINUS:          return "OP_MINUSMINUS";
			// Pointer / address
		case T::OP_AT:                  return "OP_AT";
		case T::OP_DOLLAR:              return "OP_DOLLAR";
		case T::OP_ARROW:               return "OP_ARROW";
			// Punctuation
		case T::PUNCT_DOT:              return "PUNCT_DOT";
		case T::PUNCT_COMMA:            return "PUNCT_COMMA";
		case T::PUNCT_SEMICOLON:        return "PUNCT_SEMICOLON";
		case T::PUNCT_COLON:            return "PUNCT_COLON";
		case T::PUNCT_LPAREN:           return "PUNCT_LPAREN";
		case T::PUNCT_RPAREN:           return "PUNCT_RPAREN";
		case T::PUNCT_LBRACKET:         return "PUNCT_LBRACKET";
		case T::PUNCT_RBRACKET:         return "PUNCT_RBRACKET";
		case T::PUNCT_LBRACE:           return "PUNCT_LBRACE";
		case T::PUNCT_RBRACE:           return "PUNCT_RBRACE";
		case T::PUNCT_ERROR_OPEN:       return "PUNCT_ERROR_OPEN";
		case T::PUNCT_ERROR_CLOSE:      return "PUNCT_ERROR_CLOSE";
			// Trivia
		case T::TRIVIA_LINE_COMMENT:    return "LINE_COMMENT";
		case T::TRIVIA_BLOCK_COMMENT:   return "BLOCK_COMMENT";
			// Diagnostic
		case T::TOK_ERROR:              return "ERROR";
		case T::TOK_EOF:                return "EOF";
		default:                        return "UNKNOWN";
		}
	}

	// ============================================================================
	// Lexer::Lex
	// ============================================================================

	Lexer::Result Lexer::Lex(
		std::string_view                    filename,
		std::string_view                    source,
		moodycamel::ConcurrentQueue<Token>& parserQueue,
		std::ostream* logStream) const
	{
		InitTables();

		Result result;
		result.tokens.reserve(source.size() / 4);

		const char* src = source.data();
		const uint32_t srcLen = (uint32_t)source.size();

		uint32_t  i = 0;   // current character position
		uint32_t  tokStart = 0;   // source offset of current token's first char
		uint32_t  tokLine = 1;   // line of current token's first char (1-based)
		uint32_t  tokCol = 1;   // column of current token's first char (1-based)
		uint32_t  line = 1;   // current line counter
		uint32_t  lineStart = 0;   // source offset of start of current line
		LexState  state = S::Start;
		bool      hadSpace = false;  // whitespace appeared since last emitted token

		std::string buf;
		buf.reserve(128);

		// Emit one token and optionally write a log line.
		auto EmitToken = [&](T tt, uint32_t start, uint32_t end,
			uint32_t tLine, uint32_t tCol)
			{
				Token tok;
				tok.type = tt;
				tok.line = tLine;
				tok.col = tCol;
				tok.srcStart = start;
				tok.srcLen = end - start;
				tok.flags = hadSpace ? Token::FLAG_PRECEDED_BY_SPACE : 0;
				hadSpace = false;

				result.tokens.push_back(tok);
				if (IsParserToken(tt))
					parserQueue.enqueue(tok);
				if (tt == T::TOK_ERROR)
					result.hadErrors = true;

				if (logStream) {
					// Format: "filename:line:col" padded to 24, then token type
					// padded to 24, then raw source text.
					std::string pos = std::string(filename) + ":"
						+ std::to_string(tLine) + ":"
						+ std::to_string(tCol);
					*logStream
						<< std::left << std::setw(24) << pos
						<< std::setw(24) << TokenTypeName(tt)
						<< source.substr(start, end - start)
						<< '\n';
				}
			};

		// Main DFA loop.  We drive i from 0 to srcLen (inclusive); when i ==
		// srcLen we synthesise a NUL byte which maps to CharClass::Eof.
		while (i <= srcLen) {
			uint8_t   ch = (i < srcLen) ? (uint8_t)src[i] : 0;
			uint8_t   ccr = (ch < 128) ? s_charClass[ch] : (uint8_t)C::Other;
			if (i == srcLen) ccr = (uint8_t)C::Eof;    // force Eof at end of input
			CharClass cc = (CharClass)ccr;

			const Trans& tr = s_trans[(int)state][(int)cc];
			A    act = tr.action;
			S    next = (S)tr.nextState;
			T    tt = tr.tokenType;

			bool advance = true;
			bool resetTokStart = false;
			bool stop = false;

			switch (act) {

				// ---- Acc: accumulate, advance -----------------------------------
			case A::Acc:
				buf += (char)ch;
				break;

				// ---- Skip: whitespace -------------------------------------------
			case A::Skip:
				hadSpace = true;
				resetTokStart = true;
				break;

				// ---- EmitA: accumulate char, emit, advance ----------------------
			case A::EmitA: {
				if (ch != 0) buf += (char)ch;
				uint32_t end = (ch != 0) ? i + 1 : i;
				EmitToken(tt, tokStart, end, tokLine, tokCol);
				buf.clear();
				resetTokStart = true;
				if (tt == T::TOK_EOF) stop = true;
				break;
			}

						 // ---- EmitR: emit buffer, reconsume ------------------------------
			case A::EmitR:
				EmitToken(tt, tokStart, i, tokLine, tokCol);
				buf.clear();
				resetTokStart = true;
				advance = false;
				break;

				// ---- EmitKwR: keyword-check emit, reconsume ---------------------
			case A::EmitKwR: {
				auto it = s_keywords.find(buf);
				T    kwTt = (it != s_keywords.end()) ? it->second : T::IDENTIFIER;
				EmitToken(kwTt, tokStart, i, tokLine, tokCol);
				buf.clear();
				resetTokStart = true;
				advance = false;
				break;
			}

						   // ---- TransR: state change only, reconsume -----------------------
			case A::TransR:
				advance = false;
				break;

				// ---- Error: accumulate, emit TOK_ERROR, advance ----------------
			case A::Error:
				if (ch != 0) buf += (char)ch;
				EmitToken(T::TOK_ERROR, tokStart, ch ? i + 1 : i,
					tokLine, tokCol);
				buf.clear();
				next = S::Start;
				resetTokStart = true;
				if (i >= srcLen) stop = true;
				break;
			}

			state = next;

			if (advance) {
				if (ch == '\n') {
					++line;
					lineStart = i + 1;
				}
				++i;
			}

			if (resetTokStart) {
				tokStart = i;
				tokLine = line;
				tokCol = (i >= lineStart) ? (i - lineStart + 1) : 1;
			}

			if (stop) break;
		}

		// Safety: ensure the queue always ends with TOK_EOF even if the
		// source was empty or the loop exited without emitting one.
		if (result.tokens.empty() || result.tokens.back().type != T::TOK_EOF) {
			EmitToken(T::TOK_EOF, srcLen, srcLen, line,
				srcLen >= lineStart ? (srcLen - lineStart + 1) : 1);
		}

		return result;
	}

} // namespace xmc