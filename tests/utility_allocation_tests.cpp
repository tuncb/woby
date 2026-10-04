#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "utility_allocation_probe.h"
#include <NoGraphicsAPIUtility/heap_allocator.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/delete_queue.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>
#include <memory>

namespace
{
template<typename T>
std::unique_ptr<T> own(T* value) { return std::unique_ptr<T>(value); }

void checkReleased()
{
    CHECK(utilityProbe.arrays == 0);
    CHECK(utilityProbe.heaps == 0);
    CHECK(utilityProbe.timelines == 0);
    CHECK(utilityProbe.pools == 0);
    CHECK(utilityProbe.textures == 0);
    CHECK(utilityProbe.unexpectedCalls == 0);
}

template<typename Create, typename Check>
void checkInitializationFailures(int steps, Create create, Check check)
{
    for (int step = 1; step <= steps; ++step) {
        INFO("failure step: " << step);
        utilityProbe = {};
        utilityProbe.failAt = step;
        const auto failed = create();
        auto failedOwner = own(failed.value);
        CHECK(failed.value == nullptr);
        REQUIRE(failed.error != nullptr);
        CHECK(failed.error[0] != '\0');
        CHECK(utilityProbe.step == step);
        checkReleased();

        // A failure must not poison the next attempt or retain partial resources.
        utilityProbe.step = 0;
        utilityProbe.failAt = 0;
        const auto recovered = create();
        auto owner = own(recovered.value);
        REQUIRE(recovered.value != nullptr);
        CHECK(recovered.error == nullptr);
        CHECK(utilityProbe.step == steps);
        check(*owner);
        owner.reset();
        checkReleased();
    }
}
} // namespace

TEST_CASE("Utility heap initialization reports either CPU allocation failure and recovers")
{
    alignas(16) gpu::byte storage[64]{};
    checkInitializationFailures(3,
        [&] { return gpu::create_utility<gpu::HeapAllocator>(gpu::GpuCpuRange<gpu::byte>{storage, storage, sizeof(storage)}, 4u); },
        [](gpu::HeapAllocator& allocator) {
            const auto allocation = allocator.allocate(64);
            REQUIRE(allocation.range.cpu != nullptr);
            CHECK(allocator.allocate(16).range.cpu == nullptr);
            allocator.free(allocation);
            const auto reused = allocator.allocate(64);
            CHECK(reused.range.cpu == allocation.range.cpu);
            allocator.free(reused);
        });
}

TEST_CASE("Utility texture bookkeeping failures release CPU storage and recover")
{
    checkInitializationFailures(3,
        [] { return gpu::create_utility<gpu::TextureAllocator>(utilityTestDevice(), utilityTestTextureHeap(), 4u); },
        [](gpu::TextureAllocator& allocator) {
            auto texture = allocator.allocate(nullptr, {.extent = {4, 1, 1}});
            CHECK(texture.status == gpu::TextureAllocationStatus::success);
            CHECK(texture.texture != nullptr);
            allocator.free(texture);
        });
}

TEST_CASE("Utility texture allocation distinguishes a full page from native failure and returns the range")
{
    utilityProbe = {};
    const auto created = gpu::create_utility<gpu::TextureAllocator>(utilityTestDevice(), utilityTestTextureHeap(), 4u);
    auto allocator = own(created.value);
    REQUIRE(allocator != nullptr);
    const auto tooLarge = allocator->allocate(nullptr, {.extent = {5, 1, 1}});
    CHECK(tooLarge.status == gpu::TextureAllocationStatus::full);
    CHECK(tooLarge.texture == nullptr);
    CHECK(utilityProbe.textureAttempts == 0);

    // Repeated failures must return the suballocation, not consume the page.
    for (int attempt = 0; attempt < 8; ++attempt) {
        utilityProbe.failAt = utilityProbe.step + 1;
        const auto failed = allocator->allocate(nullptr, {.extent = {4, 1, 1}});
        CHECK(failed.status == gpu::TextureAllocationStatus::failed);
        CHECK(failed.texture == nullptr);
        CHECK(utilityProbe.textures == 0);
        CHECK(utilityProbe.lastTextureOffset == 0);
    }
    utilityProbe.failAt = 0;
    auto texture = allocator->allocate(nullptr, {.extent = {4, 1, 1}});
    CHECK(texture.status == gpu::TextureAllocationStatus::success);
    CHECK(utilityProbe.textures == 1);
    const auto full = allocator->allocate(nullptr, {.extent = {1, 1, 1}});
    CHECK(full.status == gpu::TextureAllocationStatus::full);
    CHECK(utilityProbe.textureAttempts == 9);
    allocator->free(texture);
    allocator.reset();
    checkReleased();
}

TEST_CASE("Utility deletion queue reports CPU allocation failures and remains usable after retry")
{
    checkInitializationFailures(2,
        [] { return gpu::create_utility<gpu::DeleteQueue>(utilityTestTimeline(), 4u); },
        [](gpu::DeleteQueue& queue) {
            int callbacks = 0;
            queue.defer(1, [&callbacks]() noexcept { ++callbacks; });
            queue.drain();
            CHECK(callbacks == 1);
        });
}

TEST_CASE("Utility upload initialization unwinds every CPU and GPU allocation failure")
{
    checkInitializationFailures(6,
        [] { return gpu::create_utility<gpu::UploadQueue>(utilityTestDevice(), 64u, 0u, 2u); },
        [](gpu::UploadQueue& queue) {
            CHECK(queue.stats().capacity == 64);
            CHECK(utilityProbe.arrays == 1);
            CHECK(utilityProbe.heaps == 1);
            CHECK(utilityProbe.timelines == 1);
            CHECK(utilityProbe.pools == 2);
            queue.wait();
        });
}
