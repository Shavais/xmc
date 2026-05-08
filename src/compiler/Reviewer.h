// compiler/Reviewer.h
//
// STUB. Called once per Xmo after the Lexer/Parser/Morpher pipeline
// converges and before Coder runs. Real implementation will scan
// xmo.ownedSymbols for BaseTypeIds::Unresolved and emit warnings;
// errors are emitted synchronously by the Morpher itself.
//
#pragma once

#include "SymbolTable.h"
#include "Xmo.h"
#include "../Compiler.h"

namespace xmc
{
	class Reviewer
	{
	public:
		static void Review(
			Xmo&              xmo,
			SymbolTable&      symbols,
			const CompileJob& job)
		{
			(void)xmo;
			(void)symbols;
			(void)job;
			// No-op stub.
		}
	};

} // namespace xmc
