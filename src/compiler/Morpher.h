// compiler/Morpher.h
//
// Per-file Morpher: pure semantic analysis.
//
// The Morpher is invoked by the Parser, not by the Compiler. Two call sites:
//
//   MorphNoun(node, xmo, symbols, job)
//       Called (as a thread-pool fire-and-forget task) when the Parser
//       completes a noun node. Resolves the noun's type, looks up its
//       symbol, and sets baseType/rmask. The Parser must not touch
//       baseType, minrmask, maxrmask, or declaredSymbol after this call
//       is submitted.
//
//   MorphTree(xmo, symbols, job)
//       Whole-tree pass. Walks xmo.parseTree top-down after the Parser
//       returns. Used for declaration hoisting, cross-reference resolution,
//       and any analysis that requires the full tree.
//
// Threading contract:
//   The Parser sets the structural fields (kind, parent, children, srcStart,
//   srcLen, line, col, name, qualifier, regHint, intValue, floatValue,
//   pointerDepth, isArray) before submitting the task. Those fields are
//   read-only from the Morpher's perspective. The Morpher writes only
//   baseType, minrmask, maxrmask, and declaredSymbol.
//
#pragma once

#include "SymbolTable.h"
#include "Xmo.h"
#include "../Compiler.h"
#include "Parser.h"     // ParseTreeNode

namespace xmc
{
    class Morpher
    {
    public:
        // Called by the Parser (as a pool task) when a noun node is complete.
        static void MorphNoun(
            ParseTreeNode*  node,
            Xmo&            xmo,
            SymbolTable&    symbols,
            const CompileJob& job);

        // Called by the Parser after the full tree is built.
        static void MorphTree(
            Xmo&            xmo,
            SymbolTable&    symbols,
            const CompileJob& job);
    };

} // namespace xmc
