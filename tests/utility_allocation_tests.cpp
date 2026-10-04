#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "utility_allocation_probe.h"
#include <NoGraphicsAPIUtility/heap_allocator.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/delete_queue.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>
#include <NoGraphicsAPIUtility/texture_upload.hpp>
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

TEST_CASE("Upload command warmup failures unwind every pool without waiting for unsubmitted work")
{
    for (int step = 1; step <= 6; ++step) {
        utilityProbe = {};
        utilityProbe.failCommandAt = step;
        const auto result = gpu::create_utility<gpu::UploadQueue>(utilityTestDevice(), 64u, 0u, 2u);
        CHECK(result.value == nullptr);
        CHECK(result.error != nullptr);
        CHECK(utilityProbe.commandStep == step);
        checkReleased();
    }
}

TEST_CASE("Upload failures keep completion and retirement at the last successful submission")
{
    for (int step = 1; step <= 3; ++step) {
        utilityProbe = {};
        const auto created = gpu::create_utility<gpu::UploadQueue>(utilityTestDevice(), 64u, 0u, 2u);
        auto queue = own(created.value);
        REQUIRE(queue != nullptr);
        const gpu::byte bytes[64]{};
        gpu::byte destination[128]{};
        queue->upload_buffer({destination, sizeof(destination)}, {bytes, sizeof(bytes)});
        REQUIRE(queue->flush().value == 1);
        REQUIRE(utilityProbe.submissions == 1);

        // Ignore reclaim's pool reset; it is tested separately below.
        queue->reclaim();
        utilityProbe.failCommandAt = utilityProbe.commandStep + step;
        queue->upload_buffer({destination, sizeof(destination)}, {bytes, sizeof(bytes)});
        CHECK(queue->flush().value == 1);
        CHECK(queue->error != nullptr);
        CHECK(queue->stats().submissions == 1);
        CHECK(utilityProbe.submissions == 1);
        CHECK(utilityProbe.submittedValue == 1);
        const auto copies = utilityProbe.copies;
        queue->upload_buffer({destination, sizeof(destination)}, {bytes, sizeof(bytes)});
        CHECK(utilityProbe.copies == copies);
        queue->wait();
        queue.reset();
        CHECK(utilityProbe.waitedValue <= 1);
        checkReleased();
    }
}

TEST_CASE("Automatic upload flush failure stops a large copy without a null staging write")
{
    utilityProbe = {};
    const auto created = gpu::create_utility<gpu::UploadQueue>(utilityTestDevice(), 64u, 0u, 2u);
    auto queue = own(created.value);
    REQUIRE(queue != nullptr);
    utilityProbe.failCommandAt = utilityProbe.commandStep + 3; // begin, end, submit
    const gpu::byte bytes[128]{};
    gpu::byte destination[128]{};
    queue->upload_buffer({destination, sizeof(destination)}, {bytes, sizeof(bytes)});
    CHECK(queue->error != nullptr);
    CHECK(utilityProbe.copies == 1);
    CHECK(queue->flush().value == 0);
    CHECK(utilityProbe.submissions == 0);
    queue.reset();
    checkReleased();
}

TEST_CASE("Upload pool reset failure is reported after reclaiming only submitted batches")
{
    utilityProbe = {};
    const auto created = gpu::create_utility<gpu::UploadQueue>(utilityTestDevice(), 64u, 0u, 2u);
    auto queue = own(created.value);
    REQUIRE(queue != nullptr);
    const gpu::byte bytes[16]{};
    gpu::byte destination[16]{};
    queue->upload_buffer({destination, sizeof(destination)}, {bytes, sizeof(bytes)});
    REQUIRE(queue->flush().value == 1);
    utilityProbe.failCommandAt = utilityProbe.commandStep + 1;
    queue->reclaim();
    CHECK(queue->error != nullptr);
    CHECK(queue->stats().pending_batches == 0);
    CHECK(queue->flush().value == 1);
    queue.reset();
    checkReleased();
}

TEST_CASE("Tiled texture uploads stop before writing staging memory or recording after failure")
{
    for (int step = 1; step <= 3; ++step) {
        utilityProbe = {};
        const auto created = gpu::create_utility<gpu::UploadQueue>(utilityTestDevice(), 64u, 0u, 2u);
        auto queue = own(created.value);
        REQUIRE(queue != nullptr);
        utilityProbe.failCommandAt = utilityProbe.commandStep + step;
        const gpu::byte pixels[128]{};
        // The isolated copy callback does not inspect native texture ownership.
        gpu::upload_texture(*queue, nullptr,
            {.extent = {8, 4, 1}, .format = gpu::Format::rgba8_unorm}, {pixels, sizeof(pixels)});
        CHECK(queue->error != nullptr);
        CHECK(utilityProbe.copies == (step == 1 ? 0 : 1));
        CHECK(queue->flush().value == 0);
        queue.reset();
        checkReleased();
    }
}
