#include "pch.h"
#include "Parser.h"

#include "data/ParserData.h"

using namespace data;


namespace process
{
	// symbol look up / instantiation 
	Symbol* GetOrCreateSymbol(const char* tempName, uint32_t scopeId) {
		SymbolKey key(tempName, scopeId);
		size_t h = SymbolKeyHasher{}(key);
		auto& shard = SymbolTable[h >> 56];

		// Read lookup
		{
			std::shared_lock lock(shard.shared_mutex);
			auto it = shard.table.find(key);
			if (it != shard.table.end()) return it->second.symbol;
		}

		// Write path
		std::unique_lock lock(shard.shared_mutex);
		auto it = shard.table.find(key);
		if (it != shard.table.end()) return it->second.symbol;

		// --- SYMBOL IS NEW ---

		// 1. Permanently intern the string into your Arena
		size_t nameLen = strlen(tempName) + 1;
		char* permanentName = (char*)SymbolArena.Allocate(nameLen);
		memcpy(permanentName, tempName, nameLen);

		// 2. Allocate the symbol
		Symbol* symbol = (Symbol*)SymbolArena.Allocate(sizeof(Symbol));
		new (symbol) Symbol();

		symbol->name = permanentName;
		symbol->scopeId = scopeId;

		// 3. Insert using the permanent pointer as the key
		shard.table[{permanentName, scopeId}] = { permanentName, symbol };

		return symbol;
	}


}