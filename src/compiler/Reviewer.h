// compiler/Reviewer.h
//
// Post-Morpher codegen-prep pass. Called once per Xmo after the Morpher
// has converged and before the Coder runs. Assigns paramSlots to call
// arguments, computes function stack frame sizes (totalStackSize), and
// assigns rsp-relative offsets to local variables (Symbol::offset).
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
			const CompileJob& job);
	};

} // namespace xmc
