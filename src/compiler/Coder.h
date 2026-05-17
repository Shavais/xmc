// compiler/Coder.h
//
// Per-xmo code-block selection pass. Runs after the Reviewer has settled
// all paramSlots, totalStackSize, funcFlags, and Symbol::offsets.
//
// Coder::Code walks the parse tree and populates each node's codeBlocks
// vector with indices into the global CodeBlocks table (CodeBlock.h).
// It does NOT write bytes — that is the Emitter's job.
//
#pragma once

#include "Xmo.h"
#include "../Compiler.h"

namespace xmc
{
    class Coder
    {
    public:
        static void Code(
            Xmo&              xmo,
            const CompileJob& job);
    };

} // namespace xmc
