// compiler/Xmo.cpp
#include "pch/pch.h"
#include "Xmo.h"

#include <mutex>
#include <shared_mutex>

#include "Compiler.h"
#include "tool/FileMapping.h"
#include "tool/Logger.h"

namespace xmc
{
	// -------------------------------------------------------------------------
	// Serialization (TODO bodies; file format not yet finalized)
	//
	// The on-disk layout we are working toward:
	//
	//   [XmoHeader]
	//   [string pool]                  -- all interned names referenced below
	//   [XmoExport array]              -- exportCount entries
	//   [XmoScope array]               -- scopeCount entries
	//   [observer edges]               -- observerEdgeCount records:
	//                                       (symKeyNameOff, symKeyNameLen,
	//                                        innermostScope, observerNameOff,
	//                                        observerNameLen)
	//   [imports edges]                -- importEdgeCount records:
	//                                       (originNameOff, originNameLen,
	//                                        symKeyNameOff, symKeyNameLen,
	//                                        innermostScope)
	//   [code buffer]                  -- codeBufferSize bytes
	//   [XmoRelocation array]          -- relocCount entries
	//   [parse tree]                   -- present only if parseTreeSize > 0
	//
	// All offsets are absolute within the file; counts come from the header.
	//
	// Mapping discipline: LoadFrom and LoadParseTree each map their input
	// file via a stack-local FileMapping, copy/intern everything they need
	// into Xmo's owned containers (and into Xmo::arena for the parse tree),
	// and unmap on return via FileMapping's destructor. No xmo retains a
	// mapping past the call that established it. The page-cache benefits
	// of mmap (sparse access, no user-space copy of bytes we never touch,
	// OS-managed eviction across the COFF assembly walk) accrue per-call;
	// they don't require holding the mapping open across stages.
	// -------------------------------------------------------------------------

	bool Xmo::WriteTo(const std::filesystem::path& outPath, CompileJob& job) const
	{
		// TODO: implement per the layout above.
		//  - Build the string pool from `name`, `ownedSymbols` names,
		//    relocation target names, and dep-graph keys.
		//  - Emit XmoHeader with computed offsets/counts.
		//  - Emit each section. For exports, source per-symbol fields from
		//    `ownedSymbols` (current baseType / refinement masks / path).
		//  - Optionally emit the parse tree; set parseTreeSize=0 if omitted.
		(void)outPath;
		(void)job;
		return true;
	}

	bool Xmo::LoadFrom(const std::filesystem::path& inPath,
		SymbolTable& symbols,
		CompileJob& job,
		bool                         loadParseTree)
	{
		// Stack-local mapping. Released by ~FileMapping when this function
		// returns, regardless of whether we succeed, fail, or throw. The
		// bytes we deserialize get copied/interned into Xmo's owned
		// containers (or, for the parse tree, rebuilt into this->arena),
		// so nothing in this object refers to the mapped pages after we
		// return.
		FileMapping mapping;
		if (!mapping.Map(inPath)) {
			// FileMapping::Map already emitted a diagnostic to oserror.
			state.store(XmoState::LoadFailed, std::memory_order_relaxed);
			job.ErrorOccurred.store(true, std::memory_order_relaxed);
			return false;
		}

		// TODO: implement.
		//  - Validate XmoHeader.magic and version against `mapping`.
		//  - Walk the string pool; pre-intern every referenced name into
		//    `symbols` so subsequent records can resolve them in O(1).
		//  - Deserialize exports into Symbol objects via
		//    SymbolTable::InternSymbol; push each into `ownedSymbols` and
		//    set Symbol::xmoIdx = this->index.
		//  - Deserialize scopes, observer edges, imports edges.
		//  - Copy the code buffer into this->codeBuffer.
		//  - Deserialize the relocation table into this->relocs.
		//  - If loadParseTree, walk the parse-tree section and rebuild it
		//    into `arena`; otherwise leave parseTree == nullptr.
		//  - Set state to Full or HeadersOnly accordingly and return true.
		//
		// On any failure: set state = LoadFailed, set job.ErrorOccurred,
		// emit a diagnostic via oserror, and return false. The mapping
		// is released either way.
		(void)symbols;
		(void)loadParseTree;

		oserror << "Xmo::LoadFrom not yet implemented for " << inPath.string()
			<< std::endl;
		state.store(XmoState::LoadFailed, std::memory_order_relaxed);
		job.ErrorOccurred.store(true, std::memory_order_relaxed);
		return false;
	}

