#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <array>
#include <string_view>

#define WOBY_BACKEND_ALLOCATION_TESTS
namespace { int allocationStep = 0, failAllocationAt = 0; }
namespace gpu::detail {
bool fail_backend_allocation() noexcept { return ++allocationStep == failAllocationAt; }
}
#include <NoGraphicsAPI.cpp>

namespace {
struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"Woby allocation test", WS_OVERLAPPEDWINDOW,
        0, 0, 256, 256, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (handle) DestroyWindow(handle); }
};
struct NativeFixture {
    Window window;
    gpu::Device* device = nullptr;
    gpu::CommandPool* pool = nullptr;
    gpu::TimelineSemaphore* timeline = nullptr;
    NativeFixture() {
        allocationStep = failAllocationAt = 0;
        if (window.handle) device = gpu::create_device({.window = window.handle, .swapchain_format = gpu::Format::bgra8_unorm}).device;
        if (device) {
            pool = gpu::create_command_pool(device);
            timeline = gpu::create_timeline_semaphore(device);
        }
    }
    ~NativeFixture() {
        failAllocationAt = 0;
        if (!device) return;
        gpu::abandon_acquired_image(device);
        gpu::wait_idle(device);
        gpu::destroy_command_pool(pool);
        gpu::destroy_timeline_semaphore(timeline);
        gpu::destroy_device(device);
    }
};
}

TEST_CASE("Native Vulkan acquired image can be abandoned after submission bookkeeping OOM and presented again") {
    for (int step = 1; step <= 2; ++step) {
        NativeFixture fixture;
        if (!fixture.device) { WARN_MESSAGE(false, "Compatible windowed Vulkan GPU unavailable; native presentation recovery was not exercised"); return; }
        REQUIRE(fixture.pool != nullptr);
        REQUIRE(fixture.timeline != nullptr);
        auto* commands = gpu::begin_commands(fixture.pool);
        REQUIRE(commands != nullptr);
        REQUIRE(gpu::acquire(commands).render_view != nullptr);
        REQUIRE(gpu::end_commands(commands) == nullptr);
        const std::array commandList{commands};
        const gpu::SubmitDesc desc{.commands = commandList, .completion = {fixture.timeline, 1}};
        allocationStep = 0; failAllocationAt = step;
        const auto failed = gpu::submit_and_present(fixture.device, desc);
        failAllocationAt = 0;
        CHECK_FALSE(failed.submitted);
        CHECK(failed.error != nullptr);
        CHECK(gpu::timeline_completed_value(fixture.timeline) == 0);
        gpu::abandon_acquired_image(fixture.device);
        CHECK(fixture.device->acquired_swapchain == nullptr);
        REQUIRE(gpu::reset_command_pool(fixture.pool) == nullptr);
        auto* recovered = gpu::begin_commands(fixture.pool);
        REQUIRE(recovered != nullptr);
        REQUIRE(gpu::acquire(recovered).render_view != nullptr);
        REQUIRE(gpu::end_commands(recovered) == nullptr);
        const auto result = gpu::submit_and_present(fixture.device,
            {.commands = {recovered}, .completion = {fixture.timeline, 1}});
        REQUIRE(result.submitted);
        CHECK(result.error == nullptr);
        gpu::wait_timeline({fixture.timeline, 1});
        CHECK(gpu::timeline_completed_value(fixture.timeline) == 1);
    }
}

TEST_CASE("Native Vulkan device initialization recovers from every CPU ownership failure") {
    for (bool windowed : {false, true}) {
        Window window;
        if (windowed && !window.handle) { WARN_MESSAGE(false, "A hidden window could not be created; swapchain initialization was not exercised"); continue; }
        gpu::DeviceDesc desc{.swapchain_format = gpu::Format::bgra8_unorm};
        desc.window = windowed ? window.handle : nullptr;
        allocationStep = failAllocationAt = 0;
        auto baseline = gpu::create_device(desc);
        if (!baseline.device) {
            WARN_MESSAGE(false, "A compatible Vulkan GPU is unavailable; native device initialization recovery was not exercised");
            continue;
        }
        const int allocations = allocationStep;
        gpu::destroy_device(baseline.device);
        REQUIRE(allocations >= (windowed ? 4 : 3));
        for (int step = 1; step <= allocations; ++step) {
            INFO("windowed=" << windowed << ", CPU allocation=" << step);
            const char* diagnostic = nullptr;
            desc.diagnostic_context = &diagnostic;
            desc.diagnostic = [](void* context, const gpu::DeviceDiagnostic& value) noexcept {
                if (value.kind == gpu::DeviceDiagnosticKind::initialization_error)
                    *static_cast<const char**>(context) = value.detail;
            };
            allocationStep = 0; failAllocationAt = step;
            const auto failed = gpu::create_device(desc);
            failAllocationAt = 0;
            CHECK(failed.device == nullptr);
            REQUIRE(diagnostic != nullptr);
            CHECK(std::string_view(diagnostic).find("Insufficient CPU memory") != std::string_view::npos);
            auto recovered = gpu::create_device(desc);
            REQUIRE(recovered.device != nullptr);
            gpu::destroy_device(recovered.device);
        }
    }
}
