#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <array>
#include <string_view>

namespace {
template<typename T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
struct CommandProbe {
    int cpuStep = 0, failCpuAt = 0, step = 0, failAt = 0;
    int buffers = 0, queries = 0, submits = 0, presents = 0;
    int viewsDestroyed = 0, swapchainsDestroyed = 0, semaphoresDestroyed = 0;
    int semaphores = 0, fences = 0, instancesDestroyed = 0;
    int invalidWaits = 0, fenceWaits = 0;
    uint64_t signaled = 0, retirement = 0;
    bool fenceSignaled = false, acquireSignaled = false;
    VkResult failure = VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult presentResult = VK_SUCCESS, acquireResult = VK_SUCCESS, surfaceResult = VK_SUCCESS;
    VkExtent2D surfaceExtent{32, 32};
    VkResult result() { return ++step == failAt ? failure : VK_SUCCESS; }
} probe;

VKAPI_ATTR VkResult VKAPI_CALL allocateCommands(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* output) {
    const auto result = probe.result();
    // Failed Vulkan output parameters need not retain their previous value.
    *output = handle<VkCommandBuffer>(result == VK_SUCCESS ? uintptr_t(100 + ++probe.buffers) : 0xcdcd);
    return result;
}
VKAPI_ATTR void VKAPI_CALL freeCommands(VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer*) { probe.buffers -= int(count); }
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) { probe.buffers = 0; }
VKAPI_ATTR VkResult VKAPI_CALL createQueries(VkDevice, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool* output) {
    const auto result = probe.result();
    *output = handle<VkQueryPool>(result == VK_SUCCESS ? uintptr_t(200 + ++probe.queries) : 0xcdcd);
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroyQueries(VkDevice, VkQueryPool value, const VkAllocationCallbacks*) {
    if (value == handle<VkQueryPool>(0xcdcd)) ++probe.invalidWaits;
    --probe.queries;
}
VKAPI_ATTR void VKAPI_CALL resetQueries(VkDevice, VkQueryPool, uint32_t, uint32_t) {}
VKAPI_ATTR VkResult VKAPI_CALL beginCommands(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return probe.result(); }
VKAPI_ATTR VkResult VKAPI_CALL endCommands(VkCommandBuffer) { return probe.result(); }
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) { return probe.result(); }
VKAPI_ATTR void VKAPI_CALL pipelineBarrier(VkCommandBuffer, const VkDependencyInfo*) {}
VKAPI_ATTR VkResult VKAPI_CALL queueSubmit(VkQueue, uint32_t, const VkSubmitInfo2* info, VkFence) {
    const auto result = probe.result();
    if (result == VK_SUCCESS) {
        ++probe.submits;
        probe.signaled = info->pSignalSemaphoreInfos[0].value;
        if (info->signalSemaphoreInfoCount == 3) probe.retirement = info->pSignalSemaphoreInfos[2].value;
    }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL resetFences(VkDevice, uint32_t, const VkFence* fence) {
    if (*fence == handle<VkFence>(10)) probe.acquireSignaled = false;
    else probe.fenceSignaled = false;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL waitFences(VkDevice, uint32_t, const VkFence* fence, VkBool32, uint64_t) {
    ++probe.fenceWaits;
    if (!(*fence == handle<VkFence>(10) ? probe.acquireSignaled : probe.fenceSignaled)) ++probe.invalidWaits;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL acquireImage(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence fence, uint32_t* index) {
    if (probe.acquireResult == VK_SUCCESS) {
        probe.acquireSignaled = fence != VK_NULL_HANDLE;
        *index = 0;
    }
    return probe.acquireResult;
}
VKAPI_ATTR VkResult VKAPI_CALL queuePresent(VkQueue, const VkPresentInfoKHR*) {
    ++probe.presents;
    if (probe.presentResult == VK_SUCCESS || probe.presentResult == VK_ERROR_OUT_OF_DATE_KHR) probe.fenceSignaled = true;
    return probe.presentResult;
}
VKAPI_ATTR VkResult VKAPI_CALL idle(VkDevice) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL surfaceCapabilities(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* output) {
    *output = {};
    if (probe.surfaceResult == VK_SUCCESS) {
        output->currentExtent = probe.surfaceExtent;
        output->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    return probe.surfaceResult;
}
VKAPI_ATTR VkResult VKAPI_CALL semaphoreValue(VkDevice, VkSemaphore, uint64_t* output) { *output = probe.retirement; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL waitSemaphores(VkDevice, const VkSemaphoreWaitInfo* info, uint64_t) {
    for (uint32_t i = 0; i < info->semaphoreCount; ++i)
        if (info->pValues[i] > probe.retirement) ++probe.invalidWaits;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL fenceStatus(VkDevice, VkFence) { return probe.fenceSignaled ? VK_SUCCESS : VK_NOT_READY; }
VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice, VkImageView, const VkAllocationCallbacks*) { ++probe.viewsDestroyed; }
VKAPI_ATTR void VKAPI_CALL destroySwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) { ++probe.swapchainsDestroyed; }
VKAPI_ATTR VkResult VKAPI_CALL createSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* output) {
    const auto result = probe.result();
    *output = handle<VkSemaphore>(result == VK_SUCCESS ? uintptr_t(300 + ++probe.semaphores) : 0xcdcd);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL createFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* output) {
    const auto result = probe.result();
    *output = handle<VkFence>(result == VK_SUCCESS ? uintptr_t(400 + ++probe.fences) : 0xcdcd);
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroySemaphore(VkDevice, VkSemaphore value, const VkAllocationCallbacks*) {
    if (value == handle<VkSemaphore>(0xcdcd)) ++probe.invalidWaits;
    else if (reinterpret_cast<uintptr_t>(value) >= 300) --probe.semaphores;
    ++probe.semaphoresDestroyed;
}
VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice, VkFence value, const VkAllocationCallbacks*) {
    if (value == handle<VkFence>(0xcdcd)) ++probe.invalidWaits;
    else if (reinterpret_cast<uintptr_t>(value) >= 400) --probe.fences;
}
VKAPI_ATTR VkResult VKAPI_CALL instanceVersion(uint32_t* output) { *output = VK_API_VERSION_1_4; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL instanceExtensions(const char*, uint32_t* count, VkExtensionProperties*) { *count = 0; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL instanceLayers(uint32_t* count, VkLayerProperties*) { *count = 0; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance* output) {
    *output = handle<VkInstance>(0xcdcd);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance, const VkAllocationCallbacks*) { ++probe.instancesDestroyed; }
}

#define WOBY_BACKEND_ALLOCATION_TESTS
namespace gpu::detail {
bool fail_backend_allocation() noexcept { return ++probe.cpuStep == probe.failCpuAt; }
}
#define vkAllocateCommandBuffers allocateCommands
#define vkFreeCommandBuffers freeCommands
#define vkDestroyCommandPool destroyPool
#define vkCreateQueryPool createQueries
#define vkDestroyQueryPool destroyQueries
#define vkResetQueryPool resetQueries
#define vkBeginCommandBuffer beginCommands
#define vkEndCommandBuffer endCommands
#define vkResetCommandPool resetPool
#define vkCmdPipelineBarrier2 pipelineBarrier
#define vkQueueSubmit2 queueSubmit
#define vkResetFences resetFences
#define vkWaitForFences waitFences
#define vkAcquireNextImageKHR acquireImage
#define vkQueuePresentKHR queuePresent
#define vkDeviceWaitIdle idle
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCapabilities
#define vkGetSemaphoreCounterValue semaphoreValue
#define vkWaitSemaphores waitSemaphores
#define vkGetFenceStatus fenceStatus
#define vkDestroyImageView destroyView
#define vkDestroySwapchainKHR destroySwapchain
#define vkDestroySemaphore destroySemaphore
#define vkDestroyFence destroyFence
#define vkCreateSemaphore createSemaphore
#define vkCreateFence createFence
#define vkEnumerateInstanceVersion instanceVersion
#define vkEnumerateInstanceExtensionProperties instanceExtensions
#define vkEnumerateInstanceLayerProperties instanceLayers
#define vkCreateInstance createInstance
#define vkDestroyInstance destroyInstance
#include <NoGraphicsAPI.cpp>

namespace {
struct CommandsFixture {
    gpu::Device device{};
    gpu::detail::Queue queue{};
    gpu::Swapchain swapchain{};
    gpu::TimelineSemaphore completion{.state = &device, .semaphore = handle<VkSemaphore>(8)};
    gpu::CommandPool* pool = new gpu::CommandPool{.state = &device, .command_pool = handle<VkCommandPool>(1), .timestamps = true};
    CommandsFixture() {
        device.device = handle<VkDevice>(1);
        device.queues = &queue;
        device.queue_count = 1;
        device.timestamp_query_count = 4;
        device.max_timeline_value_difference = UINT64_MAX;
    }
    void window() {
        device.swapchain = &swapchain;
        device.present_context_count = 1;
        device.presentation_retirement = handle<VkSemaphore>(9);
        device.present_contexts[0] = {.acquired = handle<VkSemaphore>(2), .rendered = handle<VkSemaphore>(3), .presented = handle<VkFence>(4), .acquisition_complete = handle<VkFence>(10)};
        swapchain.state = &device;
        swapchain.handle = handle<VkSwapchainKHR>(5);
        swapchain.image_count = 1;
        swapchain.width = swapchain.height = 32;
        swapchain.images[0] = handle<VkImage>(6);
        swapchain.render_views[0] = {.state = &device, .view = handle<VkImageView>(7), .width = 32, .height = 32};
    }
    ~CommandsFixture() {
        gpu::abandon_acquired_image(&device);
        device.drain_contexts();
        if (swapchain.handle) gpu::retire_swapchain_handle(swapchain);
        device.swapchain_delete_queue.collect({}, probe.retirement);
        gpu::destroy_command_pool(pool);
        free(queue.command_submit_infos);
        free(queue.wait_submit_infos);
        device.queues = nullptr;
        device.queue_count = 0;
        device.swapchain = nullptr;
        device.presentation_retirement = {};
        device.present_context_count = 0;
        device.device = VK_NULL_HANDLE;
    }
};
void checkClean() {
    CHECK(probe.buffers == 0);
    CHECK(probe.queries == 0);
    CHECK(probe.invalidWaits == 0);
    CHECK(probe.semaphores == 0);
    CHECK(probe.fences == 0);
}
}

TEST_CASE("Vulkan command ownership and timestamp array failures unwind and allow retry") {
    for (int step = 1; step <= 3; ++step) {
        probe = {};
        {
            CommandsFixture fixture;
            probe.failCpuAt = step;
            CHECK(gpu::begin_commands(fixture.pool) == nullptr);
            CHECK(gpu::command_pool_error(fixture.pool) != nullptr);
            CHECK(fixture.pool->first == nullptr);
            CHECK(fixture.pool->last == nullptr);
            checkClean();
            probe.failCpuAt = 0;
            auto* commands = gpu::begin_commands(fixture.pool);
            REQUIRE(commands != nullptr);
            CHECK(gpu::end_commands(commands) == nullptr);
        }
        checkClean();
    }
}

TEST_CASE("Vulkan native command allocation query creation and begin failures never publish invalid handles") {
    for (const auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        for (int step = 1; step <= 3; ++step) {
            probe = {}; probe.failure = failure; probe.failAt = step;
            {
                CommandsFixture fixture;
                CHECK(gpu::begin_commands(fixture.pool) == nullptr);
                REQUIRE(gpu::command_pool_error(fixture.pool) != nullptr);
                CHECK(std::string_view(gpu::command_pool_error(fixture.pool)) == gpu::allocation_error(failure));
                probe.failAt = 0;
                CHECK(gpu::reset_command_pool(fixture.pool) == nullptr);
                auto* commands = gpu::begin_commands(fixture.pool);
                REQUIRE(commands != nullptr);
                CHECK(gpu::end_commands(commands) == nullptr);
            }
            checkClean();
        }
    }
}

TEST_CASE("Vulkan epilogue failures release acquired images without submitting or waiting on future values") {
    for (int step = 1; step <= 4; ++step) {
        probe = {};
        {
            CommandsFixture fixture;
            fixture.window();
            auto* commands = gpu::begin_commands(fixture.pool);
            REQUIRE(commands != nullptr);
            REQUIRE(gpu::acquire(commands).render_view != nullptr);
            probe.failAt = probe.step + step; // primary end, epilogue allocate/begin/end
            CHECK(gpu::end_commands(commands) != nullptr);
            const std::array commandList{commands};
        const gpu::SubmitDesc desc{.commands = commandList, .completion = {&fixture.completion, 1}};
            const auto result = gpu::submit_and_present(&fixture.device, desc);
            CHECK_FALSE(result.submitted);
            CHECK(result.error != nullptr);
            CHECK(fixture.device.presentation_retirement_value == 0);
            CHECK(probe.signaled == 0);
            gpu::abandon_acquired_image(&fixture.device);
            CHECK_FALSE(fixture.swapchain.acquired);
            CHECK(fixture.device.acquired_swapchain == nullptr);
            CHECK(commands->swapchain == nullptr);
            CHECK(probe.swapchainsDestroyed == 1);
            CHECK(probe.viewsDestroyed == 1);
            CHECK(probe.semaphoresDestroyed == 2);
            CHECK(gpu::reset_command_pool(fixture.pool) == nullptr);
        }
        checkClean();
    }
}

TEST_CASE("Vulkan submission storage preserves existing allocations and supports retry after each failure") {
    for (int step = 1; step <= 3; ++step) {
        probe = {};
        {
            CommandsFixture fixture;
            auto* commands = gpu::begin_commands(fixture.pool);
            REQUIRE(commands != nullptr);
            REQUIRE(gpu::end_commands(commands) == nullptr);
            fixture.queue.command_submit_infos = static_cast<VkCommandBufferSubmitInfo*>(malloc(sizeof(VkCommandBufferSubmitInfo)));
            fixture.queue.command_submit_capacity = 1;
            fixture.queue.wait_submit_infos = static_cast<VkSemaphoreSubmitInfo*>(malloc(sizeof(VkSemaphoreSubmitInfo)));
            fixture.queue.wait_submit_capacity = 1;
            auto* oldCommands = fixture.queue.command_submit_infos;
            auto* oldWaits = fixture.queue.wait_submit_infos;
            // Two distinct ended command buffers force both arrays to grow.
            auto* second = gpu::begin_commands(fixture.pool);
            REQUIRE(second != nullptr);
            REQUIRE(gpu::end_commands(second) == nullptr);
            gpu::CommandBuffer* buffers[]{commands, second};
            const gpu::TimelinePoint waits[]{{&fixture.completion, 0}, {&fixture.completion, 0}};
            const gpu::SubmitDesc desc{.commands = buffers, .waits = waits, .completion = {&fixture.completion, 1}};
            if (step < 3) probe.failCpuAt = probe.cpuStep + step;
            else probe.failAt = probe.step + 1;
            const auto failed = gpu::submit(&fixture.device, desc);
            CHECK_FALSE(failed.submitted);
            CHECK(failed.error != nullptr);
            CHECK_FALSE(commands->submitted);
            CHECK(probe.signaled == 0);
            if (step == 1) {
                CHECK(fixture.queue.command_submit_infos == oldCommands);
                CHECK(fixture.queue.command_submit_capacity == 1);
            }
            if (step <= 2) {
                CHECK(fixture.queue.wait_submit_infos == oldWaits);
                CHECK(fixture.queue.wait_submit_capacity == 1);
            }
            probe.failAt = probe.failCpuAt = 0;
            const auto retried = gpu::submit(&fixture.device, desc);
            CHECK(retried.submitted);
            CHECK(retried.error == nullptr);
            CHECK(commands->submitted);
            CHECK(second->submitted);
            CHECK(probe.signaled == 1);
        }
        checkClean();
    }
}

TEST_CASE("Vulkan presentation submission failure keeps acquired ownership and retirement until retry") {
    probe = {};
    {
        CommandsFixture fixture;
        fixture.window();
        auto* commands = gpu::begin_commands(fixture.pool);
        REQUIRE(commands != nullptr);
        REQUIRE(gpu::acquire(commands).render_view != nullptr);
        REQUIRE(gpu::end_commands(commands) == nullptr);
        const std::array commandList{commands};
            const gpu::SubmitDesc desc{.commands = commandList, .completion = {&fixture.completion, 1}};
        probe.failAt = probe.step + 1;
        CHECK_FALSE(gpu::submit_and_present(&fixture.device, desc).submitted);
        CHECK(fixture.swapchain.acquired);
        CHECK(fixture.device.presentation_retirement_value == 0);
        probe.failAt = 0;
        const auto retry = gpu::submit_and_present(&fixture.device, desc);
        CHECK(retry.submitted);
        CHECK(retry.error == nullptr);
        CHECK_FALSE(fixture.swapchain.acquired);
        CHECK(fixture.device.presentation_retirement_value == 1);
        CHECK(probe.fenceWaits == 0); // successful frames do not wait for acquisition on the CPU
    }
    checkClean();
}

TEST_CASE("Vulkan present OOM reports successful submission without waiting for an unenqueued present") {
    for (const auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        probe = {}; probe.presentResult = failure;
        {
            CommandsFixture fixture;
            fixture.window();
            auto* commands = gpu::begin_commands(fixture.pool);
            REQUIRE(commands != nullptr);
            REQUIRE(gpu::acquire(commands).render_view != nullptr);
            REQUIRE(gpu::end_commands(commands) == nullptr);
            const auto result = gpu::submit_and_present(&fixture.device,
                {.commands = {commands}, .completion = {&fixture.completion, 1}});
            CHECK(result.submitted);
            CHECK(result.error != nullptr);
            CHECK(probe.signaled == 1);
            CHECK(fixture.device.presentation_retirement_value == 1);
            CHECK_FALSE(fixture.device.present_contexts[0].present_pending);
            CHECK(fixture.device.acquired_swapchain == nullptr);
            CHECK(fixture.swapchain.recreate_required);
        }
        checkClean();
    }
}

TEST_CASE("Vulkan acquire failure leaves no acquired image or pending fence") {
    probe = {}; probe.acquireResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    {
        CommandsFixture fixture;
        fixture.window();
        auto* commands = gpu::begin_commands(fixture.pool);
        REQUIRE(commands != nullptr);
        const auto acquired = gpu::acquire(commands);
        CHECK(acquired.error != nullptr);
        CHECK(acquired.render_view == nullptr);
        CHECK(fixture.device.acquired_swapchain == nullptr);
        CHECK(commands->swapchain == nullptr);
    }
    checkClean();
}

TEST_CASE("Vulkan retirement growth failure preserves the ring and destroys an entire fallback group") {
    probe = {};
    {
        CommandsFixture fixture;
        auto& ring = fixture.device.swapchain_delete_queue;
        REQUIRE(ring.reserve(2));
        ring.push(1, handle<VkSwapchainKHR>(11), handle<VkImageView>(12));
        ring.push(2, handle<VkSwapchainKHR>(13), handle<VkImageView>(14));
        ring.collect({}, 1); // wrap the next append
        ring.push(3, handle<VkSwapchainKHR>(15), handle<VkImageView>(16));
        auto* entries = ring.entries;
        const auto capacity = ring.capacity;
        probe.failCpuAt = probe.cpuStep + 1;
        CHECK_FALSE(ring.reserve(1));
        CHECK(ring.entries == entries);
        CHECK(ring.capacity == capacity);
        CHECK(ring.count == 2);
        REQUIRE(ring.reserve(1));
        ring.push(4, handle<VkSwapchainKHR>(17), handle<VkImageView>(18));
        probe.retirement = 4;
        fixture.device.presentation_retirement_value = 4;
        ring.collect({}, 4);
        CHECK(probe.viewsDestroyed == 4);
        CHECK(probe.swapchainsDestroyed == 4);
        gpu::detail::RetiredSwapchain retired{.handle = handle<VkSwapchainKHR>(20), .view_count = 5};
        for (uint32_t i = 0; i < retired.view_count; ++i) retired.views[i] = handle<VkImageView>(uintptr_t(21 + i));
        probe.failCpuAt = probe.cpuStep + 1;
        fixture.device.queue_retired_swapchain(retired);
        CHECK(retired.handle == VK_NULL_HANDLE);
        CHECK(ring.count == 0);
        CHECK(probe.viewsDestroyed == 9);
        CHECK(probe.swapchainsDestroyed == 5);
    }
    checkClean();
}

TEST_CASE("Vulkan device CPU ownership failure returns a diagnostic instead of terminating") {
    probe = {}; probe.failCpuAt = 1;
    const char* detail = nullptr;
    gpu::DeviceDesc desc;
    desc.diagnostic_context = &detail;
    desc.diagnostic = [](void* context, const gpu::DeviceDiagnostic& diagnostic) noexcept {
        *static_cast<const char**>(context) = diagnostic.detail;
    };
    const auto result = gpu::create_device(desc);
    CHECK(result.device == nullptr);
    CHECK(result.error == gpu::Error::driver_error);
    REQUIRE(detail != nullptr);
    CHECK(std::string_view(detail).find("Insufficient CPU memory") != std::string_view::npos);
}

TEST_CASE("Vulkan partial presentation context creation discards poisoned failure outputs") {
    for (int step = 1; step <= 4; ++step) {
        probe = {}; probe.failAt = step;
        {
            CommandsFixture fixture;
            gpu::detail::PresentContext context;
            CHECK(fixture.device.create_present_context(context) == gpu::Error::driver_error);
            CHECK(context.acquired == VK_NULL_HANDLE);
            CHECK(context.rendered == VK_NULL_HANDLE);
            CHECK(context.presented == VK_NULL_HANDLE);
            CHECK(context.acquisition_complete == VK_NULL_HANDLE);
            checkClean();
            probe.failAt = 0;
            REQUIRE(fixture.device.create_present_context(context) == gpu::Error::none);
            fixture.device.destroy_present_context(context);
        }
        checkClean();
    }
}

TEST_CASE("Vulkan failed instance creation never destroys its undefined output handle") {
    probe = {};
    const auto result = gpu::create_device({});
    CHECK(result.device == nullptr);
    CHECK(result.error == gpu::Error::driver_error);
    CHECK(probe.instancesDestroyed == 0);
    checkClean();
}

TEST_CASE("Vulkan surface query failures defer to checked recreation without aborting") {
    for (const auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        probe = {}; probe.surfaceResult = failure;
        {
            CommandsFixture fixture;
            fixture.window();
            CHECK(gpu::swapchain_surface_configuration_changed(fixture.swapchain));
            const auto extent = gpu::get_drawable_extent(&fixture.device);
            CHECK(extent.x == 32);
            CHECK(extent.y == 32);
            CHECK(fixture.swapchain.recreate_required);
            probe.surfaceResult = VK_SUCCESS;
            probe.surfaceExtent = {64, 48};
            const auto recovered = gpu::get_drawable_extent(&fixture.device);
            CHECK(recovered.x == 64);
            CHECK(recovered.y == 48);
        }
        checkClean();
    }
}
