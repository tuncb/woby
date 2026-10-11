#pragma once

// Requested C++ allocation bytes, not allocator overhead, RSS, or working set.
// Every allocation has a small header so a delete can remove its original size.
#include <atomic>
#include <cstdlib>
#include <new>
#include <cstdint>
#include <cstddef>
#include <algorithm>

namespace allocation_probe {
struct Header { void* raw; size_t bytes; };
inline std::atomic<size_t> live{0}, peak{0}, total{0}, calls{0};
inline void* allocate(size_t bytes, size_t alignment) {
    alignment = std::max(alignment, alignof(Header));
    if (bytes > SIZE_MAX - alignment - sizeof(Header)) { throw std::bad_alloc(); }
    void* raw = std::malloc(bytes + alignment + sizeof(Header));
    if (!raw) { throw std::bad_alloc(); }
    const auto address = (reinterpret_cast<uintptr_t>(raw) + sizeof(Header) + alignment - 1) & ~(alignment - 1);
    auto* header = reinterpret_cast<Header*>(address) - 1;
    header->raw = raw; header->bytes = bytes;
    const auto current = live.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    auto high = peak.load(std::memory_order_relaxed);
    while (current > high && !peak.compare_exchange_weak(high, current, std::memory_order_relaxed)) {}
    total.fetch_add(bytes, std::memory_order_relaxed); calls.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<void*>(address);
}
inline void release(void* pointer) noexcept {
    if (!pointer) { return; }
    const auto* header = static_cast<Header*>(pointer) - 1;
    live.fetch_sub(header->bytes, std::memory_order_relaxed); std::free(header->raw);
}
struct Start { size_t liveBytes, totalBytes, callCount; };
inline Start start() {
    const auto bytes = live.load(); peak.store(bytes);
    return {bytes, total.load(), calls.load()};
}
}
void* operator new(size_t n) { return allocation_probe::allocate(n, alignof(std::max_align_t)); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { allocation_probe::release(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }
void* operator new(size_t n, std::align_val_t a) { return allocation_probe::allocate(n, static_cast<size_t>(a)); }
void* operator new[](size_t n, std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void* p, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { ::operator delete(p); }
