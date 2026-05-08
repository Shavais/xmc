// compiler/SymbolTable.cpp
#include "pch/pch.h"
#include "SymbolTable.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "Arena.h"
#include "parallel_hashmap/phmap.h"

namespace xmc
{
	// ------------------------------------------------------------------
	// Shard-private state. 256 shards indexed by (hash & 0xFF).
	// Each shard owns its own Arena so name bytes and Symbol objects for
	// a given name hash cluster together in contiguous memory.
	// ------------------------------------------------------------------

	struct SymbolEntry
	{
		InternedString       name;
		std::vector<Symbol*> candidates; // pointers into this shard's arena
	};

	struct SymbolShard
	{
		std::shared_mutex mtx;
		phmap::flat_hash_map<InternedString, SymbolEntry, InternedStringHasher, InternedStringEqual> table;
		Arena arena;
	};

	struct SymbolTable::Impl
	{
		SymbolShard           shards[256];
		std::atomic<uint32_t> nextScopeId{ 1 };
		uint32_t              maxDepth = SYMBOL_MAX_DEPTH;
	};

	// ------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------

	// FNV-1a 64-bit. Deterministic (keeps shard assignment reproducible
	// across toolchains), fast, good enough for identifier-length strings.
	static uint64_t HashBytes(const char* data, uint32_t len)
	{
		uint64_t h = 1469598103934665603ull;
		for (uint32_t i = 0; i < len; ++i) {
			h ^= static_cast<uint8_t>(data[i]);
			h *= 1099511628211ull;
		}
		return h;
	}

	// ------------------------------------------------------------------
	// SymbolTable
	// ------------------------------------------------------------------

	SymbolTable::SymbolTable() : m_(std::make_unique<Impl>()) {}
	SymbolTable::~SymbolTable() = default;

	InternedString SymbolTable::InternString(std::string_view text)
	{
		const uint32_t len = static_cast<uint32_t>(text.size());
		const uint64_t hash = HashBytes(text.data(), len);

		SymbolShard& shard = m_->shards[hash & 0xFFu];
		const InternedString probe{ text.data(), len, hash };

		// Fast path: shared lock, probe existing entry.
		{
			std::shared_lock rl(shard.mtx);
			auto it = shard.table.find(probe);
			if (it != shard.table.end()) return it->first;
		}

		// Slow path: copy bytes into the shard's arena under the write lock.
		std::unique_lock wl(shard.mtx);

		auto it = shard.table.find(probe);
		if (it != shard.table.end()) return it->first;  // lost the race

		char* storage = static_cast<char*>(shard.arena.Allocate(len + 1));
		std::memcpy(storage, text.data(), len);
		storage[len] = '\0';

		const InternedString interned{ storage, len, hash };
		SymbolEntry entry;
		entry.name = interned;
		shard.table.emplace(interned, std::move(entry));

		return interned;
	}

	Symbol* SymbolTable::InternSymbol(std::string_view name, const uint32_t* namespacePath, uint32_t pathLen)
	{
		if (pathLen > m_->maxDepth) return nullptr;

		const InternedString iname = InternString(name);
		SymbolShard& shard = m_->shards[iname.hash & 0xFFu];

		std::unique_lock wl(shard.mtx);

		void* mem = shard.arena.Allocate(sizeof(Symbol));
		Symbol* sym = new (mem) Symbol();
		sym->name = iname;
		sym->pathLen = static_cast<uint8_t>(pathLen);

		if (pathLen <= SYMBOL_INLINE_DEPTH) {
			for (uint32_t i = 0; i < pathLen; ++i) sym->inline_path[i] = namespacePath[i];
		}
		else {
			// Overflow path lives beside the Symbol in the same shard arena.
			// Trivially destructible; dies when the SymbolTable dies.
			uint32_t* overflow = static_cast<uint32_t*>(
				shard.arena.Allocate(pathLen * sizeof(uint32_t)));
			for (uint32_t i = 0; i < pathLen; ++i) overflow[i] = namespacePath[i];
			sym->overflow_path = overflow;
		}

		SymbolEntry& entry = shard.table[iname];
		if (entry.name.str == nullptr) entry.name = iname;
		entry.candidates.push_back(sym);

		return sym;
	}

	Symbol* SymbolTable::ResolveSymbol(std::string_view name, const uint32_t* callerPath, uint32_t callerPathLen)
	{
		const uint32_t len = static_cast<uint32_t>(name.size());
		const uint64_t hash = HashBytes(name.data(), len);

		SymbolShard& shard = m_->shards[hash & 0xFFu];
		const InternedString probe{ name.data(), len, hash };

		std::shared_lock rl(shard.mtx);

		auto it = shard.table.find(probe);
		if (it == shard.table.end()) return nullptr;

		const SymbolEntry& entry = it->second;
		if (entry.candidates.empty()) return nullptr;

		// Starter rule: longest namespacePath that is a prefix of callerPath
		// wins. Two candidates tied at the same depth are ambiguous.
		// TODO: visibility modifiers, using-directives, ADL for functions,
		// refinement-aware selection.
		Symbol* best = nullptr;
		uint32_t bestDepth = 0;
		bool     ambiguous = false;

		for (Symbol* s : entry.candidates) {
			if (s->pathLen > callerPathLen) continue;

			const uint32_t* p = s->Path();
			bool isPrefix = true;
			for (uint32_t i = 0; i < s->pathLen; ++i) {
				if (p[i] != callerPath[i]) { isPrefix = false; break; }
			}
			if (!isPrefix) continue;

			const uint32_t depth = s->pathLen;
			if (depth > bestDepth) {
				best = s;
				bestDepth = depth;
				ambiguous = false;
			}
			else if (depth == bestDepth && s != best) {
				ambiguous = true;
			}
		}

		return ambiguous ? nullptr : best;
	}

	uint32_t SymbolTable::AllocScopeId()
	{
		return m_->nextScopeId.fetch_add(1, std::memory_order_relaxed);
	}

} // namespace xmc