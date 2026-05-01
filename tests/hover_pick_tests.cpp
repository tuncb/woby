#include "hover_pick.h"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <vector>

namespace {

constexpr std::array<float, 16> identityMatrix = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

woby::Vertex vertex(float x, float y, float z)
{
    woby::Vertex result;
    result.position = {x, y, z};
    result.normal = {0.0f, 0.0f, 1.0f};
    return result;
}

woby::Mesh pointMesh()
{
    woby::Mesh mesh;
    mesh.vertices = {
        vertex(0.0f, 0.0f, 0.2f),
        vertex(0.0f, 0.0f, -0.4f),
        vertex(0.6f, 0.6f, 0.0f),
    };
    mesh.indices = {0u, 1u, 2u};
    mesh.nodes.push_back({"points", 0u, 3u});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    return mesh;
}

woby::UiFileState pointFile()
{
    woby::UiFileState file;
    file.path = "points.obj";
    file.mesh = pointMesh();
    file.groupSettings.emplace_back();
    return file;
}

woby::LoadedModelRuntime pointRuntime(std::vector<uint32_t> pointIndices)
{
    woby::LoadedModelRuntime runtime;
    woby::GpuNodeRange range;
    range.pointIndexOffset = 0u;
    range.pointIndexCount = static_cast<uint32_t>(pointIndices.size());
    runtime.gpuMesh.nodeRanges.push_back(range);
    runtime.gpuMesh.pointVertexIndices = std::move(pointIndices);
    return runtime;
}

std::optional<woby::HoveredVertex> pick(
    const std::vector<woby::UiFileState>& files,
    const std::vector<woby::LoadedModelRuntime>& runtimes,
    float mouseX,
    float mouseY)
{
    return woby::findHoveredVertex(
        files,
        runtimes,
        {mouseX, mouseY},
        woby::defaultMasterVertexPointSize,
        identityMatrix.data(),
        identityMatrix.data(),
        100u,
        100u,
        true);
}

} // namespace

TEST_CASE("hover picking chooses the frontmost nearby vertex")
{
    std::vector<woby::UiFileState> files = {pointFile()};
    std::vector<woby::LoadedModelRuntime> runtimes = {pointRuntime({0u, 1u, 2u})};

    const auto hovered = pick(files, runtimes, 50.0f, 50.0f);

    REQUIRE(hovered.has_value());
    CHECK(hovered->localPosition[2] == doctest::Approx(-0.4f));
    CHECK(hovered->transformedPosition[2] == doctest::Approx(-0.4f));
    CHECK(hovered->distanceSquared == doctest::Approx(0.0f));
}

TEST_CASE("hover picking applies group transforms to hit testing")
{
    std::vector<woby::UiFileState> files = {pointFile()};
    files[0].groupSettings[0].translation = {0.2f, 0.0f, 0.0f};
    std::vector<woby::LoadedModelRuntime> runtimes = {pointRuntime({0u})};

    const auto hovered = pick(files, runtimes, 60.0f, 50.0f);

    REQUIRE(hovered.has_value());
    CHECK(hovered->localPosition[0] == doctest::Approx(0.0f));
    CHECK(hovered->transformedPosition[0] == doctest::Approx(0.2f));
}

TEST_CASE("hover picking ignores hidden transparent and invalid point ranges")
{
    std::vector<woby::UiFileState> files = {pointFile()};
    std::vector<woby::LoadedModelRuntime> runtimes = {pointRuntime({0u})};

    REQUIRE(pick(files, runtimes, 50.0f, 50.0f).has_value());

    files[0].fileSettings.visible = false;
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());

    files[0].fileSettings.visible = true;
    files[0].fileSettings.opacity = 0.0f;
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());

    files[0].fileSettings.opacity = 1.0f;
    files[0].groupSettings[0].visible = false;
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());

    files[0].groupSettings[0].visible = true;
    files[0].groupSettings[0].showVertices = false;
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());

    files[0].groupSettings[0].showVertices = true;
    files[0].groupSettings[0].opacity = 0.0f;
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());

    files[0].groupSettings[0].opacity = 1.0f;
    runtimes[0].gpuMesh.pointVertexIndices = {99u};
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());
}

TEST_CASE("hover picking returns no hit outside radius frustum or matched runtime data")
{
    std::vector<woby::UiFileState> files = {pointFile()};
    std::vector<woby::LoadedModelRuntime> runtimes = {pointRuntime({0u})};

    CHECK_FALSE(pick(files, runtimes, 90.0f, 90.0f).has_value());

    files[0].mesh.vertices[0].position = {2.0f, 0.0f, 0.0f};
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());

    files = {pointFile()};
    runtimes.clear();
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());

    runtimes = {pointRuntime({0u})};
    files[0].groupSettings.clear();
    CHECK_FALSE(pick(files, runtimes, 50.0f, 50.0f).has_value());
}

TEST_CASE("hover pick signatures change when hit-test inputs change")
{
    std::vector<woby::UiFileState> files = {pointFile()};
    std::vector<woby::LoadedModelRuntime> runtimes = {pointRuntime({0u})};
    const woby::SceneCamera camera = woby::frameCameraBounds(woby::defaultDisplayBounds());
    const woby::Bounds bounds = woby::defaultDisplayBounds();

    const uint64_t baseline = woby::hoverPickSignature(
        files,
        runtimes,
        {50.0f, 50.0f},
        true,
        woby::defaultMasterVertexPointSize,
        camera,
        woby::SceneUpAxis::z,
        bounds,
        100u,
        100u,
        true);

    const uint64_t movedMouse = woby::hoverPickSignature(
        files,
        runtimes,
        {51.0f, 50.0f},
        true,
        woby::defaultMasterVertexPointSize,
        camera,
        woby::SceneUpAxis::z,
        bounds,
        100u,
        100u,
        true);
    CHECK(movedMouse != baseline);

    files[0].groupSettings[0].showVertices = false;
    const uint64_t hiddenVertices = woby::hoverPickSignature(
        files,
        runtimes,
        {50.0f, 50.0f},
        true,
        woby::defaultMasterVertexPointSize,
        camera,
        woby::SceneUpAxis::z,
        bounds,
        100u,
        100u,
        true);
    CHECK(hiddenVertices != baseline);

    files[0].groupSettings[0].showVertices = true;
    runtimes[0].gpuMesh.pointVertexIndices.push_back(1u);
    runtimes[0].gpuMesh.nodeRanges[0].pointIndexCount = 2u;
    const uint64_t changedRuntime = woby::hoverPickSignature(
        files,
        runtimes,
        {50.0f, 50.0f},
        true,
        woby::defaultMasterVertexPointSize,
        camera,
        woby::SceneUpAxis::z,
        bounds,
        100u,
        100u,
        true);
    CHECK(changedRuntime != baseline);
}
