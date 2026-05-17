// compiler/Xmo.h
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Arena.h"
#include "SymbolTable.h"
#include "Types.h"

namespace xmc
{
	struct CompileJob;
	struct ParseTreeNode;

	// -----------------------------------------------------------------
	// XmoState
	//
	// Lifecycle of an Xmo slot within one CompileJob.
	//
	//   Empty        -- slot reserved by Compiler, not yet populated.
	//   HeadersOnly  -- exports/scopes/observer graph deserialized from
	//                   disk; parse tree NOT loaded. Incremental build
	//                   default for unmodified files. The .xmo file
	//                   mapping is NOT retained -- LoadFrom unmaps
	//                   before returning. LoadParseTree re-maps if the
	//                   ParseTreeScanRequired recovery path needs the
	//                   parse tree.
	//   Full         -- everything loaded, including parse tree (in
	//                   arena). Used after a ParseTreeScanRequired
	//                   break-out triggers parse-tree load for observer
	//                   xmos.
	//   Modified     -- source was newer than .xmo; parser/morpher are
	//                   producing fresh content this session. The .xm
	//                   file mapping for this xmo is held by the
	//                   per-file pipeline task on its stack, not on the
	//                   Xmo, and is dropped when the task finishes.
	//   Regenerating -- transient state during ReparseRequired recovery.
	//                   Content is being torn down; consumers must not
	//                   read parse tree, code buffer, or symbol fields.
	//   LoadFailed   -- .xmo mapping or deserialization failed. Error
	//                   has been reported to CompileJob; slot is retained
	//                   so the Xmo* is still valid for diagnostics but
	//                   carries no meaningful content.
	// -----------------------------------------------------------------
	enum class XmoState : uint8_t
	{
		Empty,
		HeadersOnly,
		Full,
		Modified,
		Regenerating,
		LoadFailed
	};

	// -----------------------------------------------------------------
	// Dependency graph keys
	//
	// SymbolKey identifies a symbol by its interned name plus its
	// innermost scope id. Both halves are stable within a compile job
	// and serialize as (name text, scope id); on load, the name is
	// re-interned into the new job's SymbolTable.
	//
	// XmoKey identifies an xmo by its interned name (typically the
	// relative path of its source file). Used in observer/imports
	// maps instead of the mutable vector index so the graph survives
	// serialization and xmo-index reshuffles across runs.
	// -----------------------------------------------------------------

	struct XmoKey
	{
		InternedString name;

		bool operator==(const XmoKey& other) const
		{
			return InternedStringEqual{}(name, other.name);
		}
	};

	struct XmoKeyHash
	{
		uint64_t operator()(const XmoKey& k) const { return k.name.hash; }
	};

	// -----------------------------------------------------------------
	// File-format structures (wire layout)
	//
	// These mirror the on-disk xmo layout and are kept together so the
	// serialization/deserialization code has one place to look for the
	// binary contract. Field widths are fixed; do not reorder.
	// -----------------------------------------------------------------
	struct XmoHeader
	{
		uint32_t magic;              // 'xmo\0'
		uint32_t version;
		uint32_t exportCount;
		uint32_t scopeCount;
		uint32_t relocCount;
		uint32_t observerEdgeCount;
		uint32_t importEdgeCount;
		uint64_t codeBufferOffset;
		uint64_t codeBufferSize;
		uint64_t parseTreeOffset;    // 0 if parse tree not persisted
		uint64_t parseTreeSize;
	};

	struct XmoExport
	{
		uint32_t nameOffset;         // offset into string pool
		uint32_t nameLen;
		uint32_t innermostScope;
		uint32_t baseType;
		uint8_t  minrmask;
		uint8_t  maxrmask;
		uint8_t  allocType;
		uint8_t  pathLen;
		uint32_t path[SYMBOL_MAX_DEPTH];
	};

	struct XmoRelocation
	{
		uint32_t offset;             // within code buffer
		uint32_t targetNameOffset;
		uint32_t targetNameLen;
		uint32_t kind;
	};

	struct XmoScope
	{
		uint32_t scopeId;
		uint32_t parentScopeId;
		uint8_t  blockType;
	};

	// -----------------------------------------------------------------
	// Xmo
	//
	// One compiled translation unit. Owned by Compiler via
	// vector<unique_ptr<Xmo>>; self-index stored in `index` for
	// cross-referencing from Symbols (Symbol::xmoIdx).
	//
	// Holds no file mappings: .xm bytes belong to the per-file pipeline
	// task that's compiling this xmo, .xmo bytes belong to whatever
	// load function is currently reading from disk. Both kinds of
	// mapping live on stack frames and are released when the work that
	// needs them finishes. The Xmo itself only owns its parsed/computed
	// state.
	//
	// Not arena-allocated -- the class holds vectors, a map, a string,
	// and an Arena-by-value, all of which need real destruction. The
	// compiler-generated destructor is sufficient; no resource cleanup
	// is required beyond what the member destructors do.
	// -----------------------------------------------------------------
	class Xmo
	{
	public:
		Xmo() = default;

		Xmo(const Xmo&) = delete;
		Xmo& operator=(const Xmo&) = delete;

		// -------------------------------------------------------------
		// Serialization (bodies TODO; file format section not final)
		// -------------------------------------------------------------

		// Writes this xmo to outPath. Returns false on I/O failure,
		// in which case an error has been reported to `job`.
		bool WriteTo(const std::filesystem::path& outPath,
			CompileJob& job) const;

		// Maps inPath, deserializes headers (and optionally the parse
		// tree) into this xmo's owned containers, and unmaps before
		// returning. If loadParseTree is false, the state ends up as
		// HeadersOnly; otherwise Full. Symbol interning uses `symbols`.
		// Returns false on any failure, leaving state == LoadFailed
		// and an error reported to `job`.
		bool LoadFrom(const std::filesystem::path& inPath,
			SymbolTable& symbols,
			CompileJob& job,
			bool         loadParseTree);

