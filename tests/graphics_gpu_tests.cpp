#include "graphics.h"
#include "graphics_helpers.h"
#include "scene_pick.h"
#include "comparison_view.h"
#include "ui_operations.h"
#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <thread>
#include <doctest/doctest.h>

namespace
{
struct NativeFixture
{
    bool initialized = false;
    ~NativeFixture()
    {
        gpu::fail_heap_allocation_for_test(nullptr, 0);
        if (initialized)
            woby::graphics::shutdown();
    }
};
} // namespace

TEST_CASE("Native heap allocation failures report the Vulkan operation and remain recoverable")
{
    NativeFixture fixture;
    fixture.initialized = woby::graphics::init({});
    REQUIRE(fixture.initialized);
    const auto device = gpu::create_device({});
    REQUIRE(device.device != nullptr);
    // These are real Vulkan errors; the failing call is skipped so no large
    // allocation or exhausted machine is needed to exercise each cleanup path.
    for (const auto result : {-1, -2, -4, -10}) { // host/device OOM, device lost, too many objects
        for (const auto* operation : {"vkCreateBuffer", "vkAllocateMemory", "vkBindBufferMemory", "vkMapMemory"}) {
            gpu::HeapAllocationFailure failure;
            gpu::fail_heap_allocation_for_test(operation, result);
            const auto failed = gpu::try_create_gpu_heap(device.device, 4096, gpu::MemoryType::cpu_visible, failure);
            CHECK(failed.owner == nullptr);
            CHECK(failure.api_result == result);
            CHECK(std::string(failure.operation ? failure.operation : "") == operation);
            CHECK(failure.allocation_bytes >= 4096);
            gpu::destroy_gpu_heap(failed);
            const auto recovered = gpu::try_create_gpu_heap(device.device, 4096, gpu::MemoryType::cpu_visible, failure);
            CHECK(recovered.owner != nullptr);
            CHECK(failure.operation == nullptr);
            gpu::destroy_gpu_heap(recovered);
        }
    }
    gpu::destroy_device(device.device);
}

TEST_CASE("Native buffer allocation errors release borrowed input and allow subsequent uploads")
{
    namespace g = woby::graphics;
    NativeFixture fixture;
    fixture.initialized = g::init({});
    REQUIRE(fixture.initialized);
    std::array<float, 6> vertices{};
    bool released = false;
    gpu::fail_heap_allocation_for_test("vkAllocateMemory", -2);
    CHECK_THROWS_WITH(g::createVertexBuffer(g::makeRef(vertices.data(), sizeof(vertices),
        [](void*, void* context) { *static_cast<bool*>(context) = true; }, &released), {12}),
        doctest::Contains("requested 24 bytes; vkAllocateMemory returned VkResult -2"));
    CHECK(released);
    const auto buffer = g::createVertexBuffer(g::copy(vertices.data(), sizeof(vertices)), {12});
    CHECK(g::isValid(buffer));
    g::destroy(buffer); // Canceled before drawing: submitted uploads still own it.
    for (size_t i = 0; i < 5; ++i) { g::frame(); }
}

