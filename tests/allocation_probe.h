#pragma once

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <cstddef>

namespace woby::test {
// Count only this thread. The scope must contain the workload, not doctest checks.
inline thread_local size_t* allocationCount = nullptr;
inline int allocationHook(int kind, void*, size_t, int block, long, const unsigned char*, int)
{
    if (allocationCount && block != _CRT_BLOCK && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC)) {
        ++*allocationCount;
    }
    return 1;
}
struct AllocationProbe {
    size_t count = 0;
    _CRT_ALLOC_HOOK previous = _CrtSetAllocHook(allocationHook);
    AllocationProbe() { allocationCount = &count; }
    ~AllocationProbe() { allocationCount = nullptr; _CrtSetAllocHook(previous); }
};
template <typename Work>
size_t countAllocations(const Work& work)
{
    AllocationProbe probe;
    work();
    return probe.count;
}
} // namespace woby::test
#endif
