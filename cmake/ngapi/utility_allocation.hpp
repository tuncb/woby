#pragma once

#include <new>
#include <stddef.h>

namespace gpu
{
namespace detail
{
#if defined(WOBY_UTILITY_ALLOCATION_TESTS)
bool fail_utility_allocation() noexcept;
void utility_array_created() noexcept;
void utility_array_destroyed() noexcept;
#endif

template<typename T>
T* allocate_utility_array(size_t count) noexcept
{
#if defined(WOBY_UTILITY_ALLOCATION_TESTS)
    if (fail_utility_allocation()) return nullptr;
#endif
    auto* result = new (std::nothrow) T[count]{};
#if defined(WOBY_UTILITY_ALLOCATION_TESTS)
    if (result) utility_array_created();
#endif
    return result;
}

template<typename T>
void free_utility_array(T* pointer) noexcept
{
#if defined(WOBY_UTILITY_ALLOCATION_TESTS)
    if (pointer) utility_array_destroyed();
#endif
    delete[] pointer;
}
} // namespace detail

template<typename T>
struct UtilityResult
{
    T* value = nullptr;
    const char* error = nullptr;
};

// Own the returned value only on success. Failed initialization is cleaned up
// here; error messages have static lifetime and never allocate memory.
template<typename T, typename... Args>
[[nodiscard]] UtilityResult<T> create_utility(Args&&... args) noexcept
{
    static_assert(noexcept(T(static_cast<Args&&>(args)...)));
    T* value = nullptr;
#if defined(WOBY_UTILITY_ALLOCATION_TESTS)
    if (!detail::fail_utility_allocation())
#endif
    {
        value = new (std::nothrow) T(static_cast<Args&&>(args)...);
    }
    if (!value) return {nullptr, "Insufficient CPU memory for graphics utility ownership."};
    if (value->error) {
        const char* error = value->error;
        delete value;
        return {nullptr, error};
    }
    return {value, nullptr};
}
} // namespace gpu
