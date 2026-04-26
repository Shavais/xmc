// data/Arena.h
#pragma once

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <vector>

using std::make_unique;
using std::unique_ptr;
using std::vector;

namespace xmc
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

		// allocate a contiguous array of objects in the arena
		template<typename T>
		T* ConstructArray(uint32_t count) {
			void* mem = Allocate(sizeof(T) * count);
			T* ptr = static_cast<T*>(mem);

			// Call default constructor for each element in the array
			for (uint32_t i = 0; i < count; ++i) {
				new (&ptr[i]) T();
			}
			return ptr;
		}

		template <typename T>
		T* NewArray(uint32_t count) {
			return (T*)this->Allocate(count * sizeof(T));
		}

	};

	// This was an attempt to create an STL compatable arena allocator, but our arena doesn't work for STL containers
	// because of the chunk boundaries. Also, STL containers really need a heap, not an arena.
	// 
	//template <typename T>	
	//struct ArenaAllocator {
	//	using value_type = T;

	//	Arena* arena;

	//	// Required constructor
	//	ArenaAllocator(Arena& a) noexcept : arena(&a) {}

	//	// Required copy constructor for template rebound
	//	template <typename U>
	//	ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena(other.arena) {}

	//	// The core allocation call
	//	T* allocate(std::size_t n) {
	//		return static_cast<T*>(arena->Allocate(n * sizeof(T)));
	//	}

	//	// Deallocate is empty! This is the magic of arenas.
	//	void deallocate(T* p, std::size_t n) noexcept {}

	//	// Required comparison operators
	//	bool operator==(const ArenaAllocator& other) const { return arena == other.arena; }
	//	bool operator!=(const ArenaAllocator& other) const { return arena != other.arena; }
	//};

} // data
