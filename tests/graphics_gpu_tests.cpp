#include "graphics.h"
#include "graphics_helpers.h"
#include <array>
#include <doctest/doctest.h>

namespace
{
struct NativeFixture
{
    bool initialized = false;
    ~NativeFixture()
    {
        if (initialized)
            woby::graphics::shutdown();
    }
};
} // namespace

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
