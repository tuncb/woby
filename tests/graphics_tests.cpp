#include "graphics.h"
#include <array>
#include <doctest/doctest.h>

namespace
{
struct GraphicsFixture
{
    GraphicsFixture()
    {
        woby::graphics::Init options;
        options.type = woby::graphics::RendererType::Noop;
        REQUIRE(woby::graphics::init(options));
    }
    ~GraphicsFixture()
    {
        woby::graphics::shutdown();
    }
};
} // namespace

TEST_CASE("Graphics buffers validate layout and offsets and reject stale handles")
{
    GraphicsFixture fixture;
    namespace g = woby::graphics;
    const std::array<float, 6> vertices{};
    CHECK_THROWS(g::createVertexBuffer(g::copy(vertices.data(), sizeof(vertices)), {0}));
    CHECK_THROWS(g::createVertexBuffer(g::copy(vertices.data(), sizeof(vertices) - 1), {12}));
    const auto first = g::createVertexBuffer(g::copy(vertices.data(), sizeof(vertices)), {12});
    CHECK_NOTHROW(g::setVertexBuffer(0, first, 1, 1));
    CHECK_THROWS(g::setVertexBuffer(0, first, 3));
    g::destroy(first);
    const auto second = g::createVertexBuffer(g::copy(vertices.data(), sizeof(vertices)), {12});
    CHECK(first.idx != second.idx);
    CHECK_THROWS(g::setVertexBuffer(0, first));
    g::destroy(second);
    for (int i = 0; i < 8; ++i)
        CHECK(g::frame() == static_cast<uint32_t>(i + 1));
}

TEST_CASE("Graphics textures reject invalid updates and incompatible attachments")
{
    GraphicsFixture fixture;
    namespace g = woby::graphics;
    const std::array<uint8_t, 16> pixels{};
    const auto texture = g::createTexture2D(2, 2, false, 1, g::TextureFormat::RGBA8, WOBY_GPU_TEXTURE_RT);
    CHECK_THROWS(g::updateTexture2D(texture, 0, 0, 1, 1, 2, 2, g::copy(pixels.data(), sizeof(pixels))));
    CHECK_THROWS(g::updateTexture2D(texture, 0, 0, 0, 0, 2, 2, g::copy(pixels.data(), sizeof(pixels)), 4));
    CHECK_NOTHROW(g::updateTexture2D(texture, 0, 0, 1, 0, 1, 2, g::copy(pixels.data(), sizeof(pixels)), 8));
    const auto depth = g::createTexture2D(4, 4, false, 1, g::TextureFormat::D24S8, WOBY_GPU_TEXTURE_RT);
    const std::array attachments{texture, depth};
    CHECK_THROWS(g::createFrameBuffer(2, attachments.data()));
    const auto uniform = g::createUniform("u_color", g::UniformType::Vec4, 2);
    const std::array<float, 8> value{};
    CHECK_THROWS(g::setUniform(uniform, value.data(), 2));
    CHECK_NOTHROW(g::setUniform(uniform, value.data()));
    g::destroy(uniform);
    g::destroy(texture);
    g::destroy(depth);
}
