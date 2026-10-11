#include "graphics.h"
#include <array>
#include <limits>
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
    g::destroy(texture);
    g::destroy(depth);
}

TEST_CASE("Typed graphics parameters reject invalid GPU selectors")
{
    GraphicsFixture fixture;
    namespace g = woby::graphics;
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS(g::setPointParameters({nan, 16, 16}));
    CHECK_THROWS(g::setTriangleEdges({-1, true, 0}));
    CHECK_THROWS(g::setMarkerQuery({nan, 0, 16, 16}));
    CHECK_THROWS(g::setMarkerQuery({0, 0, 0, 16}));
    CHECK_THROWS(g::setMarkerOptions({2, 8}));
    CHECK_THROWS(g::setMarkerOptions({4, nan}));
    for (const float metric : {-1.0f, .5f, 4.0f, nan}) {
        CHECK_THROWS(g::setComparison({metric, 1, 2, 0}));
    }
    CHECK_NOTHROW(g::setComparison({3, 1, 2, 0}));
    CHECK_NOTHROW(g::setPointParameters({1, 0, 0}));
}

TEST_CASE("Typed sprite and edge offsets are checked against retained storage at submission")
{
    GraphicsFixture fixture;
    namespace g = woby::graphics;
    const auto program = [](const char* entry) {
        const uint32_t code = 0;
        const auto vertex = g::createShader(g::copy(&code, sizeof(code)));
        const auto fragment = g::createShader(g::copy(&code, sizeof(code)));
        g::setName(vertex, entry);
        return g::createProgram(vertex, fragment, true);
    };
    const auto points = program("vs_marker_point");
    const auto lines = program("vs_line_sprite");
    const auto edges = program("vs_mesh_edges");
    const auto edgeLines = program("vs_triangle_lines");
    const std::array<float, 12> positions{};
    const std::array<uint32_t, 6> ids{0, 1, 2, 0, 2, 1};
    const auto vertices = g::createVertexBuffer(g::copy(positions.data(), sizeof(positions)), {16});
    const auto indices = g::createIndexBuffer(g::copy(ids.data(), sizeof(ids)), WOBY_GPU_BUFFER_INDEX32);
    const auto bind = [&] {
        g::setBuffer(0, vertices, g::Access::Read);
        g::setBuffer(1, indices, g::Access::Read);
    };
    for (const bool compact : {false, true}) {
        bind(); g::setVertexCount(4); g::setInstanceCount(1);
        g::setPointParameters({8, 16, 16, UINT32_MAX, 16777217, compact});
        CHECK_THROWS_WITH(g::submit(0, points), "Sprite storage range is out of bounds");
        g::setPointParameters({8, 16, 16, compact ? 2u : 5u, 16777217, compact});
        CHECK_NOTHROW(g::submit(0, points));
        // A second draw must set its own viewport and retained storage.
        bind(); g::setVertexCount(4);
        CHECK_THROWS_WITH(g::submit(0, points), "Invalid sprite draw parameters");
        g::setPointParameters({8, 16, 16, 0, 0, compact});
        CHECK_NOTHROW(g::submit(0, points));
    }
    bind(); g::setVertexCount(4); g::setInstanceCount(2);
    g::setPointParameters({8, 16, 16}); g::setMarkerBase(UINT32_MAX);
    CHECK_THROWS_WITH(g::submit(0, points), "Marker ID range is out of bounds");
    g::setInstanceCount(1); CHECK_NOTHROW(g::submit(0, points));

    bind(); g::setVertexCount(4); g::setInstanceCount(2);
    g::setPointParameters({1, 16, 16, 3});
    CHECK_THROWS_WITH(g::submit(0, lines), "Sprite storage range is out of bounds");
    g::setPointParameters({1, 16, 16, 2}); CHECK_NOTHROW(g::submit(0, lines));

    for (const auto draw : {edges, edgeLines}) {
        bind(); g::setVertexCount(draw.idx == edges.idx ? 3 : 6);
        g::setTriangleEdges({.5f, true, UINT32_MAX});
        CHECK_THROWS_WITH(g::submit(0, draw), "Triangle edge storage range is out of bounds");
        g::setTriangleEdges({.5f, true, 3}); CHECK_NOTHROW(g::submit(0, draw));
    }
}
