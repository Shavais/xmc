// compiler/Emitter.h
//
// STUB. Two entry points:
//
//   EmitPhase1(xmo, job)
//       Per-Xmo. Runs after Coder. Real implementation will lay out
//       this xmo's code/data sections and resolve in-file relocations.
//
//   EmitPhase2(xmos, job)
//       Whole-job. Real implementation will write the final COFF
//       object file using accumulated phase-1 output from every xmo.
//
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
		static void EmitPhase1(
			Xmo&              xmo,
			const CompileJob& job)
		{
			(void)xmo;
			(void)job;
			// No-op stub.
		}

		static void EmitPhase2(
			const std::vector<std::unique_ptr<Xmo>>& xmos,
			const CompileJob&                        job)
		{
			(void)xmos;
			(void)job;
			// No-op stub.
		}
	};

} // namespace xmc
