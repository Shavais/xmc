// compiler/Morpher.h
//
// Per-file Morpher: pure semantic analysis.
//
// MorphNoun(node, xmo, symbols, job)
//   Applies one node's typing rule, then propagates upward through parent
//   pointers. At each ancestor, morphed_children is atomically incremented;
//   if the ancestor is also parser_complete and this thread wins the morphed
//   test-and-set, it applies that ancestor's rule inline and continues up.
//   Called as a pool task for every leaf node, submitted by the Parser during
//   Drive(). Interior nodes are processed inline by whoever wins their claim —
//   either the last leaf's morpher task or the parser thread itself.
//
// MorphTree(xmo, symbols, job)
//   Called by the Compiler after all file parser tasks and leaf morpher tasks
//   have completed (pool drained). Writes the MorpherLog if requested by the
//   project file. Does not process any nodes — morphing is done by MorphNoun.
//
// Threading contract:
//   The Parser sets structural fields (kind, parent, children, childCount,
//   srcStart, srcLen, line, col, name, qualifier, regHint, intValue,
//   floatValue, pointerDepth, isArray, scopeId) before the node's
//   parser_complete flag is stored true. The Morpher observes parser_complete
//   with seq_cst before reading childCount or children, so those writes are
//   guaranteed visible. The Morpher writes baseType, minrmask, maxrmask,
//   declaredSymbol, morphed_children, morphed, and parser_complete is
//   written only by the Parser.
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
        // Apply one leaf node's typing rule and propagate upward.
        // pool is needed to submit subscriber callbacks as tasks when a
        // symbol declaration drains its waiters list.
        static void MorphNoun(
            ParseTreeNode*     node,
            Xmo&               xmo,
            SymbolTable&       symbols,
            const CompileJob&  job,
            BS::thread_pool<>& pool);

        // Write the morpher log for xmo (if job.MorpherLog is set).
        // Call only after the pool has been fully drained so all MorphNoun
        // tasks have completed and node types are final.
        static void MorphTree(
            Xmo&              xmo,
            SymbolTable&      symbols,
            const CompileJob& job);
    };

} // namespace xmc