	bool Xmo::LoadParseTree(const std::filesystem::path& inPath,
		SymbolTable& symbols,
		CompileJob& job)
	{
		// Same stack-local-mapping discipline as LoadFrom. We accept the
		// .xmo path as a parameter because Xmo no longer carries a
		// persistent mapping; the caller (Compiler) knows IntDir and the
		// xmo's stem and can supply the right path.
		FileMapping mapping;
		if (!mapping.Map(inPath)) {
			state.store(XmoState::LoadFailed, std::memory_order_relaxed);
			job.ErrorOccurred.store(true, std::memory_order_relaxed);
			return false;
		}

		// TODO: implement.
		//  - Validate header.
		//  - Locate parseTreeOffset / parseTreeSize within `mapping`.
		//  - Walk the wire-format tree and rebuild it in `arena`,
		//    re-resolving any symbol references via `symbols`.
		//  - On success, set parseTree and advance state from
		//    HeadersOnly to Full.
		(void)symbols;
		(void)job;
		return false;
	}

	// -------------------------------------------------------------------------
	// Break-out recovery
	// -------------------------------------------------------------------------

	void Xmo::ResetSymbolsForRegeneration()
	{
		// Reset only the inferred fields. Symbol identity (name, path,
		// xmoIdx) and storage location are preserved -- the same Symbol
		// pointers remain valid for any holders elsewhere in the pipeline.
		for (Symbol* s : ownedSymbols) {
			s->baseType.store(0, std::memory_order_relaxed);
			s->minrmask.store(0, std::memory_order_relaxed);
			s->maxrmask.store(0xFF, std::memory_order_relaxed);
		}
	}

	void Xmo::ResetForRegeneration()
	{
		// Tear down content. ownedSymbols is intentionally retained --
		// ResetSymbolsForRegeneration clears their inferred state, and the
		// re-parse will re-discover them via SymbolTable::InternSymbol
		// (which must be idempotent for the regeneration path).
		parseTree = nullptr;
		tempGlobalOffset = 0;

		codeBuffer.clear();
		codeBuffer.shrink_to_fit();
		exports.clear();
		exports.shrink_to_fit();
		relocs.clear();
		relocs.shrink_to_fit();
		scopeTree.clear();
		scopeTree.shrink_to_fit();

		// Dep-graph edges built from the prior parse are stale; the
		// re-parse will rebuild them via RecordImport / RecordObserver.
		{
			std::unique_lock lk(depMtx_);
			observedBy.clear();
			importsOf.clear();
		}

		dirty = true;
		state.store(XmoState::Regenerating, std::memory_order_release);

		// TODO: arena.Reset().
		// Arena currently has no Reset() method, so the previous parse
		// tree's memory remains allocated until the Xmo itself is
		// destroyed at end of job. This is a bounded leak (at most one
		// arena's worth per regeneration per file), and regeneration is
		// rare. Add Arena::Reset() and call it here when convenient.
	}

	// -------------------------------------------------------------------------
	// Dependency graph mutation
	// -------------------------------------------------------------------------

	void Xmo::RecordImport(const XmoKey& originXmoName, const SymbolKey& symbol)
	{
		std::unique_lock lk(depMtx_);
		// unordered_set handles dedup; repeated references to the same
		// imported symbol from this xmo collapse to a single edge.
		importsOf[originXmoName].insert(symbol);
	}

	void Xmo::RecordObserver(const SymbolKey& symbol, const XmoKey& observer)
	{
		std::unique_lock lk(depMtx_);

		// observedBy values are vectors (small, cache-friendly, serializes
		// sequentially). Dedup with a linear scan -- typical observer
		// counts are in the low tens, so linear is cheaper than a set.
		auto& observers = observedBy[symbol];
		for (const auto& existing : observers) {
			if (InternedStringEqual{}(existing.name, observer.name)) return;
		}
		observers.push_back(observer);
	}

	void Xmo::ScrubDependencyEdges(Compiler& compiler)
	{
		// TODO: implement once Compiler exposes its xmos vector and a
		// name -> index map (or a Compiler::FindXmo(XmoKey) accessor).
		//
		// Algorithm:
		//   1. For each (originName, symbolSet) in importsOf:
		//        Xmo* origin = compiler.FindXmo(originName);
		//        if (!origin) continue;
		//        std::unique_lock olk(origin->depMtx_);
		//        for (const SymbolKey& sk : symbolSet) {
		//            auto it = origin->observedBy.find(sk);
		//            if (it == origin->observedBy.end()) continue;
		//            auto& vec = it->second;
		//            // Erase any entry whose name matches this xmo.
		//            std::erase_if(vec, [this](const XmoKey& k) {
		//                return InternedStringEqual{}(k.name, this->internedName);
		//            });
		//            if (vec.empty()) origin->observedBy.erase(it);
		//        }
		//   2. After scrubbing all origins:
		//        std::unique_lock lk(depMtx_);
		//        importsOf.clear();
		//        observedBy.clear();   // we own these; a fresh parse refills them
		(void)compiler;
	}

} // namespace xmc