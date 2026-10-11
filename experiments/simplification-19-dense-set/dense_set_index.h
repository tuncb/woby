#pragma once

// Probe-only owning facade. Each key lives once, in unordered_dense::set values.
// No pointer/span is stored, so default copy/move cannot retain another owner's
// key view. Like the production vector, returned borrows expire on mutation.
#include "analysis_index.h"
#include "index_allocation.h"
#include <ankerl/unordered_dense.h>
#include <span>

namespace woby {
template<class T, size_t N> struct DenseSetKeyHash {
    size_t operator()(const std::array<T,N>& key) const { return analysisKeyHash(key); }
};

template<class T, size_t N, class Bucket = ankerl::unordered_dense::bucket_type::standard>
struct DenseSetKeys {
    using Key = std::array<T,N>;
    using Set = ankerl::unordered_dense::set<Key,DenseSetKeyHash<T,N>,std::equal_to<Key>,DenseSetTrackingAllocator<Key>,Bucket>;
    Set lookup;
    size_t size() const { return lookup.values().size(); }
    size_t capacity() const { return lookup.values().capacity(); }
    bool empty() const { return lookup.values().empty(); }
    const Key* data() const { return lookup.values().data(); }
    const Key& operator[](size_t id) const { return lookup.values()[id]; }
    auto begin() const { return lookup.values().begin(); }
    auto end() const { return lookup.values().end(); }
};

template<class T, size_t N, class Bucket = ankerl::unordered_dense::bucket_type::standard, bool FindFirst = false>
struct DenseSetAnalysisIndex { DenseSetKeys<T,N,Bucket> keys; };

template<class T, size_t N, class Bucket>
constexpr size_t denseSetCapacityLimit() {
    using Set = typename DenseSetKeys<T,N,Bucket>::Set;
    // A size equal to max_size() can wrap the library's value-index loop bound.
    // The production reserve also rejects values above SIZE_MAX >> 2.
    return std::min(std::numeric_limits<size_t>::max() >> 2, Set::max_size()-1);
}

template<class T, size_t N, class Bucket, bool FindFirst>
void reserveAnalysisIndex(DenseSetAnalysisIndex<T,N,Bucket,FindFirst>& index, size_t capacity) {
    if(capacity > denseSetCapacityLimit<T,N,Bucket>()) {
        throw std::length_error("Dense-set probe requires the production wide fallback.");
    }
    // set.insert appends before duplicate detection. One spare slot prevents a
    // full-reserve hit from growing the value vector just to remove that copy.
    index.keys.lookup.reserve(capacity+(FindFirst?0:1));
}

template<class T, size_t N, class Bucket, bool FindFirst>
size_t analysisIndex(DenseSetAnalysisIndex<T,N,Bucket,FindFirst>& index, const std::array<T,N>& key) {
    auto& set = index.keys.lookup;
    // A copied value vector can have capacity==size despite the spare policy.
    // Search that boundary before insert so a hit keeps all existing borrows.
    if(FindFirst || set.size()==set.values().capacity() || set.size() >= denseSetCapacityLimit<T,N,Bucket>()) {
        const auto found = set.find(key);
        if(found != set.end()) { return static_cast<size_t>(found-set.begin()); }
        if(set.size() >= denseSetCapacityLimit<T,N,Bucket>()) {
            throw std::length_error("Dense-set probe requires the production wide fallback.");
        }
    }
    const auto [entry,inserted] = set.insert(key);
    const auto id=static_cast<size_t>(entry-set.begin());
    if constexpr(!FindFirst) {
        if(inserted && set.size()==set.values().capacity()) {
            // Keep a spare after unreserved growth too. Grow geometrically;
            // reserving just one extra slot each time would be quadratic.
            const auto capacity=set.values().capacity();
            const auto expanded=capacity+std::max(size_t{1},capacity/2);
            set.reserve(std::min(expanded,denseSetCapacityLimit<T,N,Bucket>()+1));
        }
    }
    return id;
}
}