		// Loads just the parse tree for an Xmo previously loaded in
		// HeadersOnly state. Re-maps inPath, walks the parse-tree
		// section, rebuilds it into this->arena, and unmaps before
		// returning. Advances state to Full on success. Used by
		// ParseTreeScanRequired recovery.
		bool LoadParseTree(const std::filesystem::path& inPath,
			SymbolTable& symbols,
			CompileJob& job);

		// -------------------------------------------------------------
		// Break-out recovery
		// -------------------------------------------------------------

		// Resets inferred fields on every Symbol this xmo owns
		// (baseType = 0, minrmask = 0, maxrmask = 0xFF). The Symbol
		// objects themselves stay valid; only their inferred contents
		// are cleared. Called by the ReparseRequired handler before
		// re-parsing this xmo from source.
		void ResetSymbolsForRegeneration();

		// Tears down content (parse tree, code buffer, exports, scopes,
		// dependency edges) in preparation for a from-source re-parse.
		// Arena memory is released. `ownedSymbols` is retained because
		// the Symbols themselves persist; ResetSymbolsForRegeneration
		// handles their contents. Sets state = Regenerating.
		void ResetForRegeneration();

		// -------------------------------------------------------------
		// Morpher registration helpers (thread-safe, called from pool tasks)
		// -------------------------------------------------------------

		// Appends sym to ownedSymbols under morphMtx_. Called from
		// DeclareSymbol which may run on any pool worker.
		void PushOwnedSymbol(Symbol* sym)
		{
			std::lock_guard lk(morphMtx_);
			ownedSymbols.push_back(sym);
		}

		// Appends rec to scopeTree under morphMtx_. Called from
		// ApplyNodeRule for scope-opening nodes.
		void PushScopeRecord(const XmoScope& rec)
		{
			std::lock_guard lk(morphMtx_);
			scopeTree.push_back(rec);
		}

		// Dependency graph mutation
		//
		// These are called during Parser/Morpher as references and
		// exports are discovered. All take the per-Xmo shared_mutex
		// as needed; safe to call from any worker thread.
		// -------------------------------------------------------------

		// Records that this xmo imports `symbol` from `originXmoName`.
		void RecordImport(const XmoKey& originXmoName,
			const SymbolKey& symbol);

		// Records that `observer` imports one of our symbols.
		void RecordObserver(const SymbolKey& symbol,
			const XmoKey& observer);

		// Removes all outgoing edges (this xmo's imports from others
		// and entries this xmo contributed to others' observedBy maps).
		// Called during regeneration before re-parsing adds fresh edges.
		// Requires access to the full xmo set to scrub observedBy maps
		// on other xmos -- hence the Compiler& parameter.
		void ScrubDependencyEdges(class Compiler& compiler);

		// -------------------------------------------------------------
		// Public data
		//
		// Kept as public members rather than accessors -- this class is
		// a content container that the pipeline stages read and write
		// directly, and accessors would add noise without adding safety
		// (the concurrency model is "one owning stage at a time, break-
		// out barrier between stages").
		// -------------------------------------------------------------

		// Identity
		std::string name;                    // typically source file path
		InternedString internedName;         // interned into SymbolTable
		uint32_t       index = 0;            // self-index in Compiler::xmos

		// Lifecycle
		std::atomic<XmoState> state{ XmoState::Empty };
		bool                  dirty = false;

		// Content
		Arena                      arena;
		std::vector<uint8_t>       codeBuffer;
		std::vector<XmoExport>     exports;
		std::vector<XmoRelocation> relocs;
		std::vector<XmoScope>      scopeTree;
		ParseTreeNode* parseTree = nullptr; // into arena
		// Emitter Phase 1 → Phase 2 communication.
		// relocTargetNames[i] is the symbol name for relocs[i] (indexed by
		// XmoRelocation::targetNameOffset). kind=0 means external call;
		// kind=1 means .rdata string reference.
		// funcSymbols holds (name, offset-in-codeBuffer) for each FuncDecl
		// emitted; Phase 2 uses this to define COFF public symbols.
		std::vector<std::string>                       relocTargetNames;
		std::vector<std::pair<std::string, uint32_t>>  funcSymbols;

		// Back-reference: symbols declared in this xmo. Non-owning;
		// SymbolTable owns the Symbol objects. Populated by
		// Parser/Morpher as they intern symbols for this xmo. Used by
		// ResetSymbolsForRegeneration and by export-table generation
		// during WriteTo.
		std::vector<Symbol*> ownedSymbols;

		// Dependency graph (per-xmo view).
		//
		// observedBy[sym] -> xmos that import `sym` from us.
		// importsOf[other] -> symbols we import from `other`.
		//
		// Both persist to the .xmo file so an incremental build can
		// determine the observer frontier without loading parse trees.
		std::unordered_map<SymbolKey, std::vector<XmoKey>, SymbolKeyHash> observedBy;
		std::unordered_map<XmoKey, std::unordered_set<SymbolKey, SymbolKeyHash>, XmoKeyHash> importsOf;

	private:
		// Guards ownedSymbols and scopeTree during the Morpher pass.
		// Multiple pool tasks may call DeclareSymbol or push XmoScope
		// records concurrently for the same Xmo.
		mutable std::mutex morphMtx_;

		// Guards dependency-graph mutation. Pipeline stages may call
		// RecordImport/RecordObserver concurrently for the same xmo
		// when resolving cross-xmo references.
		mutable std::shared_mutex depMtx_;
	};

} // namespace xmc