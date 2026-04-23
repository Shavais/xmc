#include "pch.h"
#include "Emitter.h"

#include <algorithm>
#include <cstdint>
#include <future>
#include <latch>
#include <semaphore>
#include <unordered_map>
#include <vector>

#include "data/AltCodeBlocks.h"
#include "data/EmitterData.h"
#include "data/GlobalData.h"
#include "data/PrimaryCodeBlocks.h"
#include "data/ProjectFileData.h"
#include "data/XmoData.h"

#include "process/Util.h"

#include "tool/BS_thread_pool.hpp"
#include "tool/Coff.h"
#include "tool/Logger.h"

using namespace data;

namespace process
{
	void ProcessNode(data::Xmo* xmo, data::ParseTreeNode* node, std::vector<EmitterFuncContext>& funcStack) {
		if (!node) return;
		using namespace data;

		bool isFunction = (node->funcData != nullptr);

		if (isFunction) {
			xmo->codeBuffer.clear(); // FORCE CLEAR to ensure no ghost data
			xmo->relocs.clear();

			// Record the physical start of the function for the Export table
			xmo->exports[node->funcData->exportIdx].offset = (uint32_t)xmo->codeBuffer.size();

			// Handle Prologue (Automatic Injection)
			if (!node->funcData->isLeaf) {
				const auto* b = &data::PrimaryCodeBlocks[bid.prologue];

				EmitterFuncContext ctx;
				// Capture where the stack immediate byte will live so we can back-patch it
				ctx.prologueImmediatePos = (uint32_t)xmo->codeBuffer.size() + b->patchOffset;
				ctx.finalStackSize = node->funcData->totalStackSize;

				xmo->codeBuffer.insert(xmo->codeBuffer.end(), b->code, b->code + b->codeSize);
				funcStack.push_back(ctx);
			}
		}

		uint32_t symbolIdx = 0;

		// Main Body Emission Loop (Uses raw pointer and explicit codeBlockCount)
		for (uint32_t i = 0; i < node->codeBlockCount; ++i) {
			const data::CodeBlock* b = &data::PrimaryCodeBlocks[node->codeBlocks[i]];

			// Handle Relocations (Linear: matches block to next available symbol)
			if (b->patchOffset != 0xFF) {
				if (symbolIdx < node->patchSymbolCount) {
					// Fetch the interned string name safely
					string symbolName = string(node->patchSymbols[symbolIdx].str, node->patchSymbols[symbolIdx].len);
					symbolIdx++;

					xmo->relocs.push_back({
						symbolName,
						(uint32_t)xmo->codeBuffer.size() + b->patchOffset,
						4 // IMAGE_REL_AMD64_REL32
						});
				}
			}

			// Stamp machine code
			xmo->codeBuffer.insert(xmo->codeBuffer.end(), b->code, b->code + b->codeSize);
		}

		// Recurse through children (Parser-determined tree order)
		for (uint32_t i = 0; i < node->childCount; i++) {
			auto* child = node->children[i];
			ProcessNode(xmo, child, funcStack);
		}

		if (isFunction) {
			if (!node->funcData->isLeaf) {
				auto& ctx = funcStack.back();
				uint8_t stackVal = (uint8_t)ctx.finalStackSize;

				// Back-patch the Prologue's "sub rsp, XX"
				xmo->codeBuffer[ctx.prologueImmediatePos] = stackVal;

				// Handle Epilogue (Automatic Injection)
				const auto* epi = &data::PrimaryCodeBlocks[bid.epilogue];
				uint64_t epiStart = xmo->codeBuffer.size();
				xmo->codeBuffer.insert(xmo->codeBuffer.end(), epi->code, epi->code + epi->codeSize);

				// Patch the "add rsp, XX" and the 'ret' is already in the block
				xmo->codeBuffer[epiStart + epi->patchOffset] = stackVal;

				funcStack.pop_back();
			}
			else {
				// Leaf functions just need a return instruction
				xmo->codeBuffer.push_back(0xC3); // ret
			}

			// Emit Static Data (Strings/Constants) after the function exit
			for (uint32_t i = 0; i < node->staticDataCount; ++i) {
				const auto& sData = node->staticData[i];
				xmo->exports[sData.exportIdx].offset = (uint32_t)xmo->codeBuffer.size();
				xmo->codeBuffer.insert(xmo->codeBuffer.end(), sData.bytes.begin(), sData.bytes.end());
			}
		}
	}

	// Takes code blocks from the xmo parse trees and update the xmo code buffers.
	void UpdateXmoCode() {

		auto xmos = data::Xmos;
		uint8_t maxThreads = data::MaxThreads;

		BS::thread_pool pool(maxThreads);

		for (data::Xmo* xmo : xmos) {
			if (!xmo->dirty_) continue;		// we don't need to update the xmo's of unmodified files if there were no refinments in them and their xm's did not need to be recompiled
			pool.detach_task([xmo]() {
				try {
					uint32_t liveMask = 0;
					std::vector<EmitterFuncContext> funcStack;

					// Trigger the recursive tree walk
					if (xmo != nullptr && xmo->parseTree != nullptr) {
						ProcessNode(xmo, xmo->parseTree, funcStack);
					}
				} 
				catch (...) {};
			});
		}
		pool.wait();
	}

	// Takes code buffers from xmo's and writes to the target coff file for the project
	void WriteToCoff() {
		Coff coff;
		uint16_t textIdx = coff.CreateSection(".text", SectionType::Code);
		std::unordered_map<std::string, uint32_t> globalSymbolMap;

		// 1. Physical Placement
		for (auto* xmo : Xmos) {
			coff.AppendPadding(textIdx, 16);
			xmo->tempGlobalOffset = (uint32_t)coff.GetSectionBufferSize(textIdx);

			// Copy raw machine code and data into the COFF section
			coff.AddRawBytes(textIdx, xmo->codeBuffer);

			// Map ONLY the exports defined by the parser and fixed by the emitter
			for (auto& exp : xmo->exports) {
				uint32_t absoluteOffset = xmo->tempGlobalOffset + exp.offset;
				uint32_t symIdx = coff.DefineSymbol(exp.name, absoluteOffset, textIdx, IMAGE_SYM_CLASS_EXTERNAL);
				globalSymbolMap[exp.name] = symIdx;
			}
		}

		// 2. Relocation Mapping
		for (auto* xmo : Xmos) {
			for (auto& rel : xmo->relocs) {
				uint32_t targetIdx;
				if (globalSymbolMap.count(rel.targetSymbol)) {
					targetIdx = globalSymbolMap[rel.targetSymbol];
				}
				else {
					targetIdx = coff.AddExternalSymbol(rel.targetSymbol);
					globalSymbolMap[rel.targetSymbol] = targetIdx;
				}
				coff.AddRelocation(textIdx, xmo->tempGlobalOffset + rel.offset, targetIdx, rel.type);
			}
		}
		
		coff.WriteTo(joinpath(outdir, outfile));
	}
}