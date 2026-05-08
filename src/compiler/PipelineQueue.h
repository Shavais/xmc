// compiler/PipelineQueue.h
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "blockingconcurrentqueue.h"

namespace xmc
{
	// Wrapper around moodycamel::BlockingConcurrentQueue that records:
	//   maxDepth   -- highest size_approx seen at any enqueue
	//   maxBlockNs -- longest single wait_dequeue duration in nanoseconds
	//   totalEnq / totalDeq -- counters for the log
	//
	// Both metrics are best-effort under contention; size_approx is
	// already best-effort by design and the max-update is a relaxed CAS
	// loop, so concurrent producers can race and one update may be lost
	// in a tie. That's fine -- we want a ballpark, not a proof.
	template <class T>
	class PipelineQueue
	{
	public:
		void Enqueue(T&& item)
		{
			q_.enqueue(std::move(item));
			totalEnq_.fetch_add(1, std::memory_order_relaxed);
			const uint64_t depth = static_cast<uint64_t>(q_.size_approx());
			uint64_t prev = maxDepth_.load(std::memory_order_relaxed);
			while (depth > prev &&
				!maxDepth_.compare_exchange_weak(prev, depth,
					std::memory_order_relaxed)) {
			}
		}

		// Blocks until an item is available. Records the wait duration
		// into maxBlockNs if it exceeds the current max.
		void WaitDequeue(T& out)
		{
			const auto start = std::chrono::steady_clock::now();
			q_.wait_dequeue(out);
			const auto elapsed = std::chrono::steady_clock::now() - start;
			const uint64_t ns = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
			uint64_t prev = maxBlockNs_.load(std::memory_order_relaxed);
			while (ns > prev &&
				!maxBlockNs_.compare_exchange_weak(prev, ns,
					std::memory_order_relaxed)) {
			}
			totalDeq_.fetch_add(1, std::memory_order_relaxed);
		}

		uint64_t MaxDepth()   const { return maxDepth_.load(std::memory_order_relaxed); }
		uint64_t MaxBlockNs() const { return maxBlockNs_.load(std::memory_order_relaxed); }
		uint64_t TotalEnq()   const { return totalEnq_.load(std::memory_order_relaxed); }
		uint64_t TotalDeq()   const { return totalDeq_.load(std::memory_order_relaxed); }

	private:
		moodycamel::BlockingConcurrentQueue<T> q_;
		std::atomic<uint64_t> maxDepth_{ 0 };
		std::atomic<uint64_t> maxBlockNs_{ 0 };
		std::atomic<uint64_t> totalEnq_{ 0 };
		std::atomic<uint64_t> totalDeq_{ 0 };
	};
} // namespace xmc