TEST_CASE("Native comparison distance upload failures clean up both sides and support retry")
{
    using namespace woby;
    namespace g = woby::graphics;
    NativeFixture fixture;
    fixture.initialized = g::init({});
    REQUIRE(fixture.initialized);
    struct ComparisonFixture {
        UiState state;
        ComparisonRuntimes runtimes;
        std::filesystem::path root = std::filesystem::temp_directory_path()
            / ("woby-gpu-comparison-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ComparisonFixture() { std::filesystem::create_directory(root); }
        ~ComparisonFixture() {
            destroyComparisonRuntimes(runtimes);
            std::error_code ignored; std::filesystem::remove_all(root, ignored);
        }
    } comparison;
    Mesh mesh;
    mesh.vertices.resize(3);
    mesh.vertices[1].position[0] = 1;
    mesh.vertices[2].position[1] = 1;
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"triangle", 0, 3});
    captureSourceMesh(mesh, SourceProvenance::objPositions);
    finalizeMesh(mesh, true);
    comparison.state.files.push_back(createUiFileState(comparison.root / "triangle.obj", mesh, 0));
    appendDefaultSceneNodesForFiles(comparison.state, 0);
    const auto id = createComparison(comparison.state);
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        setComparisonObjects(comparison.state, {comparison.state.files[0].objectId}, side, true, id);
    }
    auto& runtime = comparison.runtimes.objects[id];
    runtime.fullResultsRequested = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!comparisonResultsReady(runtime, comparison.state, id, true) && runtime.jobs.error.empty()
        && std::chrono::steady_clock::now() < deadline) {
        updateComparisonRuntimes(comparison.runtimes, comparison.state);
        g::frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    INFO(runtime.jobs.error);
    REQUIRE(comparisonResultsReady(runtime, comparison.state, id, true));
    for (const auto skip : {0u, 1u}) {
        for (auto* surface : {&runtime.gpu.original, &runtime.gpu.repaired}) {
            g::destroy(surface->samples);
            surface->samples = WOBY_GPU_INVALID_HANDLE;
        }
        runtime.gpu.uploadedStages &= ~comparisonDistance;
        gpu::fail_heap_allocation_for_test("vkAllocateMemory", -2, skip);
        CHECK_NOTHROW(updateComparisonRuntimes(comparison.runtimes, comparison.state));
        CHECK((runtime.jobs.failedStages & comparisonDistance) != 0);
        CHECK((runtime.gpu.uploadedStages & comparisonDistance) == 0);
        CHECK_FALSE(g::isValid(runtime.gpu.original.samples));
        CHECK_FALSE(g::isValid(runtime.gpu.repaired.samples));
        CHECK(runtime.jobs.error.find("VkResult -2") != std::string::npos);
        CHECK_FALSE(comparisonResultsReady(runtime, comparison.state, id, true));
        for (size_t i = 0; i < 5; ++i) { g::frame(); }
        // The same state changes as the UI Retry button; CPU results survive.
        runtime.attemptedSignature = 0;
        runtime.jobs.failedStages = 0;
        runtime.jobs.error.clear();
        updateComparisonRuntimes(comparison.runtimes, comparison.state);
        CHECK(comparisonResultsReady(runtime, comparison.state, id, true));
    }
    // Removing an analysis with uploads pending must also be safe.
    removeComparison(comparison.state, id);
    updateComparisonRuntimes(comparison.runtimes, comparison.state);
    CHECK(comparison.runtimes.objects.empty());
    for (size_t i = 0; i < 5; ++i) { g::frame(); }
}

TEST_CASE("Native headless devices never enable mailbox presentation")
{
    const auto created = gpu::create_device({.allow_mailbox_presentation = true});
    REQUIRE(created.device != nullptr);
    CHECK_FALSE(gpu::supports_mailbox_presentation(created.device));
    gpu::set_mailbox_presentation(created.device, true);
    CHECK_FALSE(gpu::supports_mailbox_presentation(created.device));
    gpu::set_mailbox_presentation(created.device, false);
    gpu::destroy_device(created.device);
}

TEST_CASE("Native renderer submits empty frames before geometry exists")
{
    namespace g = woby::graphics;
    NativeFixture fixture;
    fixture.initialized = g::init({});
    REQUIRE(fixture.initialized);
    // Exercise submission and command-pool reuse without scene or upload work.
    // Run this in Release too: dangling submission spans can survive in Debug.
    for (uint32_t expected = 1; expected <= 16; ++expected)
        CHECK(g::frame() == expected);
}

TEST_CASE("Native draws retain texture versions and geometry until GPU completion")
{
    namespace g = woby::graphics;
    std::array<uint8_t, 8 * 8 * 4> pixels{};
    NativeFixture fixture;
    fixture.initialized = g::init({});
    REQUIRE(fixture.initialized);
    const auto program =
        woby::loadProgram(WOBY_TEST_ASSET_DIRECTORY, "vs_marker_screen.bin", "fs_marker_composite.bin");
    const auto sampler = g::createUniform("s_tex", g::UniformType::Sampler);
    const std::array<uint8_t, 4> red{255, 0, 0, 255}, blue{0, 0, 255, 255};
    const auto texture = g::createTexture2D(1, 1, false, 1, g::TextureFormat::RGBA8, 0, g::copy(red.data(), 4));
    const auto output =
        g::createTexture2D(8, 8, false, 1, g::TextureFormat::RGBA8, WOBY_GPU_TEXTURE_RT | WOBY_GPU_TEXTURE_READ_BACK);
    const auto target = g::createFrameBuffer(1, &output);
    g::setViewFrameBuffer(0, target);
    g::setViewRect(0, 0, 0, 8, 8);
    g::setViewClear(0, WOBY_GPU_CLEAR_COLOR, 0x00ff00ff);
    // Record a red draw, publish a blue atlas version, then record a blue draw.
    // Both are submitted together; each must observe its own descriptor version.
    for (int i = 0; i < 2; ++i)
    {
        if (i)
            g::updateTexture2D(texture, 0, 0, 0, 0, 1, 1, g::copy(blue.data(), 4));
        g::setTexture(0, sampler, texture);
        g::setScissor(static_cast<uint16_t>(i * 4), 0, 4, 8);
        g::setVertexCount(4);
        g::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_PT_TRISTRIP);
        g::submit(0, program);
    }
    // Cancel an uploaded, never-submitted odd-sized index allocation.
    const std::array<uint16_t, 3> indices{0, 1, 2};
    g::destroy(g::createIndexBuffer(g::copy(indices.data(), sizeof(indices))));
    g::destroy(texture);
    g::destroy(sampler);
    const auto ready = g::readTexture(output, pixels.data());
    while (g::frame() < ready)
    {
    }
    for (size_t y = 0; y < 8; ++y)
        for (size_t x = 0; x < 8; ++x)
        {
            const auto &expected = x < 4 ? red : blue;
            for (size_t channel = 0; channel < 4; ++channel)
                CHECK(pixels[(y * 8 + x) * 4 + channel] == expected[channel]);
        }
    g::destroy(target);
    g::destroy(output);
    g::destroy(program);
    for (int i = 0; i < 5; ++i)
        g::frame();
}

