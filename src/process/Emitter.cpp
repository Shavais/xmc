#include "pch.h"
#include "Emitter.h"

#include <unordered_map>
#include <cstdint>
#include <future>
#include <vector>
#include <algorithm>


#include "data/AltCodeBlocks.h"
#include "tool/Coff.h"
#include "data/EmitterData.h"
#include "data/PrimaryCodeBlocks.h"
#include "data/XmoData.h"

namespace process
{
	void ProcessNode(data::Xmo* xmo, data::ParseTreeNode* node, uint32_t& liveMask, std::vector<data::EmitterFuncContext>& funcStack) {
		if (!node) return;

		// 1. Handle Function Entry (Prologue)
		bool isFunction = (node->funcData != nullptr);
		if (isFunction) {
			data::EmitterFuncContext ctx;
			ctx.localVars = node->funcData->localSize;
			ctx.currentSpills = 0;
			ctx.peakSpills = 0;

			// Emit Prologue with 0x00 placeholder
			const auto* b = &data::PrimaryCodeBlocks[(uint16_t)data::BlockIds::prologue];
			ctx.prologueImmediatePos = xmo->codeBuffer.size() + b->patchOffset;
			xmo->codeBuffer.insert(xmo->codeBuffer.end(), b->code, b->code + b->codeSize);

			funcStack.push_back(ctx);
		}

		// 2. Emit Blocks for this Node
		for (size_t i = 0; i < node->blockId.size(); ++i) {
			const data::CodeBlock* b = &data::PrimaryCodeBlocks[node->blockId[i]];

			// Register Conflict Resolution
			while ((b->reserveMask & liveMask) != 0) {
				if (b->altTableIndex == 0) {
					// Hit terminal Spill-to-Stack block
					if (!funcStack.empty()) {
						auto& ctx = funcStack.back();
						ctx.currentSpills++; // Logically "push"
						if (ctx.currentSpills > ctx.peakSpills) ctx.peakSpills = ctx.currentSpills;

						// Note: currentSpills-- happens after emission 
						// because the block cleans up its own stack (Push->Code->Pop)
					}
					break;
				}
				b = &data::AltCodeBlocks[b->altTableIndex];
			}

			// Handle Relocations (Patches)
			if (b->patchOffset != 0xFF) {
				data::XmoRelocation rel;
				rel.offset = (uint32_t)xmo->codeBuffer.size() + b->patchOffset;
				rel.type = IMAGE_REL_AMD64_REL32;
				rel.targetSymbol = node->patchSymbol[i];
				xmo->relocs.push_back(rel);
			}

			// Stitch Machine Code
			xmo->codeBuffer.insert(xmo->codeBuffer.end(), b->code, b->code + b->codeSize);

			// Update masks and cleanup spill tracking
			if (b->altTableIndex == 0 && !funcStack.empty()) {
				funcStack.back().currentSpills--; // Logically "pop"
			}
			liveMask |= b->reserveMask;
			liveMask &= ~b->releaseMask;
		}

		// 3. Recurse through children
		for (auto* child : node->children) {
			ProcessNode(xmo, child, liveMask, funcStack);
		}

		// 4. Handle Function Exit (Epilogue + Back-patch)
		if (isFunction) {
			auto& ctx = funcStack.back();

			// (Shadow 32) + Locals + (Peak * 8), aligned to 16
			uint32_t finalSize = (32 + ctx.localVars + (ctx.peakSpills * 8) + 15) & ~15;

			// Back-patch Prologue
			xmo->codeBuffer[ctx.prologueImmediatePos] = (uint8_t)finalSize;

			// Emit Epilogue
			const auto* epi = &data::PrimaryCodeBlocks[(uint16_t)data::BlockIds::epilogue];
			size_t epiStart = xmo->codeBuffer.size();
			xmo->codeBuffer.insert(xmo->codeBuffer.end(), epi->code, epi->code + epi->codeSize);
			xmo->codeBuffer[epiStart + epi->patchOffset] = (uint8_t)finalSize;

			funcStack.pop_back();
		}
	}

	void UpdateXmoCode(std::vector<data::Xmo*>& xmos, uint32_t maxThreads) 
	{
		std::vector<std::future<void>> workers;

		for (data::Xmo* xmo : xmos) 
		{
			if (workers.size() >= maxThreads) 
			{
				workers.front().get();
				workers.erase(workers.begin());
			}

			workers.push_back(std::async(std::launch::async, 
				[xmo]() 
				{
					uint32_t liveMask = 0;
					std::vector<data::EmitterFuncContext> funcStack;

					if (xmo->parseTree != nullptr) {
						ProcessNode(xmo, xmo->parseTree, liveMask, funcStack);
					}
				}
			));
		}

		// Final synchronization: ensure all Xmo buffers are populated before returning
		for (auto& worker : workers) 
		{
			if (worker.valid()) worker.get();
		}
	}

	void WriteToCoff(const std::vector<data::Xmo*>& xmos, const std::string& outputPath) {
		Coff coff;
		uint16_t textIdx = coff.CreateSection(".text", SectionType::Code);
		std::unordered_map<std::string, uint32_t> globalSymbolMap;

		// 1. Process Exports & Data
		for (auto* xmo : xmos) {
			coff.AppendPadding(textIdx, 16);

			// Record the offset BEFORE we add the data
			uint32_t xmoStartOffset = (uint32_t)coff.GetSectionBufferSize(textIdx);

			// Save the start offset for relocation step
			xmo->tempGlobalOffset = xmoStartOffset;

			// Add the code block and its main symbol
			// (Assuming each XMO has a primary name/entry)
			uint32_t symIdx = coff.AddDataSymbol(xmo->name, textIdx, xmo->codeBuffer);
			globalSymbolMap[xmo->name] = symIdx;

			for (auto& exp : xmo->exports) {
				// The symbol value is the start of the XMO + the internal offset
				uint32_t absoluteExportOffset = xmoStartOffset + exp.offset;

				// We don't use AddDataSymbol here because the bytes are already in the buffer
				RawSymbol sym = { 0 };
				coff.SetSymbolName(sym, exp.name);
				sym.SectionNumber = textIdx;
				sym.Value = absoluteExportOffset;
				sym.StorageClass = IMAGE_SYM_CLASS_EXTERNAL;

				// You'll need a small helper in Coff to push a RawSymbol directly
				uint32_t expIdx = coff.PushRawSymbol(sym);
				globalSymbolMap[exp.name] = expIdx;
			}

		}

		// 2. Process Relocations
		for (auto* xmo : xmos) {
			for (auto& rel : xmo->relocs) {
				// Find or Add the target symbol
				uint32_t targetIdx;
				if (globalSymbolMap.count(rel.targetSymbol)) {
					targetIdx = globalSymbolMap[rel.targetSymbol];
				}
				else {
					targetIdx = coff.AddExternalSymbol(rel.targetSymbol);
					globalSymbolMap[rel.targetSymbol] = targetIdx;
				}

				// The relocation offset is just XMO_START + LOCAL_OFFSET
				uint32_t finalRelocOffset = xmo->tempGlobalOffset + rel.offset;
				coff.AddRelocation(textIdx, finalRelocOffset, targetIdx, rel.type);
			}
		}


		coff.WriteTo(outputPath);
	}
}