// compiler/Morpher.h
//
// Per-file Morpher: pure analysis (Under_The_Hood §6.1).
//
// Two entry points:
//
//   Morph(xmo, nodeQueue, symbols, job)
//       Streaming entry. Runs as a jthread alongside the Lexer and
//       Parser. Drains nodeQueue until the nullptr end-of-stream
//       sentinel and then runs MorphTree on the now-complete parse
//       tree. (The streaming queue is preserved for future use; the
//       current implementation defers all real work to the post-drain
//       MorphTree pass because hello.xm is small enough that the
//       overhead of true streaming morph would dominate.)
//
//   MorphTree(xmo, symbols, job)
//       Whole-tree entry. Walks xmo.parseTree top-down, allocates
//       scope ids and Symbols for declarations, resolves Idents to
//       Symbols, and sets baseType (and rmask, where determinable)
//       on every node.
//
// Per §6.1 the Morpher does not mutate tree shape. Its outputs are
// the baseType / minrmask / maxrmask fields on each node and the
// declaredSymbol back-pointer on declaration / Ident nodes.
//
#pragma once

#include "PipelineQueue.h"
#include "SymbolTable.h"
#include "Xmo.h"
#include "../Compiler.h"
#include "Parser.h"     // ParseTreeNode

namespace xmc
{
	class Morpher
	{
	public:
		static void Morph(
			Xmo& xmo,
			PipelineQueue<ParseTreeNode*>& nodeQueue,
			SymbolTable& symbols,
			const CompileJob& job);

		static void MorphTree(
			Xmo& xmo,
			SymbolTable& symbols,
			const CompileJob& job);
	};

} // namespace xmc