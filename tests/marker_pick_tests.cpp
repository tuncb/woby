#include "marker_pick.h"

#include <doctest/doctest.h>
#include <limits>

namespace {
std::array<float, 16> translated(float x, float y, float z)
{
    return {1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1};
}
}

TEST_CASE("Marker readback preserves the full 32 bit identity and rejects invalid pixels")
{
    for (uint32_t id : {0u, 1u, 65535u, 65536u, 16777217u, 40000001u, 0xffffffffu}) {
        CHECK(woby::decodeMarkerPixel({static_cast<float>(id & 65535u), static_cast<float>(id >> 16u), 0, 0}) == id);
    }
    CHECK_FALSE(woby::decodeMarkerPixel({1.5f, 0, 0, 0}));
    CHECK_FALSE(woby::decodeMarkerPixel({-1, 0, 0, 0}));
    CHECK_FALSE(woby::decodeMarkerPixel({0, 65536, 0, 0}));
    CHECK_FALSE(woby::decodeMarkerPixel({std::numeric_limits<float>::quiet_NaN(), 0, 0, 0}));
}

TEST_CASE("Marker draw identities distinguish models and repeated instances without an index")
{
    woby::MarkerDrawList list;
    woby::MarkerDraw a{translated(1, 2, 3), 0, 12, 4, 0, 10};
    auto b = a; b.fileIndex = 1; b.fileId = 20;
    auto instance = a; instance.model = translated(4, 5, 6);
    CHECK(woby::appendMarkerDraw(list, a, 4) == 1);
    CHECK(woby::appendMarkerDraw(list, b, 8) == 5);
    CHECK(woby::appendMarkerDraw(list, instance, 6) == 9);
    REQUIRE(list.draws.size() == 3);
    CHECK(woby::findMarkerDraw(list.draws, 8) == &list.draws[1]);
    CHECK(woby::findMarkerDraw(list.draws, 9) == &list.draws[2]);
    CHECK_FALSE(woby::findMarkerDraw(list.draws, 0));
    CHECK_FALSE(woby::findMarkerDraw(list.draws, 13));
    CHECK(list.largestPoint == 8);
    list.nextId = 0xffffffffu;
    CHECK(woby::appendMarkerDraw(list, a, 40) == 0);
    CHECK(list.draws.size() == 3);
    a.count = 1;
    CHECK(woby::appendMarkerDraw(list, a, 4) == 0xffffffffu);
    CHECK(woby::appendMarkerDraw(list, a, 4) == 0);
    a.count = 0;
    CHECK(woby::appendMarkerDraw(list, a, 4) == 0);
}

TEST_CASE("Marker coordinates use the selected file and the submitted instance transform")
{
    woby::UiState state;
    state.files.resize(2);
    std::vector<woby::LoadedModelRuntime> runtimes(2);
    for (size_t i = 0; i < 2; ++i) {
        state.files[i].objectId = 10 + i;
        state.files[i].mesh.vertices.resize(2);
        state.files[i].mesh.vertices[1].position = {static_cast<float>(i), 2, 3};
        runtimes[i].gpuMesh.pointVertexIndices = {0, 1};
    }
    std::vector<woby::MarkerDraw> draws = {{translated(10, 20, 30), 100, 1, 1, 1, 11}};
    const auto hit = woby::resolveMarkerCoordinates(100, draws, state, runtimes);
    REQUIRE(hit);
    CHECK(hit->localPosition == std::array<float, 3>{1, 2, 3});
    CHECK(hit->transformedPosition == std::array<float, 3>{11, 22, 33});
    // An in-flight snapshot is unaffected by subsequent transform edits.
    state.files[1].fileSettings.translation = {-100, 0, 0};
    CHECK(woby::resolveMarkerCoordinates(100, draws, state, runtimes)->transformedPosition == hit->transformedPosition);
    state.files.erase(state.files.begin());
    CHECK_FALSE(woby::resolveMarkerCoordinates(100, draws, state, runtimes));
    state.files.resize(2); state.files[1].objectId = 99;
    CHECK_FALSE(woby::resolveMarkerCoordinates(100, draws, state, runtimes));
    CHECK_FALSE(woby::resolveMarkerCoordinates(0, draws, state, runtimes));
}

TEST_CASE("Marker coordinates reject missing or replaced point storage")
{
    woby::UiState state; state.files.resize(1); state.files[0].objectId = 2;
    std::vector<woby::LoadedModelRuntime> runtimes(1);
    std::vector<woby::MarkerDraw> draws = {{translated(0, 0, 0), 1, 0, 1, 0, 2}};
    CHECK_FALSE(woby::resolveMarkerCoordinates(1, draws, state, runtimes));
    runtimes[0].gpuMesh.pointVertexIndices = {100};
    CHECK_FALSE(woby::resolveMarkerCoordinates(1, draws, state, runtimes));
    CHECK_FALSE(woby::resolveMarkerCoordinates(1, draws, state, {}));
}

TEST_CASE("Marker asynchronous completion rejects canceled and out of order results across frame wrap")
{
    CHECK(woby::acceptMarkerCompletion(2, 2, 10, 9));
    CHECK_FALSE(woby::acceptMarkerCompletion(1, 2, 10, 9));
    CHECK_FALSE(woby::acceptMarkerCompletion(2, 2, 9, 9));
    CHECK_FALSE(woby::acceptMarkerCompletion(2, 2, 8, 9));
    CHECK(woby::markerFrameReached(20, 20));
    CHECK_FALSE(woby::markerFrameReached(19, 20));
    CHECK(woby::markerFrameReached(1, 0xffffffffu));
    CHECK_FALSE(woby::markerFrameReached(0xffffffffu, 1));
}

TEST_CASE("Marker GPU capability checks retain fallback for unsupported backends")
{
    woby::graphics::Caps caps{};
    caps.rendererType = woby::graphics::RendererType::Vulkan;
    caps.supported = WOBY_GPU_CAPS_COMPUTE | WOBY_GPU_CAPS_TEXTURE_READ_BACK | WOBY_GPU_CAPS_TEXTURE_BLIT | WOBY_GPU_CAPS_BLEND_INDEPENDENT;
    caps.limits.maxFBAttachments = 2;
    CHECK(woby::supportsGpuMarkerPicking(caps));
    for (auto flag : {WOBY_GPU_CAPS_COMPUTE, WOBY_GPU_CAPS_TEXTURE_READ_BACK, WOBY_GPU_CAPS_TEXTURE_BLIT, WOBY_GPU_CAPS_BLEND_INDEPENDENT}) {
        auto missing = caps; missing.supported &= ~flag;
        CHECK_FALSE(woby::supportsGpuMarkerPicking(missing));
    }
    caps.originBottomLeft = true;
    CHECK_FALSE(woby::supportsGpuMarkerPicking(caps));
    caps.originBottomLeft = false; caps.limits.maxFBAttachments = 1;
    CHECK_FALSE(woby::supportsGpuMarkerPicking(caps));
}