TEST_CASE("Native circular markers use pixel coverage consistently for display and picking")
{
    namespace g = woby::graphics;
    NativeFixture fixture;
    fixture.initialized = g::init({});
    REQUIRE(fixture.initialized);
    const std::array<float, 3> position{0, 0, .5f};
    const std::array<uint32_t, 1> pointId{0};
    const auto vertices = g::createVertexBuffer(g::copy(position.data(), sizeof(position)), {12},
        WOBY_GPU_BUFFER_COMPUTE_READ);
    const auto indices = g::createIndexBuffer(g::copy(pointId.data(), sizeof(pointId)),
        WOBY_GPU_BUFFER_INDEX32 | WOBY_GPU_BUFFER_COMPUTE_READ);
    const auto color = g::createUniform("u_color", g::UniformType::Vec4);
    const auto parameters = g::createUniform("u_pointParams", g::UniformType::Vec4, 2);
    const std::array<float, 4> red{1, 0, 0, 1};
    const std::array<float, 8> pointParameters{8, 16, 16, 0, 0, 0, 0, 0};
    for (const bool multisample : {false, true})
        for (const bool markerIds : {false, true})
        {
            CAPTURE(multisample);
            CAPTURE(markerIds);
            const uint64_t flags = multisample ? WOBY_GPU_TEXTURE_RT_MSAA_X4 : WOBY_GPU_TEXTURE_RT;
            const auto output = g::createTexture2D(16, 16, false, 1, g::TextureFormat::RGBA8,
                flags | WOBY_GPU_TEXTURE_READ_BACK);
            // Keep the picking attachment unresolved, as in the production path.
            const auto ids = markerIds ? g::createTexture2D(16, 16, false, 1, g::TextureFormat::RGBA8,
                flags | (multisample ? WOBY_GPU_TEXTURE_MSAA_SAMPLE : uint64_t{0})) : g::TextureHandle{};
            const std::array attachments{output, ids};
            const auto target = g::createFrameBuffer(markerIds ? 2 : 1, attachments.data());
            const auto program = woby::loadProgram(WOBY_TEST_ASSET_DIRECTORY,
                markerIds ? "vs_marker_point.bin" : "vs_point_sprite.bin",
                markerIds ? "fs_marker_point.bin" : "fs_point_sprite.bin");
            g::setViewFrameBuffer(0, target);
            g::setViewRect(0, 0, 0, 16, 16);
            g::setViewClear(0, WOBY_GPU_CLEAR_COLOR, 0x000000ff);
            g::setUniform(color, red.data());
            g::setUniform(parameters, pointParameters.data(), 2);
            g::setBuffer(0, vertices, g::Access::Read);
            g::setBuffer(1, indices, g::Access::Read);
            g::setVertexCount(4);
            g::setInstanceCount(1);
            g::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_PT_TRISTRIP);
            g::submit(0, program);
            std::array<uint8_t, 16 * 16 * 4> pixels{};
            const auto ready = g::readTexture(output, pixels.data());
            while (g::frame() < ready) {}
            // This pixel-aligned 8-pixel circle must use the pixel center for
            // its cutout at both sample counts, including when hover picking
            // switches to the MRT shader. Per-sample UVs create partial pixels
            // at the circle boundary and fail these full-footprint assertions.
            for (size_t y = 0; y < 16; ++y)
                for (size_t x = 0; x < 16; ++x)
                {
                    CAPTURE(x);
                    CAPTURE(y);
                    const int dx = static_cast<int>(x * 2 + 1) - 16;
                    const int dy = static_cast<int>(y * 2 + 1) - 16;
                    const auto pixel = (y * 16 + x) * 4;
                    CHECK(pixels[pixel] == (dx * dx + dy * dy <= 64 ? 255 : 0));
                    CHECK(pixels[pixel + 1] == 0);
                    CHECK(pixels[pixel + 2] == 0);
                    CHECK(pixels[pixel + 3] == 255);
                }
            g::destroy(program);
            g::destroy(target);
            g::destroy(output);
            if (markerIds)
                g::destroy(ids);
        }
    g::destroy(parameters);
    g::destroy(color);
    g::destroy(indices);
    g::destroy(vertices);
}

