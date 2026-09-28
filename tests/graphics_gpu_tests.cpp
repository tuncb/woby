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
