// data/Arena.h
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

using std::make_unique;
using std::unique_ptr;
using std::vector;

namespace data
{

	struct Arena {

		struct Chunk {
			std::unique_ptr<uint8_t[]> data;
			std::atomic<size_t> offset{ 0 };
			size_t capacity;

			Chunk(size_t cap) : data(std::make_unique<uint8_t[]>(cap)), capacity(cap) {}
		};

		std::shared_mutex chunk_mtx; // Protects the vector itself
		std::vector<std::unique_ptr<Chunk>> chunks;
		static constexpr size_t CHUNK_SIZE = 1024 * 1024;	// 1mb chunks

		void* Allocate(size_t size) {
			size = (size + 7) & ~7; // 8-byte align

			// Try to allocate from the current chunk (Lock-free Fast Path)
			{
				std::shared_lock read_lock(chunk_mtx);
				if (!chunks.empty()) {
					Chunk& current = *chunks.back();
					size_t old_offset = current.offset.fetch_add(size, std::memory_order_relaxed);

					if (old_offset + size <= current.capacity) {
						return &current.data[old_offset];
					}
				}
			}

			// Current chunk is full, need a new one
			std::unique_lock write_lock(chunk_mtx);

			// Double-check after acquiring lock to see if another thread already added a chunk
			if (!chunks.empty()) {
				Chunk& current = *chunks.back();
				// We use a non-atomic read here because we hold the write lock
				if (current.offset.load() + size <= current.capacity) {
					size_t old_offset = current.offset.fetch_add(size);
					return &current.data[old_offset];
				}
			}

			// Add new chunk
			size_t next_size = std::max(CHUNK_SIZE, size);
			auto new_chunk = std::make_unique<Chunk>(next_size);

			// allocate in the new chunk
			void* ptr = &new_chunk->data[0];
			new_chunk->offset.store(size);
			chunks.push_back(std::move(new_chunk));

			return ptr;
		}

		// construct any object in the arena
		template<typename T, typename... Args>
		T* Construct(Args&&... args) {
			void* mem = Allocate(sizeof(T));
			return new (mem) T(std::forward<Args>(args)...); // construct and place
		}

	};

} // ns data
