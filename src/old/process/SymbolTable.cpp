// process/SymbolTable.cpp
#pragma once
#include "pch.h"
#include "data/ParserData.h"

using namespace data;

namespace process
{
	// Interns a raw C string into SymbolArena.
	// Hash and length are computed once here and cached in the returned InternedString.
	// Uses wyhash for fast hashing over the string bytes.
	InternedString InternString(const char* str) {
		uint32_t len = (uint32_t)strlen(str);

		char* permanent = (char*)SymbolArena.Allocate(len + 1);
		memcpy(permanent, str, len + 1);

		// FNV-1a over the string bytes — simple and reliable.
		// Replace with wyhash here if profiling shows this is hot.
		uint64_t h = 14695981039346656037ULL;
		for (uint32_t i = 0; i < len; ++i) {
			h ^= (uint8_t)str[i];
			h *= 1099511628211ULL;
		}

		return InternedString{ permanent, len, h };
	}


	// Registers a symbol definition encountered during parsing.
	Symbol* InternSymbol(const char* tempName, std::vector<uint32_t> namespacePath) {
		// Compute the hash from the raw string to select the shard.
		// We do this before interning so we don't allocate if the symbol already exists.
		uint64_t h = 14695981039346656037ULL;
		for (const char* p = tempName; *p; ++p) {
			h ^= (uint8_t)*p;
			h *= 1099511628211ULL;
		}
		uint32_t len = (uint32_t)strlen(tempName);
		InternedString tempKey{ tempName, len, h };

		auto& shard = SymbolTable[h >> 56];

		std::unique_lock lock(shard.shared_mutex);

		auto it = shard.table.find(tempKey);

		if (it == shard.table.end()) {
			// First time we've seen this name — intern it and create the entry
			InternedString interned = InternString(tempName);

			Symbol* symbol = SymbolArena.Construct<Symbol>();
			symbol->name = interned;
			symbol->namespacePath = std::move(namespacePath);

			shard.table[interned] = SymbolEntry{ interned, { symbol } };
			return symbol;
		}

		// Name exists — check if this exact namespace path is already registered.
		// Handles re-entrant parsing or duplicate declarations.
		auto& entry = it->second;
		for (Symbol* candidate : entry.candidates) {
			if (candidate->namespacePath == namespacePath) {
				return candidate;
			}
		}

		// Same name, different namespace — add as a new candidate.
		// Reuse the already-interned string from the existing entry.
		Symbol* symbol = SymbolArena.Construct<Symbol>();
		symbol->name = entry.name;
		symbol->namespacePath = std::move(namespacePath);

		entry.candidates.push_back(symbol);
		return symbol;
	}


	// Resolves a symbol reference from a given call site.
	// Finds the candidate whose namespacePath is the longest prefix of callerPath
	// (i.e. the most deeply nested enclosing namespace that defines this name).
	Symbol* ResolveSymbol(const char* name, const std::vector<uint32_t>& callerPath) {
		uint64_t h = 14695981039346656037ULL;
		for (const char* p = name; *p; ++p) {
			h ^= (uint8_t)*p;
			h *= 1099511628211ULL;
		}
		uint32_t len = (uint32_t)strlen(name);
		InternedString key{ name, len, h };

		auto& shard = SymbolTable[h >> 56];

		std::shared_lock lock(shard.shared_mutex);

		auto it = shard.table.find(key);
		if (it == shard.table.end()) return nullptr;

		const auto& candidates = it->second.candidates;

		Symbol* bestMatch = nullptr;
		int     bestDepth = -1;
		bool    ambiguous = false;

		for (Symbol* candidate : candidates) {
			const auto& symPath = candidate->namespacePath;

			// The symbol is visible from the call site only if its full namespace path
			// is a prefix of the caller's path — meaning the caller is in the same
			// namespace or a child of it.
			uint32_t limit = (uint32_t)std::min(callerPath.size(), symPath.size());
			uint32_t sharedDepth = 0;
			while (sharedDepth < limit && callerPath[sharedDepth] == symPath[sharedDepth]) {
				++sharedDepth;
			}

			if (sharedDepth < (uint32_t)symPath.size()) continue; // not a prefix match

			if ((int)sharedDepth > bestDepth) {
				bestDepth = (int)sharedDepth;
				bestMatch = candidate;
				ambiguous = false;
			}
			else if ((int)sharedDepth == bestDepth) {
				ambiguous = true;
			}
		}

		if (ambiguous) {
			// TODO: surface a proper compiler diagnostic
			return nullptr;
		}

		return bestMatch;
	}

	void ConsumeLength(TextParser& p, size_t length) {
		p.Consume(length);
	}

}
