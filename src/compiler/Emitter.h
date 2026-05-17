// compiler/Emitter.h
#pragma once

#include <memory>
#include <vector>

#include "Xmo.h"
#include "../Compiler.h"

namespace xmc
{
	class Emitter
	{
	public:
		// Per-Xmo pass. Walks the parse tree, emits code bytes into
		// xmo.codeBuffer, and accumulates relocations into xmo.relocs /
		// xmo.relocTargetNames. Also records function symbols for Phase 2.
		static void EmitPhase1(Xmo& xmo, const CompileJob& job);

		// Whole-job pass. Assembles all xmos' code buffers, string-literal
		// data, and relocation records into a single COFF .obj file.
		static void EmitPhase2(
			const std::vector<std::unique_ptr<Xmo>>& xmos,
			const CompileJob&                        job);
	};

} // namespace xmc
