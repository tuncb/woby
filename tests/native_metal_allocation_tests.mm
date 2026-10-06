#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <string_view>
#include <thread>
#include <vector>

#define WOBY_BACKEND_ALLOCATION_TESTS
namespace {
std::atomic<int> allocationStep{0};
int failAllocationAt = 0;
}
namespace gpu::detail {
bool fail_backend_allocation() noexcept { return ++allocationStep == failAllocationAt; }
}
#include <NoGraphicsAPIMetal.mm>

namespace {
struct MetalFixture {
    gpu::Device* device = nullptr;
    gpu::CommandPool* pool = nullptr;
    std::vector<gpu::GpuHeap> heaps;
    MetalFixture() {
        allocationStep = failAllocationAt = 0;
        device = gpu::create_device({.timestamp_query_count = 4}).device;
        if (device) pool = gpu::create_command_pool(device);
    }
    ~MetalFixture() {
        failAllocationAt = 0;
        if (device) {
            gpu::abandon_acquired_image(device);
            gpu::wait_idle(device);
            for (auto heap:heaps) gpu::destroy_gpu_heap(heap);
            gpu::destroy_command_pool(pool);
            gpu::destroy_device(device);
        }
    }
};
}

TEST_CASE("Metal device ownership fails without throwing through noexcept") {
    allocationStep = 0; failAllocationAt = 1;
    const auto result = gpu::create_device({});
    failAllocationAt = 0;
    CHECK(result.device == nullptr);
    CHECK(result.error == gpu::Error::driver_error);
}

TEST_CASE("Metal queue ownership failure releases partially initialized device") {
    MetalFixture supported;
    if (!supported.device) { WARN_MESSAGE(false, "Metal 4 GPU unavailable; native recovery requires macOS 26 and supported hardware"); return; }
    allocationStep = 0; failAllocationAt = 2;
    const auto result = gpu::create_device({});
    failAllocationAt = 0;
    CHECK(result.device == nullptr);
    CHECK(result.error == gpu::Error::driver_error);
}

TEST_CASE("Metal command context failures release native objects and timestamp reservations before retry") {
    int allocations = 0;
    {
        MetalFixture baseline;
        if (!baseline.device) { WARN_MESSAGE(false, "Metal 4 GPU unavailable; command recovery was not exercised"); return; }
        REQUIRE(baseline.pool != nullptr);
        allocationStep = 0;
        auto* commands = gpu::begin_commands(baseline.pool);
        REQUIRE(commands != nullptr);
        allocations = allocationStep;
        REQUIRE(allocations >= 4);
        REQUIRE(gpu::end_commands(commands) == nullptr);
    }
    for (int step = 1; step <= allocations; ++step) {
        INFO("allocation failure at " << step);
        MetalFixture fixture;
        REQUIRE(fixture.pool != nullptr);
        allocationStep = 0; failAllocationAt = step;
        auto* failed = gpu::begin_commands(fixture.pool);
        failAllocationAt = 0;
        CHECK(failed == nullptr);
        CHECK(gpu::command_pool_error(fixture.pool) != nullptr);
        CHECK(fixture.pool->first == nullptr);
        for (auto* page = fixture.device->counter_pages; page; page = page->next)
            for (auto occupied : page->occupied) CHECK(occupied == 0);
        auto* recovered = gpu::begin_commands(fixture.pool);
        REQUIRE(recovered != nullptr);
        CHECK(gpu::end_commands(recovered) == nullptr);
    }
}

TEST_CASE("Metal command pool depth state allocations unwind on every failure") {
    MetalFixture fixture;
    if (!fixture.device) { WARN_MESSAGE(false, "Metal 4 GPU unavailable; depth state recovery was not exercised"); return; }
    allocationStep = 0;
    auto* baseline = gpu::create_command_pool(fixture.device);
    REQUIRE(baseline != nullptr);
    const int allocations = allocationStep;
    gpu::destroy_command_pool(baseline);
    for (int step = 1; step <= allocations; ++step) {
        allocationStep = 0; failAllocationAt = step;
        auto* failed = gpu::create_command_pool(fixture.device);
        failAllocationAt = 0;
        CHECK(failed == nullptr);
        gpu::destroy_command_pool(failed);
    }
}

TEST_CASE("Metal command segment failure is reported before submission and can be reset") {
    MetalFixture fixture;
    if (!fixture.device) { WARN_MESSAGE(false, "Metal 4 GPU unavailable; segment recovery was not exercised"); return; }
    REQUIRE(fixture.pool != nullptr);
    for (int step = 1; step <= 3; ++step) {
        auto* commands = gpu::begin_commands(fixture.pool);
        REQUIRE(commands != nullptr);
        [commands->commands endCommandBuffer];
        commands->commands = nil;
        allocationStep = 0; failAllocationAt = step;
        CHECK(gpu::native_commands(commands) == nil);
        failAllocationAt = 0;
        CHECK(gpu::end_commands(commands) != nullptr);
        CHECK(gpu::reset_command_pool(fixture.pool) == nullptr);
    }
}

