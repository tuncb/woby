#pragma once

#include <new>
#include <stdint.h>
#include <stdlib.h>

namespace gpu::detail
{
struct BackendAllocation {};
#if defined(WOBY_BACKEND_ALLOCATION_TESTS)
bool fail_backend_allocation() noexcept;
#endif

inline bool backend_allocation_allowed() noexcept
{
#if defined(WOBY_BACKEND_ALLOCATION_TESTS)
    return !fail_backend_allocation();
#else
    return true;
#endif
}

template<typename T>
T* backend_array(size_t count) noexcept
{
    // Leave room for an implementation's array cookie as well as the elements.
    if (count > (SIZE_MAX - sizeof(size_t)) / sizeof(T) || !backend_allocation_allowed()) return nullptr;
    return new (std::nothrow) T[count]{};
}

template<typename T>
bool reserve_backend_array(T*& pointer, size_t& capacity, size_t count) noexcept
{
    if (count <= capacity) return true;
    if (count > SIZE_MAX / sizeof(T) || !backend_allocation_allowed()) return false;
    auto* replacement = static_cast<T*>(realloc(pointer, count * sizeof(T)));
    if (!replacement) return false;
    pointer = replacement;
    capacity = count;
    return true;
}
} // namespace gpu::detail

// Native-resource initializers must not run when CPU ownership cannot be allocated.
inline void* operator new(size_t bytes, gpu::detail::BackendAllocation) noexcept
{
    return gpu::detail::backend_allocation_allowed() ? ::operator new(bytes, std::nothrow) : nullptr;
}
inline void operator delete(void* pointer, gpu::detail::BackendAllocation) noexcept
{
    ::operator delete(pointer);
}
