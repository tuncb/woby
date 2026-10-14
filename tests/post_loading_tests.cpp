#include "post_loading.h"
#include "obj_mesh.h"
#include "surface_annotation.h"
#include "scene_inspector_queries.h"
#include "control_scene.h"
#include "background_load.h"
#include "ui_operations.h"
#include "parallel_work.h"
#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <fstream>
#include <future>
#include <limits>

namespace {
constexpr std::string_view seamModel =
    "v 0 0 0\nv 2 0 0\nv 0 3 0\nv 99 99 99\n"
    "vt 0 0\nvt 1 0\nvt 0 1\nvt .5 .5\n"
    "g surface\nf 3/3 1/1 2/2\nf 3/4 2/2 1/1\n";

struct PostLoadFixture {
    std::filesystem::path root;
    woby::UiState state;
    std::vector<woby::LoadedModelRuntime> models;
    woby::PostLoadRuntime runtime;
    bool graphics = false;
    PostLoadFixture()
    {
        static std::atomic<unsigned> sequence = 0;
        root = std::filesystem::temp_directory_path() / ("woby-post-load-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
        std::filesystem::create_directory(root);
    }
    ~PostLoadFixture()
    {
        woby::cancelPostLoading(runtime);
        if (graphics) { woby::destroyModelRuntimes(models); woby::graphics::shutdown(); }
        std::error_code error; std::filesystem::remove_all(root, error);
    }
    void add(woby::Mesh mesh)
    {
        state.files.push_back(woby::createUiFileState(root / "model.obj", std::move(mesh), 0));
        woby::appendDefaultSceneNodesForFiles(state, state.files.size() - 1);
        woby::recalculateSceneBounds(state);
    }
    void initializeGraphics()
    {
        woby::graphics::Init init; init.type = woby::graphics::RendererType::Noop;
        graphics = woby::graphics::init(init);
        REQUIRE(graphics);
        for (const auto& file : state.files) {
            woby::LoadedModelRuntime model;
            model.gpuMesh = woby::createGpuMesh(file.mesh, woby::meshVertexLayout());
            models.push_back(std::move(model));
        }
    }
    void frame()
    {
        woby::graphics::frame();
        woby::notePostLoadFrame(runtime, state, woby::graphics::submittedFrameToken());
    }
    void update()
    {
        woby::updatePostLoading(runtime, state, models, woby::meshVertexLayout(), woby::graphics::completedFrameToken());
    }
};

struct UploadBlocker {
    std::promise<void> release;
    std::shared_ptr<woby::graphics::BufferUpload> ticket;
    bool blocked = true;
    UploadBlocker()
    {
        std::promise<void> entered;
        const auto ready = release.get_future().share();
        ticket = woby::graphics::beginBufferUpload([&entered, ready](const std::atomic<bool>&) {
            entered.set_value(); ready.wait(); return std::vector<woby::graphics::BufferUploadSource>{};
        });
        entered.get_future().wait();
    }
    void unblock()
    {
        if (!blocked) { return; }
        release.set_value();
        blocked = false;
        woby::graphics::waitBufferUpload(ticket);
    }
    ~UploadBlocker() { unblock(); }
};

woby::Mesh deferred(std::string_view text)
{
    woby::ObjPrototypeOptions options; options.deferPreparation = true; options.workers = 1;
    return woby::loadObjMeshTextPrototype(text, {}, options);
}
}

TEST_CASE("Deferred OBJ preparation preserves precise source identity UV seams and group bounds")
{
    PostLoadFixture fixture;
    const auto eager = woby::loadObjMeshText(seamModel);
    fixture.add(deferred(seamModel));
    auto& file = fixture.state.files.front();
    REQUIRE(file.mesh.sourceData);
    CHECK(file.mesh.sourceData->indicesPending);
    CHECK(file.mesh.sourceData->indices.empty());
    CHECK_FALSE(file.mesh.originalBounds);
    CHECK_FALSE(file.groupSettings.front().localBoundsValid);
    const auto owner = file.mesh.storage;
    auto data = woby::preparePostLoadData(file.mesh);
    REQUIRE(data);
    REQUIRE(data->source);
    CHECK_FALSE(data->source->indicesPending);
    CHECK(data->source->indices == eager.sourceData->indices);
    CHECK(data->source->positions == file.mesh.sourceData->positions);
    CHECK(data->originalBounds == *eager.originalBounds);
    const auto groups = woby::createUiGroupStates(eager, 0);
    REQUIRE(data->groups.size() == groups.size());
    CHECK(data->groups[0].local.min == groups[0].localBounds.min);
    CHECK(data->groups[0].local.max == groups[0].localBounds.max);
    CHECK(data->groups[0].local.radius == groups[0].localBounds.radius);
    CHECK(data->groups[0].original == *groups[0].originalBounds);
    CHECK(data->annotations->geometry == owner);
    file.groupSettings[0].showVertices = true;
    const auto document = woby::createSceneDocument(fixture.state);
    const auto revision = fixture.state.sceneEditRevision;
    REQUIRE(woby::publishPostLoadData(fixture.state, woby::postLoadIdentity(fixture.state, file), std::move(*data)));
    CHECK(woby::scenePostLoadReady(fixture.state));
    CHECK(file.mesh.storage == owner);
    CHECK(woby::createSceneDocument(fixture.state) == document);
    CHECK(fixture.state.sceneEditRevision == revision);
    CHECK_FALSE(fixture.state.isDirty);
}

TEST_CASE("Post loading waits for frame completion and publishes GPU points without replacing surfaces")
{
    PostLoadFixture fixture;
    fixture.add(deferred(seamModel));
    fixture.state.files[0].groupSettings[0].showVertices = true;
    fixture.initializeGraphics();
    // Keep the pending-upload assertions independent of worker scheduling.
    UploadBlocker uploads;
    const auto vertex = fixture.models[0].gpuMesh.vertexBuffer;
    const auto triangles = fixture.models[0].gpuMesh.triangleIndexBuffer;
    CHECK_FALSE(woby::graphics::isValid(fixture.models[0].gpuMesh.pointIdBuffer));
    CHECK_FALSE(woby::buildSceneDrawPlan(fixture.state).items[0].points);
    woby::updatePostLoadNotice(fixture.runtime, fixture.state, 10);
    CHECK(woby::postLoadNotice(fixture.runtime, fixture.state, 100) == "Preparing model...");
    fixture.update();
    CHECK_FALSE(fixture.runtime.job);
    fixture.frame();
    woby::updatePostLoading(fixture.runtime, fixture.state, fixture.models, woby::meshVertexLayout(), 0);
    CHECK_FALSE(fixture.runtime.job);
    fixture.update();
    REQUIRE(fixture.runtime.job);
    fixture.runtime.job->worker.join();
    REQUIRE(fixture.runtime.job->done.load());
    REQUIRE(fixture.runtime.job->result);
    REQUIRE(fixture.runtime.job->result->source);
    CHECK_FALSE(fixture.runtime.job->result->source->indicesPending);
    REQUIRE(fixture.runtime.job->result->annotations);
    CHECK_FALSE(fixture.state.files[0].mesh.annotationCache);
    fixture.update();
    REQUIRE(fixture.runtime.upload);
    CHECK(fixture.state.files[0].postLoadPhase == woby::PostLoadPhase::uploading);
    CHECK_FALSE(woby::graphics::isValid(fixture.models[0].gpuMesh.pointIdBuffer));
    fixture.update();
    CHECK_FALSE(woby::scenePostLoadReady(fixture.state));
    CHECK_FALSE(fixture.state.files[0].mesh.annotationCache);
    CHECK(fixture.state.files[0].mesh.sourceData->indicesPending);
    REQUIRE(fixture.runtime.upload);
    const auto frameToken = woby::graphics::submittedFrameToken();
    uploads.unblock();
    woby::waitGpuMeshUpload(*fixture.runtime.upload);
    fixture.update();
    CHECK(woby::graphics::submittedFrameToken() == frameToken);
    CHECK(woby::scenePostLoadReady(fixture.state));
    CHECK(woby::annotationMeshSnapshotReady(fixture.state.files[0].mesh));
    CHECK_FALSE(fixture.state.files[0].mesh.sourceData->indicesPending);
    CHECK(fixture.models[0].gpuMesh.vertexBuffer.idx == vertex.idx);
    CHECK(fixture.models[0].gpuMesh.triangleIndexBuffer.idx == triangles.idx);
    CHECK(woby::graphics::isValid(fixture.models[0].gpuMesh.pointIdBuffer));
    CHECK(woby::buildSceneDrawPlan(fixture.state).items[0].points);
    woby::updatePostLoadNotice(fixture.runtime, fixture.state, 101);
    CHECK(woby::postLoadNotice(fixture.runtime, fixture.state, 102.99) == "Ready");
    CHECK(woby::postLoadNotice(fixture.runtime, fixture.state, 103).empty());
}

TEST_CASE("Post load source and bounds failures join both tasks and release the worker budget")
{
    auto mesh = deferred(seamModel);
    std::string error;
    SUBCASE("source mapping") {
        auto source = std::make_shared<woby::SourceMeshData>(*mesh.sourceData);
        source->positions = std::make_shared<const std::vector<woby::Coordinate>>();
        mesh.sourceData = std::move(source);
        error = "Invalid source triangle position.";
    }
    SUBCASE("original bounds") {
        mesh.origin[0] = std::numeric_limits<double>::infinity();
        error = "Non-finite original coordinate.";
    }
    CHECK_THROWS_WITH((void)woby::preparePostLoadData(mesh), error.c_str());
    CHECK(woby::analysisWorkerCount.load() == 0);
    CHECK_FALSE(mesh.annotationCache);
    CHECK(mesh.sourceData->indicesPending);
    CHECK(mesh.sourceData->indices.empty());
}

TEST_CASE("Post load cancellation during early preparation does not publish partial results")
{
    const auto mesh = deferred(seamModel);
    // Polls are serialized by the coordinator, including calls from nested workers.
    for (const unsigned cancelAt : {3u, 6u, 9u}) {
        unsigned polls = 0;
        CHECK_FALSE(woby::preparePostLoadData(mesh, [&] { return ++polls >= cancelAt; }));
        CHECK(polls == cancelAt);
        CHECK(woby::analysisWorkerCount.load() == 0);
        CHECK_FALSE(mesh.annotationCache);
        CHECK(mesh.sourceData->indicesPending);
        CHECK(mesh.sourceData->indices.empty());
    }
    const auto data = woby::preparePostLoadData(mesh);
    REQUIRE(data);
    REQUIRE(data->annotations);
    REQUIRE(data->source);
    CHECK_FALSE(data->source->indicesPending);
}

TEST_CASE("Point-only models render a placeholder then upload independently of later frames")
{
    PostLoadFixture fixture;
    woby::Mesh mesh; mesh.preparationDeferred = true;
    auto& geometry = woby::editMeshGeometry(mesh);
    for (uint32_t i = 0; i < 65536; ++i) {
        geometry.vertices.push_back({{static_cast<float>(i % 256), static_cast<float>(i / 256), 0}, {0,0,1}, {}});
        geometry.pointIndices.push_back(i);
    }
    woby::MeshNode node; node.name = "points"; node.pointIndexCount = 65536;
    geometry.nodes.push_back(node);
    woby::captureSourceMesh(mesh, woby::SourceProvenance::importerVertices);
    woby::finalizeMesh(mesh, false);
    fixture.add(std::move(mesh));
    fixture.initializeGraphics();
    CHECK(woby::buildSceneDrawPlan(fixture.state).items.empty());
    CHECK_FALSE(woby::graphics::isValid(fixture.models[0].gpuMesh.vertexBuffer));
    fixture.frame(); fixture.update();
    REQUIRE(fixture.runtime.job);
    fixture.runtime.job->worker.join();
    fixture.update();
    REQUIRE(fixture.runtime.upload);
    CHECK_FALSE(woby::scenePostLoadReady(fixture.state));
    const auto frameToken = woby::graphics::submittedFrameToken();
    woby::waitGpuMeshUpload(*fixture.runtime.upload);
    fixture.update();
    CHECK(woby::graphics::submittedFrameToken() == frameToken);
    REQUIRE(woby::scenePostLoadReady(fixture.state));
    REQUIRE(fixture.models[0].gpuMesh.pointCloud);
    CHECK(fixture.models[0].gpuMesh.pointCloud->sourceVertices.size() == 65536);
    CHECK_FALSE(fixture.models[0].gpuMesh.pointChunks.empty());
    CHECK_FALSE(fixture.models[0].gpuMesh.proxyChunks.empty());
    CHECK(fixture.models[0].gpuMesh.compactOnly);
    CHECK(woby::buildSceneDrawPlan(fixture.state).items[0].points);
}

TEST_CASE("Pending bounds hide saved pivot transforms and keep preparation controls unavailable")
{
    PostLoadFixture fixture;
    fixture.add(deferred(seamModel));
    auto& file = fixture.state.files[0];
    file.groupSettings[0].rotationDegrees = {0,0,90};
    file.groupSettings[0].showVertices = true;
    woby::selectSceneObject(fixture.state, file.groupSettings[0].objectId, false);
    CHECK(woby::buildSceneDrawPlan(fixture.state).items.empty());
    CHECK_FALSE(woby::selectedObjectProperty(fixture.state, woby::UiObjectProperty::vertices).available);
    CHECK_FALSE(woby::selectedObjectProperty(fixture.state, woby::UiObjectProperty::scale).available);
    woby::SceneInspectorRuntime inspector;
    woby::updateSceneInspector(inspector, fixture.state);
    CHECK_FALSE(inspector.snapshot.bounds);
    CHECK_FALSE(inspector.snapshot.dimensions);
    auto data = woby::preparePostLoadData(file.mesh);
    REQUIRE(data);
    REQUIRE(woby::publishPostLoadData(fixture.state, woby::postLoadIdentity(fixture.state, file), std::move(*data)));
    REQUIRE(woby::buildSceneDrawPlan(fixture.state).items.size() == 1);
    CHECK(file.groupSettings[0].rotationDegrees == std::array<float,3>{0,0,90});
    woby::updateSceneInspector(inspector, fixture.state);
    CHECK(inspector.snapshot.bounds.has_value());
}

TEST_CASE("Small point-only models defer their vertex and ID buffers without a hierarchy")
{
    PostLoadFixture fixture;
    fixture.add(deferred("v 0 0 0\nv 1 0 0\nv 0 1 0\np 1 2 3\n"));
    fixture.initializeGraphics();
    CHECK_FALSE(woby::graphics::isValid(fixture.models[0].gpuMesh.vertexBuffer));
    fixture.frame(); fixture.update();
    REQUIRE(fixture.runtime.job);
    fixture.runtime.job->worker.join();
    fixture.update();
    REQUIRE(fixture.runtime.upload);
    woby::waitGpuMeshUpload(*fixture.runtime.upload);
    fixture.update();
    REQUIRE(woby::scenePostLoadReady(fixture.state));
    CHECK(woby::graphics::isValid(fixture.models[0].gpuMesh.vertexBuffer));
    CHECK(woby::graphics::isValid(fixture.models[0].gpuMesh.pointIdBuffer));
    CHECK_FALSE(fixture.models[0].gpuMesh.pointCloud);
    CHECK(fixture.models[0].gpuMesh.pointVertexIndices == std::vector<uint32_t>{0,1,2});
}

TEST_CASE("Post loading rejects stale documents removed models and canceled results")
{
    PostLoadFixture fixture;
    fixture.add(deferred(seamModel));
    const auto identity = woby::postLoadIdentity(fixture.state, fixture.state.files[0]);
    auto copy = fixture.state;
    CHECK_FALSE(woby::postLoadCurrent(identity, copy));
    CHECK_FALSE(woby::preparePostLoadData(fixture.state.files[0].mesh, [] { return true; }));
    auto data = woby::preparePostLoadData(fixture.state.files[0].mesh);
    REQUIRE(data);
    ++fixture.state.sceneGeneration;
    CHECK_FALSE(woby::publishPostLoadData(fixture.state, identity, std::move(*data)));
    fixture.initializeGraphics(); fixture.frame(); fixture.update();
    REQUIRE(fixture.runtime.job);
    fixture.state.files.clear();
    fixture.update();
    CHECK_FALSE(fixture.runtime.job);
    woby::updatePostLoadNotice(fixture.runtime, fixture.state, 12);
    CHECK(woby::postLoadNotice(fixture.runtime, fixture.state, 12).empty());
}

TEST_CASE("Failed post uploads keep the surface available and never report Ready")
{
    PostLoadFixture fixture;
    fixture.add(deferred(seamModel)); fixture.initializeGraphics();
    woby::updatePostLoadNotice(fixture.runtime, fixture.state, 0);
    fixture.frame(); fixture.update();
    REQUIRE(fixture.runtime.job);
    fixture.runtime.job->worker.join();
    fixture.update();
    REQUIRE(fixture.runtime.upload);
    woby::graphics::cancelBufferUpload(fixture.runtime.upload->ticket);
    fixture.runtime.upload->ticket = woby::graphics::beginBufferUpload([](const std::atomic<bool>&) -> std::vector<woby::graphics::BufferUploadSource> {
        throw std::runtime_error("fixture upload failed");
    });
    woby::graphics::waitBufferUpload(fixture.runtime.upload->ticket);
    fixture.update();
    CHECK(fixture.state.files[0].postLoadPhase == woby::PostLoadPhase::failed);
    CHECK(woby::graphics::isValid(fixture.models[0].gpuMesh.vertexBuffer));
    CHECK_FALSE(woby::scenePostLoadReady(fixture.state));
    woby::updatePostLoadNotice(fixture.runtime, fixture.state, 1);
    CHECK(woby::postLoadNotice(fixture.runtime, fixture.state, 1) != "Ready");
    CHECK_FALSE(woby::postLoadNotice(fixture.runtime, fixture.state, 1).empty());
}

TEST_CASE("Changing geometry during preparation requires a fresh completed frame")
{
    PostLoadFixture fixture;
    fixture.add(deferred(seamModel)); fixture.initializeGraphics();
    fixture.frame(); fixture.update();
    REQUIRE(fixture.runtime.job);
    fixture.runtime.job->worker.join();
    fixture.state.files[0].mesh = deferred(seamModel);
    fixture.update();
    CHECK_FALSE(fixture.runtime.job);
    CHECK(fixture.state.files[0].postLoadPhase == woby::PostLoadPhase::waitingForFrame);
    fixture.update();
    CHECK_FALSE(fixture.runtime.job);
    fixture.frame(); fixture.update();
    REQUIRE(fixture.runtime.job);
    CHECK(fixture.runtime.job->identity.revision == fixture.state.files[0].mesh.contentRevision);
}

TEST_CASE("Saved annotations and section workers remain hidden until post loading is ready")
{
    PostLoadFixture fixture;
    fixture.add(deferred(seamModel));
    const auto group = fixture.state.files[0].groupSettings[0].objectId;
    const auto id = woby::createCrossSection(fixture.state, {}, {group});
    woby::SceneQueryRuntime queries;
    woby::updateSceneAnnotationQueries(queries, fixture.state);
    CHECK_FALSE(queries.sections);
    REQUIRE(queries.annotations.objects.contains(id));
    CHECK(queries.annotations.objects.at(id).lines.empty());
    CHECK(queries.annotations.objects.at(id).sheet.empty());
    auto data = woby::preparePostLoadData(fixture.state.files[0].mesh);
    REQUIRE(data);
    REQUIRE(woby::publishPostLoadData(fixture.state, woby::postLoadIdentity(fixture.state, fixture.state.files[0]), std::move(*data)));
    woby::updateSceneAnnotationQueries(queries, fixture.state);
    CHECK(queries.sections != nullptr);
}

TEST_CASE("Deferred mixed and freeform OBJ source topology matches complete loading")
{
    PostLoadFixture fixture;
    const std::array<std::string_view, 2> inputs{
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 2 2 0\nv 90 90 90\n"
        "vn 0 1 0\ng mixed\nf 1//1 2//1 3//1\nl 1 4\np 4\n",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
        "f 1 2 3\ncstype bezier\ndeg 1 1\nsurf 0 1 0 1 1 2 3 4\nparm u 0 1\nparm v 0 1\nend\n"};
    for (const auto input : inputs) {
        const auto eager = woby::loadObjMeshText(input);
        const auto pending = deferred(input);
        auto data = woby::preparePostLoadData(pending);
        REQUIRE(data);
        REQUIRE(data->source);
        CHECK(data->source->indices == eager.sourceData->indices);
        CHECK(woby::sourcePositions(*data->source) == woby::sourcePositions(*eager.sourceData));
        CHECK(data->originalBounds == *eager.originalBounds);
        const auto groups = woby::createUiGroupStates(eager, 0);
        REQUIRE(groups.size() == data->groups.size());
        for (size_t i = 0; i < groups.size(); ++i) {
            CHECK(data->groups[i].local.min == groups[i].localBounds.min);
            CHECK(data->groups[i].local.max == groups[i].localBounds.max);
            CHECK(data->groups[i].original == *groups[i].originalBounds);
        }
    }
}

TEST_CASE("Viewer batch and saved scene loading defer derived data without changing saved modes")
{
    PostLoadFixture fixture;
    const auto model = fixture.root / "triangle.obj", scene = fixture.root / "scene.woby";
    { std::ofstream stream(model); stream << "o triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
    const auto batch = woby::loadModelBatchCpu({model}, 0, {}, {}, true, true);
    REQUIRE(batch.files.size() == 1);
    REQUIRE(batch.preparations.size() == 1);
    CHECK(batch.files[0].postLoadPhase == woby::PostLoadPhase::waitingForFrame);
    CHECK(batch.files[0].mesh.sourceData->indices.empty());
    CHECK(batch.preparations[0].pointVertexIndices.empty());
    CHECK_FALSE(batch.preparations[0].pointCloud);
    woby::SceneDocument document;
    woby::SceneFileRecord record; record.path = model;
    woby::SceneGroupRecord group; group.name = "triangle";
    group.settings.showVertices = true; group.settings.rotationDegrees = {0,0,30};
    record.groups.push_back(group); document.files.push_back(record);
    woby::writeSceneDocument(scene, document);
    const auto loaded = woby::loadSceneCpu(scene, {}, {}, true, true);
    REQUIRE(loaded.files.size() == 1);
    CHECK(loaded.files[0].groupSettings[0].showVertices);
    CHECK(loaded.files[0].groupSettings[0].rotationDegrees == group.settings.rotationDegrees);
    CHECK(loaded.preparations[0].features == 0);
    CHECK(loaded.preparations[0].pointVertexIndices.empty());
}
