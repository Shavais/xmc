// compiler/Morpher.h
//
// Per-file Morpher: pure semantic analysis.
//
// The Morpher is invoked by the Parser. Two call sites:
//
//   MorphNoun(node, xmo, symbols, job)
//       Applies the node's typing rule, then propagates upward through
//       parent pointers, applying each ancestor's rule once all of its
//       children have resolved (atomic pendingChildren decrement).
//       Called sequentially by MorphTree for each leaf in the leaves
//       vector collected during Drive().
//
//   MorphTree(xmo, symbols, job, leaves)
//       Called by the Parser after Drive() completes and all pendingChildren
//       have been initialised by popNode(). Processes each leaf in order —
//       safe because all parent pendingChildren are now correctly set.
//       Writes the MorpherLog if requested by the project file.
//
// Threading contract:
//   The Parser sets the structural fields (kind, parent, children, srcStart,
//   srcLen, line, col, name, qualifier, regHint, intValue, floatValue,
//   pointerDepth, isArray, scopeId, pendingChildren) before calling MorphTree.
//   Those fields are read-only from the Morpher's perspective. The Morpher
//   writes only baseType, minrmask, maxrmask, and declaredSymbol.
//
//   pendingChildren is std::atomic<uint16_t>; the machinery is in place for
//   a future concurrent variant. In the current sequential implementation only
//   one thread touches it at a time, so the atomicity is unused overhead (but
//   the cost is negligible and the code is already correct for the concurrent
//   variant when the subscriber mechanism described in §2c is implemented).
//
#pragma once

#include <vector>

#include "SymbolTable.h"
#include "Xmo.h"
#include "../Compiler.h"
#include "Parser.h"     // ParseTreeNode

namespace xmc
{
    class Morpher
    {
    public:
        // Apply one leaf node's typing rule and propagate upward.
        static void MorphNoun(
            ParseTreeNode*    node,
            Xmo&              xmo,
            SymbolTable&      symbols,
            const CompileJob& job);

        // Process all leaves collected by the Parser, then write the morpher log.
        static void MorphTree(
            Xmo&                                xmo,
            SymbolTable&                        symbols,
            const CompileJob&                   job,
            const std::vector<ParseTreeNode*>&  leaves);
    };

} // namespace xmc
