// compiler/Arena.h
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <shared_mutex>
#include <vector>

namespace xmc
{
	// Lock-striped bump allocator.
	//
	// Chunk buffers are 64-byte aligned, so any Allocate request with
	// align <= 64 pays only offset rounding, not address rebasing. For
	// align > 64 the allocator still works via address-based alignment
	// and wastes at most (align - 1) bytes per allocation.
	//
	// Contents are never destructed. Do not store objects with
	// non-trivial destructors -- names, machine code, ParseTreeNodes,
	// and Symbols are all fine; anything holding a std::vector or
	// std::string by value is not.
	struct Arena
	{
		static constexpr size_t CHUNK_SIZE = 1024 * 1024;
		static constexpr size_t CHUNK_ALIGN = 64;

		struct Chunk
		{
			uint8_t* data;
			std::atomic<size_t> offset{ 0 };
			size_t              capacity;

			explicit Chunk(size_t cap) : capacity(cap)
			{
				data = static_cast<uint8_t*>(
					::operator new[](cap, std::align_val_t{ CHUNK_ALIGN }));
			}

			~Chunk()
			{
				::operator delete[](data, std::align_val_t{ CHUNK_ALIGN });
			}

			Chunk(const Chunk&) = delete;
			Chunk& operator=(const Chunk&) = delete;
			Chunk(Chunk&&) = delete;
			Chunk& operator=(Chunk&&) = delete;
		};

		std::shared_mutex                   chunk_mtx;
		std::vector<std::unique_ptr<Chunk>> chunks;

		void* Allocate(size_t size, size_t align = 8)
		{
			// Round size up to a multiple of align so the next allocation
			// at the same alignment lands naturally without more rounding.
			size = (size + align - 1) & ~(align - 1);

			// Fast path: shared lock, CAS on the back chunk's offset.
			{
				std::shared_lock rl(chunk_mtx);
				if (!chunks.empty()) {
					Chunk& cur = *chunks.back();
					uintptr_t base = reinterpret_cast<uintptr_t>(cur.data);
					size_t    old = cur.offset.load(std::memory_order_relaxed);
					while (true) {
						uintptr_t start = (base + old + align - 1) & ~(uintptr_t)(align - 1);
						size_t    aligned_off = static_cast<size_t>(start - base);
						size_t    next = aligned_off + size;
						if (next > cur.capacity) break;           // chunk full
						if (cur.offset.compare_exchange_weak(
							old, next, std::memory_order_relaxed)) {
							return cur.data + aligned_off;
						}
						// CAS refreshed `old` -- retry with the new value.
					}
				}
			}

			// Slow path: another thread may have added a chunk with room.
			std::unique_lock wl(chunk_mtx);

			if (!chunks.empty()) {
				Chunk& cur = *chunks.back();
				uintptr_t base = reinterpret_cast<uintptr_t>(cur.data);
				size_t    old = cur.offset.load(std::memory_order_relaxed);
				uintptr_t start = (base + old + align - 1) & ~(uintptr_t)(align - 1);
				size_t    aligned_off = static_cast<size_t>(start - base);
				size_t    next = aligned_off + size;
				if (next <= cur.capacity) {
					cur.offset.store(next, std::memory_order_relaxed);
					return cur.data + aligned_off;
				}
			}

			// Grow. size + align guarantees the request fits even if the
			// caller asked for align > CHUNK_ALIGN.
			size_t chunk_cap = std::max(CHUNK_SIZE, size + align);
			auto   new_chunk = std::make_unique<Chunk>(chunk_cap);

			uintptr_t base = reinterpret_cast<uintptr_t>(new_chunk->data);
			uintptr_t start = (base + align - 1) & ~(uintptr_t)(align - 1);
			size_t    aligned_off = static_cast<size_t>(start - base);
			void* ptr = new_chunk->data + aligned_off;
			new_chunk->offset.store(aligned_off + size, std::memory_order_relaxed);
			chunks.push_back(std::move(new_chunk));
			return ptr;
		}

		template<typename T, typename... Args>
		T* Construct(Args&&... args)
		{
			void* mem = Allocate(sizeof(T), alignof(T));
			return new (mem) T(std::forward<Args>(args)...);
		}

		template<typename T>
		T* ConstructArray(uint32_t count)
		{
			void* mem = Allocate(sizeof(T) * count, alignof(T));
			T* ptr = static_cast<T*>(mem);
			for (uint32_t i = 0; i < count; ++i) new (&ptr[i]) T();
			return ptr;
		}

		template<typename T>
		T* NewArray(uint32_t count)
		{
			return static_cast<T*>(Allocate(count * sizeof(T), alignof(T)));
		}
	};

} // namespace xmc