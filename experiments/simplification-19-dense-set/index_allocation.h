#pragma once

// Count only analysis-index allocator requests. The same allocator is used by
// the extracted custom vectors and by dense values/bucket allocator rebinds.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>

namespace woby {
namespace dense_set_allocations {
inline std::atomic<size_t> live{0},peak{0},total{0},calls{0};
struct Start { size_t liveBytes,totalBytes,callCount; };
inline Start start() {
    const auto current=live.load(); peak.store(current);
    return {current,total.load(),calls.load()};
}
}
#if defined(WOBY_DENSE_SET_REPORT_EXIT)
struct DenseSetAllocationExitReport {
    ~DenseSetAllocationExitReport() {
        std::fprintf(stderr,"{\"analysis_index_allocations\":{\"live_requested_bytes\":%zu,\"peak_requested_bytes\":%zu,\"total_requested_bytes\":%zu,\"allocation_calls\":%zu}}\n",
            dense_set_allocations::live.load(),dense_set_allocations::peak.load(),
            dense_set_allocations::total.load(),dense_set_allocations::calls.load());
    }
};
inline void ensureDenseSetAllocationExitReport() {
    // A non-ODR-used inline global can have deferred initialization. Register
    // this local owner at the first allocation in both pipeline backends.
    static DenseSetAllocationExitReport report;
    (void)report;
}
#endif
template<class T> struct DenseSetTrackingAllocator {
    using value_type=T;
    using is_always_equal=std::true_type;
    DenseSetTrackingAllocator()=default;
    template<class U> DenseSetTrackingAllocator(const DenseSetTrackingAllocator<U>&) {}
    T* allocate(size_t count) {
#if defined(WOBY_DENSE_SET_REPORT_EXIT)
        ensureDenseSetAllocationExitReport();
#endif
        auto* result=std::allocator<T>{}.allocate(count);
        const auto requested=count*sizeof(T);
        const auto current=dense_set_allocations::live.fetch_add(requested,std::memory_order_relaxed)+requested;
        auto high=dense_set_allocations::peak.load(std::memory_order_relaxed);
        while(current>high&&!dense_set_allocations::peak.compare_exchange_weak(high,current,std::memory_order_relaxed)) {}
        dense_set_allocations::total.fetch_add(requested,std::memory_order_relaxed);
        dense_set_allocations::calls.fetch_add(1,std::memory_order_relaxed);
        return result;
    }
    void deallocate(T* pointer,size_t count) {
        dense_set_allocations::live.fetch_sub(count*sizeof(T),std::memory_order_relaxed);
        std::allocator<T>{}.deallocate(pointer,count);
    }
    template<class U> bool operator==(const DenseSetTrackingAllocator<U>&) const { return true; }
};
}