TEST_CASE("Reversed depth resolves close and distant layers independent of draw order and MSAA")
{
    namespace g = woby::graphics;
    NativeFixture fixture;
    fixture.initialized = g::init({});
    REQUIRE(fixture.initialized);
    const auto program = woby::loadProgram(WOBY_TEST_ASSET_DIRECTORY, "vs_color.bin", "fs_color.bin");
    const auto color = g::createUniform("u_color", g::UniformType::Vec4);
    const std::array<float, 4> red{1, 0, 0, 1}, blue{0, 0, 1, 1};
    const std::array<float, 18> positions{
        -200000, -200000, .5f, 200000, -200000, .5f, 0, 200000, .5f,
        -200000, -200000, 0, 200000, -200000, 0, 0, 200000, 0};
    const auto vertices = g::createVertexBuffer(g::copy(positions.data(), sizeof(positions)), {12});
    woby::Bounds bounds; bounds.radius = 300000;
    for (const bool msaa : {false, true}) {
        const uint64_t flags = msaa ? WOBY_GPU_TEXTURE_RT_MSAA_X4 : WOBY_GPU_TEXTURE_RT;
        const auto output = g::createTexture2D(16, 16, false, 1, g::TextureFormat::RGBA8,
            flags | WOBY_GPU_TEXTURE_READ_BACK);
        const auto depth = g::createTexture2D(16, 16, false, 1, g::TextureFormat::D24S8, flags);
        const std::array attachments{output, depth};
        const auto target = g::createFrameBuffer(2, attachments.data());
        // Match the window path: one view clears and a later view loads depth.
        // Screenshot and marker passes also exercise clear-and-draw in one view.
        g::setViewFrameBuffer(0, target);
        g::setViewRect(0, 0, 0, 16, 16);
        g::setViewClear(0, WOBY_GPU_CLEAR_COLOR | WOBY_GPU_CLEAR_DEPTH, 0x000000ff, 1);
        g::setViewTransform(0, nullptr, nullptr, true);
        g::setViewFrameBuffer(1, target);
        g::setViewRect(1, 0, 0, 16, 16);
        g::setViewClear(1, WOBY_GPU_CLEAR_NONE);
        for (const float distance : {1.0f, 33000.0f, 80000.0f}) {
            for (const float angle : {0.0f, .4f}) {
                const auto camera = woby::cameraLookingAt({}, {distance * angle, distance * angle, distance},
                    {0, 0, 0}, woby::SceneUpAxis::y);
                const auto view = woby::scenePickView(camera, woby::SceneUpAxis::y, bounds, 16, 16, false, 1);
                g::setViewTransform(1, view.view.data(), view.renderProjection.data(), true);
                for (const bool frontFirst : {false, true}) {
                    for (const auto test : {WOBY_GPU_STATE_DEPTH_TEST_LESS, WOBY_GPU_STATE_DEPTH_TEST_LEQUAL}) {
                        CAPTURE(msaa); CAPTURE(distance); CAPTURE(angle); CAPTURE(frontFirst); CAPTURE(test);
                        g::touch(0);
                        for (int draw = 0; draw < 2; ++draw) {
                            const bool front = (draw == 0) == frontFirst;
                            g::setUniform(color, front ? red.data() : blue.data());
                            g::setVertexBuffer(0, vertices, front ? 0 : 3, 3);
                            g::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_WRITE_Z | test);
                            g::submit(1, program);
                        }
                        std::array<uint8_t, 16 * 16 * 4> pixels{};
                        const auto ready = g::readTexture(output, pixels.data());
                        while (g::frame() < ready) {}
                        for (size_t y = 4; y < 12; ++y) {
                            for (size_t x = 4; x < 12; ++x) {
                                const auto pixel = (y * 16 + x) * 4;
                                CHECK(pixels[pixel] == 255);
                                CHECK(pixels[pixel + 1] == 0);
                                CHECK(pixels[pixel + 2] == 0);
                            }
                        }
                    }
                }
            }
        }
        g::destroy(target); g::destroy(depth); g::destroy(output);
    }
    g::destroy(vertices); g::destroy(color); g::destroy(program);
}