TEST_CASE("Metal compute encoder failure is latched and reset before recording again") {
    MetalFixture fixture;
    if (!fixture.device) { WARN_MESSAGE(false, "Metal 4 GPU unavailable; encoder recovery was not exercised"); return; }
    REQUIRE(fixture.pool != nullptr);
    auto* commands = gpu::begin_commands(fixture.pool);
    REQUIRE(commands != nullptr);
    allocationStep = 0; failAllocationAt = 1;
    CHECK(gpu::compute_encoder(commands) == nil);
    failAllocationAt = 0;
    CHECK(gpu::end_commands(commands) != nullptr);
    REQUIRE(gpu::reset_command_pool(fixture.pool) == nullptr);
    commands = gpu::begin_commands(fixture.pool);
    REQUIRE(commands != nullptr);
    REQUIRE(gpu::compute_encoder(commands) != nil);
    CHECK(gpu::end_commands(commands) == nullptr);
}

TEST_CASE("Metal submission growth failure leaves completion and command ownership untouched") {
    MetalFixture fixture;
    if (!fixture.device) { WARN_MESSAGE(false, "Metal 4 GPU unavailable; submission recovery was not exercised"); return; }
    REQUIRE(fixture.pool != nullptr);
    auto* commands = gpu::begin_commands(fixture.pool);
    REQUIRE(commands != nullptr);
    REQUIRE(gpu::end_commands(commands) == nullptr);
    auto* timeline = gpu::create_timeline_semaphore(fixture.device);
    REQUIRE(timeline != nullptr);
    const std::array commandList{commands};
    const gpu::SubmitDesc desc{.commands = commandList, .completion = {timeline, 1}};
    auto& queue = fixture.device->queues[0];
    allocationStep = 0; failAllocationAt = 1;
    const auto failed = gpu::submit(fixture.device, desc);
    failAllocationAt = 0;
    CHECK_FALSE(failed.submitted);
    CHECK(failed.error != nullptr);
    CHECK(queue.submitted_value == 0);
    CHECK(queue.submission == nullptr);
    CHECK(queue.submission_capacity == 0);
    CHECK(commands->retirement == nil);
    const auto recovered = gpu::submit(fixture.device, desc);
    CHECK(recovered.submitted);
    CHECK(recovered.error == nullptr);
    gpu::wait_timeline({timeline, 1});
    gpu::destroy_timeline_semaphore(timeline);
}

TEST_CASE("Metal buffer registry grows beyond 64 and resolves live subranges after removal") {
    MetalFixture fixture;
    if (!fixture.device) { WARN_MESSAGE(false, "Metal 4 GPU unavailable; registry growth was not exercised"); return; }
    for (int i=0;i<256;++i) {
        const auto heap=gpu::create_gpu_heap(fixture.device,4096);
        REQUIRE(heap.owner != nullptr); fixture.heaps.push_back(heap);
    }
    CHECK(fixture.device->buffer_count==256);
    for (size_t i=0;i<fixture.heaps.size();++i) {
        auto& heap=fixture.heaps[i];
        gpu::uint64 offset=0;
        CHECK(gpu::resolve_buffer(fixture.device,{heap.range.gpu+127,4096-127},&offset)!=nil);
        CHECK(offset==127);
        CHECK(gpu::resolve_buffer(fixture.device,{heap.range.gpu+127,4096},&offset)==nil);
        if (i%2==0) { gpu::destroy_gpu_heap(heap); heap={}; }
    }
    CHECK(fixture.device->buffer_count==128);
    // Concurrent registration/removal must not invalidate lookup snapshots.
    std::array<bool,4> valid{true,true,true,true};
    {
        std::array<std::jthread,4> workers;
        for (size_t worker=0;worker<workers.size();++worker) workers[worker]=std::jthread([&,worker] {
            for (size_t i=1;i<fixture.heaps.size();i+=2) {
                const auto extra=gpu::create_gpu_heap(fixture.device,4096);
                gpu::uint64 offset=0;
                valid[worker]=valid[worker] && extra.owner
                    && gpu::resolve_buffer(fixture.device,{fixture.heaps[i].range.gpu+23,17},&offset)!=nil && offset==23;
                gpu::destroy_gpu_heap(extra);
            }
        });
    }
    for (bool result:valid) CHECK(result);
    CHECK(fixture.device->buffer_count==128);
}

TEST_CASE("Metal buffer registry growth failure preserves existing buffers and permits retry") {
    MetalFixture fixture;
    if (!fixture.device) { WARN_MESSAGE(false, "Metal 4 GPU unavailable; registry recovery was not exercised"); return; }
    do {
        const auto heap=gpu::create_gpu_heap(fixture.device,4096);
        REQUIRE(heap.owner != nullptr); fixture.heaps.push_back(heap);
    } while (fixture.device->buffer_count<fixture.device->buffer_capacity);
    const auto count=fixture.device->buffer_count;
    allocationStep=0; failAllocationAt=2; // Owner allocation succeeds, registry growth fails.
    const auto heap = gpu::create_gpu_heap(fixture.device, 4096);
    failAllocationAt=0;
    CHECK(heap.owner == nullptr);
    REQUIRE(heap.error != nullptr);
    CHECK(std::string_view(heap.error) == "Metal buffer registration capacity exhausted.");
    CHECK(fixture.device->buffer_count==count);
    for (const auto& live:fixture.heaps) {
        gpu::uint64 offset=0;
        CHECK(gpu::resolve_buffer(fixture.device,{live.range.gpu,4096},&offset)!=nil);
    }
    const auto recovered = gpu::create_gpu_heap(fixture.device, 4096);
    REQUIRE(recovered.owner != nullptr);
    CHECK(fixture.device->buffer_count==count+1);
    gpu::destroy_gpu_heap(recovered);
    CHECK(fixture.device->buffer_count==count);
}
