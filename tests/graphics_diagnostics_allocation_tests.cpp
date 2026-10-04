#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "graphics.h"
#include "graphics_diagnostics.h"
#include <cstdlib>
#include <new>
#include <string_view>

namespace {
thread_local bool denyAllocations = false;
struct NoMemory {
    NoMemory() { denyAllocations = true; }
    ~NoMemory() { denyAllocations = false; }
};
}
// An isolated executable keeps fault injection out of other tests and production.
void* operator new(size_t bytes) {
    if (denyAllocations) throw std::bad_alloc();
    if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }

TEST_CASE("Device diagnostics cannot throw through the backend when all CPU allocations fail") {
    namespace g = woby::graphics;
    g::DeviceCreationDiagnostics diagnostics;
    char detail[] = "Insufficient CPU memory for Vulkan queue priorities";
    {
        NoMemory failure;
        g::collectDeviceDiagnostic(&diagnostics, {.kind = gpu::DeviceDiagnosticKind::initialization_error,
            .detail = detail, .error = gpu::Error::driver_error, .api_result = -1});
    }
    detail[0] = 'x';
    CHECK(diagnostics.incomplete);
    CHECK(diagnostics.entries.empty());
    CHECK(std::string_view(diagnostics.emergencyDetail.data()) == "Insufficient CPU memory for Vulkan queue priorities");
    CHECK(diagnostics.emergencyApiResult == -1);
    const auto message = g::formatDeviceCreationFailure(diagnostics, gpu::Error::driver_error);
    CHECK(message.find("CPU memory was exhausted") != std::string::npos);
    CHECK(message.find("Vulkan queue priorities") != std::string::npos);
}

TEST_CASE("Renderer initialization has an allocation-free error and can be retried") {
    namespace g = woby::graphics;
    g::shutdown();
    g::Init options;
    options.type = g::RendererType::Noop;
    bool initialized = true;
    {
        NoMemory failure;
        initialized = g::init(options);
    }
    CHECK_FALSE(initialized);
    CHECK(std::string_view(g::initializationError()) == "Insufficient CPU memory while initializing the renderer.");
    REQUIRE(g::init(options));
    CHECK(std::string_view(g::initializationError()).empty());
    g::shutdown();
}
