#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace woby {

// Shared by simultaneous analyses. Leave capacity for the UI, render thread,
// and the two coordinating analysis workers; small jobs stay on their caller.
inline std::atomic<unsigned> analysisWorkerCount{0};
inline unsigned analysisWorkerLimit()
{
    static const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 3 ? std::min(8u, hardware - 3) : 0u;
}

template <typename Function>
void parallelAnalysisBatches(size_t count, size_t batchSize, std::stop_token stop, const Function& function)
{
    if (batchSize == 0) { throw std::invalid_argument("Analysis batch size must be positive."); }
    const auto checkCanceled = [&] {
        if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
    };
    checkCanceled();
    if (count < 4096 || analysisWorkerLimit() == 0) {
        for (size_t begin = 0; begin < count;) {
            checkCanceled();
            const size_t end = begin + std::min(batchSize, count - begin);
            function(begin, end);
            begin = end;
        }
        checkCanceled();
        return;
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr failure;
    std::mutex failureMutex;
    const auto work = [&] {
        try {
            while (!failed.load(std::memory_order_relaxed)) {
                checkCanceled();
                const auto begin = next.fetch_add(batchSize, std::memory_order_relaxed);
                if (begin >= count) { break; }
                function(begin, begin + std::min(batchSize, count - begin));
            }
        } catch (...) {
            std::lock_guard lock(failureMutex);
            if (!failure) { failure = std::current_exception(); }
            failed.store(true, std::memory_order_relaxed);
        }
    };
    {
        std::vector<std::jthread> workers;
        const auto limit = analysisWorkerLimit();
        workers.reserve(limit);
        for (unsigned i = 0; i < limit && i + 1 < count / batchSize; ++i) {
            auto active = analysisWorkerCount.load(std::memory_order_relaxed);
            while (active < limit && !analysisWorkerCount.compare_exchange_weak(active, active + 1,
                std::memory_order_relaxed)) {}
            if (active >= limit) { break; }
            try {
                workers.emplace_back([&] {
                    work();
                    analysisWorkerCount.fetch_sub(1, std::memory_order_relaxed);
                });
            } catch (const std::system_error&) {
                analysisWorkerCount.fetch_sub(1, std::memory_order_relaxed);
                break; // Thread resource exhaustion: the caller drains the queue.
            } catch (...) {
                analysisWorkerCount.fetch_sub(1, std::memory_order_relaxed);
                failed.store(true, std::memory_order_relaxed);
                throw;
            }
        }
        work();
    } // Join before reading results or propagating a worker's exception.
    if (failure) { std::rethrow_exception(failure); }
    checkCanceled();
}

} // namespace woby
