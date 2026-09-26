#include "surface_annotation.h"
#include "allocation_probe.h"
#include "annotation_ui.h"
#include "scene_scale_overlay.h"
#include "ui_operations.h"
#include "scene_history.h"
#include "automation_registry.h"
#include "control_scene.h"
#include "obj_mesh.h"
#include "camera.h"
#include <nlohmann/json.hpp>

#include <bx/math.h>
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {
using namespace woby;
Mesh surface(bool folded = false)
{
    Mesh mesh;
    for (float x : {-1.0f, 0.0f, 1.0f}) {
        for (float y : {-1.0f, 1.0f}) {
            Vertex vertex;
            vertex.position = {x, y, folded ? .2f + .6f * std::abs(x) : .5f};
            mesh.vertices.push_back(vertex);
        }
    }
    mesh.indices = {0, 2, 3, 0, 3, 1, 2, 4, 5, 2, 5, 3};
    mesh.nodes = {{"surface", 0, 12}};
    mesh.bounds = calculateBounds(mesh.vertices);
    return mesh;
}
Mesh tessellatedSurface(uint32_t n, uint32_t layers = 1)
{
    Mesh mesh;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        const auto offset = static_cast<uint32_t>(mesh.vertices.size());
        for (uint32_t y = 0; y <= n; ++y) {
            for (uint32_t x = 0; x <= n; ++x) {
                Vertex vertex;
                const float u = static_cast<float>(x) / static_cast<float>(n) * 2 - 1;
                const float v = static_cast<float>(y) / static_cast<float>(n) * 2 - 1;
                vertex.position = {u, v, .2f + .3f * u * u + static_cast<float>(layer) * .01f};
                mesh.vertices.push_back(vertex);
            }
        }
        for (uint32_t y = 0; y < n; ++y) {
            for (uint32_t x = 0; x < n; ++x) {
                const uint32_t a = offset + y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
                mesh.indices.insert(mesh.indices.end(), {a,b,d,a,d,c});
            }
        }
    }
    mesh.nodes = {{"surface", 0, static_cast<uint32_t>(mesh.indices.size())}};
    mesh.bounds = calculateBounds(mesh.vertices);
    return mesh;
}
ScenePickView view()
{
    ScenePickView result;
    bx::mtxIdentity(result.view.data()); bx::mtxIdentity(result.projection.data());
    result.width = result.height = 200;
    return result;
}
Mesh holedSurface(bool multiple = false)
{
    Mesh mesh;
    const std::vector<float> xs = multiple ? std::vector<float>{-1, -.6f, -.2f, .2f, .6f, 1}
        : std::vector<float>{-1, -.3f, .3f, 1};
    const std::array<float, 4> ys{-1, -.6f, .6f, 1};
    for (float y : ys) {
        for (float x : xs) {
            Vertex vertex; vertex.position = {x, y, .5f + .2f * x};
            mesh.vertices.push_back(vertex);
        }
    }
    const auto width = static_cast<uint32_t>(xs.size());
    for (uint32_t y = 0; y < 3; ++y) {
        for (uint32_t x = 0; x + 1 < width; ++x) {
            if (y == 1 && (x == 1 || (multiple && x == 3))) { continue; }
            const uint32_t a = y * width + x, b = a + 1, c = a + width, d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a,b,d,a,d,c});
        }
    }
    mesh.nodes = {{"surface", 0, static_cast<uint32_t>(mesh.indices.size())}};
    mesh.bounds = calculateBounds(mesh.vertices);
    return mesh;
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("woby-annotation-" + automationRandomHex(8));
    UiState state;
    Fixture(bool folded = false)
    {
        state.files.push_back(createUiFileState(root / "model.obj", surface(folded), 0));
        appendDefaultSceneNodesForFiles(state, 0); clearSceneDirty(state);
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    SceneObjectId target() const { return state.files[0].groupSettings[0].objectId; }
    AnnotationProjection projection() const { return annotationProjection(scenePickParts(state), view(), target()); }
    AnnotationGeometry geometry(AnnotationShape shape = AnnotationShape::line) const
    { return projectAnnotation(projection(), shape, {-.8f, -.4f}, {.8f, .4f}); }
    SceneObjectId add(AnnotationShape shape = AnnotationShape::line) { return createAnnotation(state, target(), geometry(shape)); }
};
TEST_CASE("annotations retain projection precision with a distant camera and tiny near plane")
{
    Fixture fixture;
    SceneCamera camera;
    // Camera values from singular_projector.woby.
    camera.target = {920.996521f, 2694.68408f, 22564.8379f};
    camera.yawRadians = -.400796115f;
    camera.pitchRadians = 1.57079637f;
    camera.distance = 44366.6055f;
    camera.nearPlane = .00131279614f;
    bool homogeneous = false;
    SUBCASE("zero to one depth") {}
    SUBCASE("homogeneous depth") { homogeneous = true; }
    SUBCASE("further zoomed out") { camera.distance *= 2; }
    SUBCASE("multiple source parts") {
        auto& mesh = fixture.state.files[0].mesh;
        mesh.nodes = {{"left", 0, 6}, {"right", 6, 6}};
        fixture.state.files[0] = createUiFileState(fixture.root / "model.obj", mesh, 0);
        fixture.state.sceneNodes.clear();
        appendDefaultSceneNodesForFiles(fixture.state, 0);
    }
    auto& mesh = fixture.state.files[0].mesh;
    for (auto& vertex : mesh.vertices) {
        const auto p = vertex.position;
        vertex.position = {camera.target[0] + p[0] * 60000, camera.target[1],
            camera.target[2] + p[1] * 60000};
    }
    mesh.bounds = calculateBounds(mesh.vertices);
    auto distant = view();
    distant.homogeneousDepth = homogeneous;
    bx::mtxLookAt(distant.view.data(), cameraEye(camera, SceneUpAxis::y), cameraLookAt(camera), cameraUp(camera, SceneUpAxis::y));
    bx::mtxProj(distant.projection.data(), 60, 1, camera.nearPlane, cameraFarPlane(camera, mesh.bounds), homogeneous);
    const auto parts = scenePickParts(fixture.state);
    const std::array<float, 2> start{-.2f, -.2f}, end{.2f, .2f};
    auto projection = annotationGestureProjection(parts, distant, fixture.target(), start);
    setAnnotationProjectionTargets(projection, parts, distant, fixture.target(), annotationGroupTargets(fixture.state, fixture.target()));
    expandAnnotationGestureProjection(projection, parts, distant, start, end);
    CHECK_NOTHROW((void)previewAnnotation(projection, AnnotationShape::rectangle, start, end));
    const auto geometry = projectAnnotation(projection, AnnotationShape::rectangle, start, end);
    const auto id = createAnnotation(fixture.state, projection.targetId, geometry, projection.targetIds);
    REQUIRE(findAnnotation(fixture.state, id));
    std::filesystem::create_directories(fixture.root);
    const auto path = fixture.root / "distant.woby";
    const auto document = createSceneDocument(fixture.state);
    writeSceneDocument(path, document);
    const auto read = readSceneDocument(path);
    REQUIRE(read.annotations == document.annotations);
    const auto restored = prepareSceneReplacement(fixture.state, fixture.state.files, read);
    REQUIRE(restored.annotations.size() == 1);
    CHECK_NOTHROW((void)projectAnnotation(annotationEditProjection(scenePickParts(restored), restored.annotations[0]),
        AnnotationShape::rectangle, {-.1f, -.1f}, {.1f, .1f}));
}
TEST_CASE("distant annotation projection distinguishes overlapping surface depths")
{
    Fixture fixture;
    auto front = surface();
    for (auto& vertex : front.vertices) {
        vertex.position = {vertex.position[0] * 30000, vertex.position[1] * 30000, 0};
    }
    front.bounds = calculateBounds(front.vertices);
    auto back = front;
    for (auto& vertex : back.vertices) { vertex.position[2] = 100; }
    back.bounds = calculateBounds(back.vertices);
    fixture.state = {};
    fixture.state.files.push_back(createUiFileState(fixture.root / "back.obj", back, 0));
    fixture.state.files.push_back(createUiFileState(fixture.root / "front.obj", front, 1));
    appendDefaultSceneNodesForFiles(fixture.state, 0);
    auto distant = view();
    bx::mtxLookAt(distant.view.data(), bx::Vec3(0, 0, -44366.6055f), bx::Vec3(0, 0, 0));
    bx::mtxProj(distant.projection.data(), 60, 1, .00131279614f, 200000, false);
    const auto parts = scenePickParts(fixture.state);
    const auto frontId = fixture.state.files[1].groupSettings[0].objectId;
    const auto projection = annotationProjection(parts, distant, frontId);
    CHECK(pickAnnotationSurface(projection, {0, 0}) == frontId);
    CHECK_NOTHROW((void)projectAnnotation(projection, AnnotationShape::rectangle, {-.2f, -.2f}, {.2f, .2f}));
    CHECK_THROWS((void)projectAnnotation(annotationProjection(parts, distant, fixture.target()),
        AnnotationShape::rectangle, {-.2f, -.2f}, {.2f, .2f}));
}
TEST_CASE("drawing projection grows from the pointer to the full rectangle")
{
    Fixture fixture;
    fixture.state.files[0].mesh = tessellatedSurface(36);
    const auto parts = scenePickParts(fixture.state);
    const auto full = annotationProjection(parts, view(), fixture.target());
    const std::array<float, 2> start{-.62f, -.43f}, end{.57f, .48f};
    auto gesture = annotationGestureProjection(parts, view(), fixture.target(), start);
    CHECK(gesture.triangles.size() < full.triangles.size());
    CHECK(pickAnnotationSurface(gesture, start) == fixture.target());
    for (int step = 1; step <= 10; ++step) {
        const float t = static_cast<float>(step) / 10;
        const std::array<float, 2> current{start[0] + t * (end[0] - start[0]),
            start[1] + t * (end[1] - start[1])};
        expandAnnotationGestureProjection(gesture, parts, view(), start, current);
        CHECK(projectAnnotation(gesture, AnnotationShape::rectangle, start, current)
            == projectAnnotation(full, AnnotationShape::rectangle, start, current));
    }
    const auto expected = projectAnnotation(full, AnnotationShape::rectangle, start, end);
    const auto actual = projectAnnotation(gesture, AnnotationShape::rectangle, start, end);
    CHECK(actual == expected);
    const auto count = gesture.triangles.size();
    expandAnnotationGestureProjection(gesture, parts, view(), start, end);
    CHECK(gesture.triangles.size() == count);
}
TEST_CASE("large mesh annotation cache preserves the source fingerprint")
{
    auto mesh = tessellatedSurface(160);
    const auto expected = annotationFingerprint(mesh, mesh.nodes[0].indexOffset, mesh.nodes[0].indexCount);
    prepareAnnotationMeshCache(mesh);
    REQUIRE(mesh.annotationCache);
    REQUIRE(mesh.annotationCache->fingerprints.size() == 1);
    CHECK(mesh.annotationCache->fingerprints[0] == expected);
    CHECK(mesh.annotationCache->blocks.size() > 1);
    auto copy = mesh;
    copy.vertices[0].position[2] += .1f;
    ScenePickPart part;
    part.objectId = 1; part.mesh = &copy; part.indexCount = copy.indices.size(); part.solid = true;
    bx::mtxIdentity(part.model.data());
    const auto gesture = annotationGestureProjection(std::array{part}, view(), 1, {0, 0});
    CHECK(gesture.definition.fingerprint == annotationFingerprint(copy, 0, copy.indices.size()));
}
TEST_CASE("cached surface picking agrees with a full triangle scan")
{
    auto mesh = tessellatedSurface(160);
    prepareAnnotationMeshCache(mesh);
    REQUIRE(mesh.annotationCache);
    const auto cache = mesh.annotationCache;
    ScenePickPart part;
    part.objectId = 1; part.mesh = &mesh; part.indexCount = mesh.indices.size();
    part.solid = true; part.bounds = mesh.bounds;
    bx::mtxIdentity(part.model.data());
    for (const PickPoint point : {PickPoint{0, 0}, {20, 150}, {100, 100}, {199, 199}, {200, 100}}) {
        const auto cached = pickSceneObject(std::array{part}, view(), point);
        mesh.annotationCache.reset();
        const auto scanned = pickSceneObject(std::array{part}, view(), point);
        CHECK(cached == scanned);
        mesh.annotationCache = cache;
    }
}
// Independently tessellated siblings with no identical seam vertices. The right
// part has its own local origin; it touches the left only after its transform.
void siblingSurface(Fixture& fixture, float gap = 0, float depthJump = 0)
{
    Mesh mesh;
    for (const auto p : std::vector<std::array<float, 3>>{
        {-1,-1,.5f}, {-gap,-1,.5f}, {-gap,1,.5f}, {-1,1,.5f},
        {2+gap,-.9f,.8f+depthJump}, {3,-.9f,.8f+depthJump}, {3,.9f,.8f+depthJump}, {2+gap,.9f,.8f+depthJump}}) {
        Vertex vertex; vertex.position = p; mesh.vertices.push_back(vertex);
    }
    mesh.indices = {0,1,2,0,2,3,4,5,6,4,6,7};
    mesh.nodes = {{"left",0,6}, {"right",6,6}};
    mesh.bounds = calculateBounds(mesh.vertices);
    fixture.state = {};
    fixture.state.files.push_back(createUiFileState(fixture.root / "siblings.obj", mesh, 0));
    appendDefaultSceneNodesForFiles(fixture.state, 0);
    fixture.state.files[0].groupSettings[1].translation = {-2,0,-.3f};
    clearSceneDirty(fixture.state);
}
SceneObjectId drawSiblingAnnotation(Fixture& fixture, AnnotationShape shape = AnnotationShape::line)
{
    AnnotationInteraction interaction; interaction.tool = shape;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    REQUIRE(interaction.dragging);
    moveAnnotationPointer(fixture.state, interaction, {180,60});
    REQUIRE(interaction.error.empty());
    endAnnotationPointer(fixture.state, interaction, true);
    REQUIRE(interaction.error.empty());
    REQUIRE(fixture.state.annotations.size() == 1);
    return fixture.state.annotations.front().objectId;
}
void nearPoint(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    for (size_t k = 0; k < 3; ++k) { CHECK(a[k] == doctest::Approx(b[k]).epsilon(1e-5)); }
}
struct AnnotationUiFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    Fixture scene;
    AnnotationInteraction interaction;
    AnnotationNameEdit nameEdit;
    ScenePickView pickView = view();
    ImVec2 lineButton{}, rectangleButton{}, dimensions{};
    bool pointerAllowed = true, captureMouse = false, disabled = false;
    bool showObjects = false, showInspector = false;
    bool showScaleOverlay = false;
    float messageBottom = 0;
    float windowY = 0;
    ImVec2 removeButton{}, visibilityButton{};
    std::string inspectorContents, objectContents;
    AnnotationUiFixture()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = {800,600}; io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        frame(); frame();
    }
    ~AnnotationUiFixture() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
    void frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({280,showObjects ? 560.0f : 120.0f});
        ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
        const auto origin = ImGui::GetCursorScreenPos();
        drawAnnotationTools(scene.state, interaction, disabled);
        const auto low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
        rectangleButton = {(low.x + high.x) / 2, (low.y + high.y) / 2};
        lineButton = {origin.x + (high.x - low.x) / 2, rectangleButton.y};
        ImGui::SameLine();
        bool showDimensions = false;
        ImGui::Checkbox("Show dimensions", &showDimensions);
        dimensions = ImGui::GetItemRectMin();
        if (showObjects) {
            ImGui::LogToBuffer();
            drawAnnotationObjects(scene.state, nameEdit);
            objectContents = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            const auto removeLow = ImGui::GetItemRectMin(), removeHigh = ImGui::GetItemRectMax();
            removeButton = {(removeLow.x + removeHigh.x) / 2, (removeLow.y + removeHigh.y) / 2};
            visibilityButton = {lineButton.x, removeButton.y};
        }
        ImGui::End();
        if (showInspector) {
            ImGui::SetNextWindowPos({300,0}); ImGui::SetNextWindowSize({480,600});
            ImGui::Begin("Properties");
            ImGui::LogToBuffer();
            drawAnnotationInspector(scene.state);
            inspectorContents = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
        }
        ImGui::GetIO().WantCaptureMouse = captureMouse;
        messageBottom = drawAnnotationOverlay(scene.state, interaction, pickView, 300, 1 / pickView.pixelScale, pointerAllowed, windowY);
        if (showScaleOverlay) {
            drawSceneScaleOverlay(*ImGui::GetBackgroundDrawList(), scene.state, std::nullopt,
                pickView, {300, 0}, 1 / pickView.pixelScale, ImGui::GetFontSize());
        }
        ImGui::EndFrame();
    }
    void click(ImVec2 p, ImGuiMouseButton button = ImGuiMouseButton_Left)
    {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(p.x,p.y); frame();
        io.AddMouseButtonEvent(button,true); frame(); io.AddMouseButtonEvent(button,false); frame();
    }
    void key(ImGuiKey value)
    {
        auto& io = ImGui::GetIO(); io.AddKeyEvent(value, true); frame();
        io.AddKeyEvent(value, false); frame(); frame();
    }
    void type(const char* value) { ImGui::GetIO().AddInputCharactersUTF8(value); frame(); }
    ImGuiMouseCursor cursor(ImVec2 p)
    {
        ImGui::GetIO().AddMousePosEvent(p.x,p.y); frame(); frame();
        return ImGui::GetMouseCursor();
    }
};
using Json = nlohmann::json;
const ObjectIdFormatter annotationId = [](SceneObjectId id) { return std::to_string(id); };
Json annotationCommand(Fixture& fixture, const std::string& method, Json params = Json::object(), SceneObjectId id = 0)
{
    if (id) { params["target"] = annotationId(id); }
    auto command = parseControlOperation(*findControlMethod(method), params);
    command.objectId = id;
    if (command.object) { command.memberId = std::stoull(*command.object); }
    return applyControlSceneOperation(fixture.state, {}, command, annotationId, 200, 800);
}
void annotationCamera(Fixture& fixture)
{
    setSceneUpAxis(fixture.state, SceneUpAxis::y);
    recalculateSceneBounds(fixture.state);
    annotationCommand(fixture, "camera.look-at", {{"eye", {0,0,3}}, {"target", {0,0,.5}}});
}
}

TEST_CASE("annotation gestures draw on half-opacity surfaces with any selection scope")
{
    for (const auto shape : {AnnotationShape::line, AnnotationShape::rectangle}) {
        Fixture fixture;
        setFileOpacity(fixture.state.files[0].fileSettings, .5f);
        SUBCASE("no selection") { fixture.state.selectedSceneObjects.clear(); }
        SUBCASE("single part selected") { selectSceneObject(fixture.state, fixture.target()); }
        SUBCASE("multiple parts selected") {
            fixture.state.files.push_back(createUiFileState(fixture.root / "other.obj", surface(), 1));
            appendDefaultSceneNodesForFiles(fixture.state, 1);
            setFileTranslation(fixture.state.files[1].fileSettings, {3,0,0});
            fixture.state.selectedSceneObjects = {fixture.target(), fixture.state.files[1].groupSettings[0].objectId};
        }
        AnnotationInteraction interaction;
        interaction.tool = shape;
        REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
        REQUIRE(interaction.dragging);
        moveAnnotationPointer(fixture.state, interaction, {180,60});
        REQUIRE(interaction.error.empty());
        endAnnotationPointer(fixture.state, interaction, true);
        REQUIRE(fixture.state.annotations.size() == 1);
        CHECK(fixture.state.annotations.front().targetId == fixture.target());
        const auto& item = fixture.state.annotations.front();
        CHECK(item.geometry.shape == shape);
        const auto vertices = annotationVertices(fixture.state, item);
        REQUIRE(vertices.size() == (shape == AnnotationShape::line ? 2u : 4u));
        nearPoint(vertices.front(), {-.8f,-.4f,.5f});
        nearPoint(vertices[shape == AnnotationShape::line ? 1 : 2], {.8f,.4f,.5f});
    }
}

TEST_CASE("annotation target picking includes transparency but projection ignores transparent neighbors")
{
    Fixture fixture;
    auto front = surface();
    for (auto& vertex : front.vertices) { vertex.position[2] = .2f; }
    front.bounds = calculateBounds(front.vertices);
    fixture.state.files.push_back(createUiFileState(fixture.root / "front.obj", std::move(front), 1));
    appendDefaultSceneNodesForFiles(fixture.state, 1);
    setFileOpacity(fixture.state.files[1].fileSettings, .5f);
    const auto parts = scenePickParts(fixture.state);
    CHECK(pickAnnotationSurface(annotationProjection(parts, view(), 0), {0,0})
        == fixture.state.files[1].groupSettings[0].objectId);
    CHECK(pickAnnotationSurface(fixture.projection(), {0,0}) == fixture.target());
    CHECK_NOTHROW(fixture.geometry());
    setFileOpacity(fixture.state.files[1].fileSettings, 0);
    CHECK(pickAnnotationSurface(annotationProjection(scenePickParts(fixture.state), view(), 0), {0,0}) == fixture.target());
    setFileOpacity(fixture.state.files[1].fileSettings, 1);
    CHECK_THROWS(fixture.geometry());
}

TEST_CASE("annotation CLI creates inspects styles moves reshapes and deletes surface annotations")
{
    Fixture fixture(true); annotationCamera(fixture);
    std::string shape = "line";
    SUBCASE("line") {}
    SUBCASE("rectangle") { shape = "rectangle"; }
    selectSceneObject(fixture.state, fixture.target());
    const auto created = annotationCommand(fixture, "annotation.create",
        {{"shape", shape}, {"start", {-.15,-.15}}, {"end", {.15,.15}}, {"name", "CLI range"}, {"aspect", 1.5}}, fixture.target());
    const auto id = std::stoull(created["target"].get<std::string>());
    REQUIRE(fixture.state.annotations.size() == 1);
    CHECK(fixture.state.selectedSceneObjects == std::vector<SceneObjectId>{fixture.target()});
    CHECK(created["object"]["vertices"].size() == (shape == "line" ? 2u : 4u));
    CHECK(created["object"]["sourceId"] == annotationId(fixture.target()));
    CHECK(created["object"]["targetValid"] == true);
    CHECK(created["object"]["effectiveVisible"] == true);
    const auto list = annotationCommand(fixture, "annotation.list");
    CHECK(list["annotations"].size() == 1);
    const auto read = annotationCommand(fixture, "annotation.get", Json::object(), id);
    CHECK(read["object"] == created["object"]);
    CHECK(controlSceneInfo(fixture.state)["annotationCount"] == 1);
    CHECK(controlSceneTree(fixture.state, annotationId).back()["kind"] == "annotation");
    CHECK(controlObjectDetails(fixture.state, id, annotationId)["vertices"] == created["object"]["vertices"]);
    const std::string comments = "Review \"this\"\nUTF-8 \xc3\xa9";
    auto updated = annotationCommand(fixture, "annotation.set", {{"comments", comments}, {"rgb", {.2,.4,.6}}, {"opacity", .7}, {"width", 99}}, id);
    CHECK(updated["object"]["settings"]["comments"] == comments);
    CHECK(updated["object"]["settings"]["width"] == 12);
    const auto visibility = annotationCommand(fixture, "visibility.set", {{"visible", false}}, id);
    CHECK(visibility["applied"]["visible"] == false);
    CHECK(visibility["bounds"] == controlSceneInfo(fixture.state)["bounds"]);
    CHECK_FALSE(findAnnotation(fixture.state, id)->settings.visible);
    annotationCommand(fixture, "visibility.set", {{"visible", true}}, id);
    annotationCommand(fixture, "color.set", {{"rgb", {.1,.2,.3}}}, id);
    annotationCommand(fixture, "opacity.set", {{"value", .5}}, id);
    annotationCommand(fixture, "color.reset", Json::object(), id);
    CHECK(findAnnotation(fixture.state, id)->settings.color[3] == .5f);
    annotationCommand(fixture, "camera.frame", {{"object", annotationId(id)}});
    const auto original = findAnnotation(fixture.state, id)->geometry;
    annotationCommand(fixture, "annotation.move", {{"delta", {.05,.05}}}, id);
    CHECK(findAnnotation(fixture.state, id)->geometry.start[0] == doctest::Approx(-.1));
    CHECK(findAnnotation(fixture.state, id)->geometry.end[0] == doctest::Approx(.2));
    CHECK(findAnnotation(fixture.state, id)->geometry.projector == original.projector);
    annotationCommand(fixture, "annotation.reshape", {{"start", {-.05,-.05}}}, id);
    CHECK(findAnnotation(fixture.state, id)->geometry.start[0] == doctest::Approx(-.05));
    CHECK(findAnnotation(fixture.state, id)->geometry.end[0] == doctest::Approx(.2));
    annotationCommand(fixture, "annotation.set", {{"comments", ""}}, id);
    CHECK(findAnnotation(fixture.state, id)->settings.note.empty());
    annotationCommand(fixture, "annotation.delete", Json::object(), id);
    CHECK(fixture.state.annotations.empty());
    CHECK_THROWS(annotationCommand(fixture, "annotation.get", Json::object(), id));
}
TEST_CASE("annotation CLI rejects invalid geometry atomically and respects locks and missing sources")
{
    Fixture fixture; annotationCamera(fixture);
    const auto id = fixture.add();
    const auto original = createSceneDocument(fixture.state);
    CHECK_THROWS(annotationCommand(fixture, "annotation.create", {{"shape", "rectangle"}, {"start", {0,0}}, {"end", {0,0}}}, fixture.target()));
    CHECK_THROWS(annotationCommand(fixture, "annotation.create", {{"shape", "line"}, {"start", {-.1,-.1}}, {"end", {.1,.1}}}, fixture.state.files[0].objectId));
    CHECK_THROWS(annotationCommand(fixture, "annotation.move", {{"delta", {1,1}}}, id));
    CHECK_THROWS(annotationCommand(fixture, "annotation.reshape", {{"end", {-.8,-.4}}}, id));
    CHECK(sceneContentEqual(createSceneDocument(fixture.state), original));
    annotationCommand(fixture, "annotation.set", {{"locked", true}}, id);
    CHECK_THROWS(annotationCommand(fixture, "annotation.move", {{"delta", {.1,.1}}}, id));
    annotationCommand(fixture, "annotation.set", {{"comments", "Locked comment remains editable"}}, id);
    annotationCommand(fixture, "annotation.set", {{"locked", false}}, id);
    setFileVisible(fixture.state.files[0], false);
    const auto hidden = annotationCommand(fixture, "annotation.get", Json::object(), id)["object"];
    CHECK(hidden["effectiveVisible"] == false); CHECK(hidden["vertices"].size() == 2);
    CHECK_THROWS(annotationCommand(fixture, "annotation.move", {{"delta", {.1,.1}}}, id));
    REQUIRE(removeFileFromState(fixture.state, 0));
    const auto missing = annotationCommand(fixture, "annotation.get", Json::object(), id)["object"];
    CHECK(missing["sourceId"].is_null()); CHECK(missing["targetValid"] == false); CHECK(missing["vertices"].empty());
    annotationCommand(fixture, "annotation.set", {{"name", "Missing source"}}, id);
    annotationCommand(fixture, "annotation.delete", Json::object(), id);
    CHECK(fixture.state.annotations.empty());
}
TEST_CASE("annotation CLI commands preserve geometry comments through scene save and undo redo")
{
    Fixture fixture(true); annotationCamera(fixture);
    const auto id = fixture.add(AnnotationShape::rectangle);
    const auto clean = createSceneDocument(fixture.state);
    SceneHistory history; resetSceneHistory(history, fixture.state);
    annotationCommand(fixture, "annotation.move", {{"delta", {.1,.1}}}, id);
    REQUIRE(recordSceneHistory(history, fixture.state));
    const auto moved = findAnnotation(fixture.state, id)->geometry;
    auto restored = prepareSceneHistoryStep(history, fixture.state, clean, false);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.state, std::move(*restored), false);
    CHECK(findAnnotation(fixture.state, id)->geometry == clean.annotations[0].geometry);
    restored = prepareSceneHistoryStep(history, fixture.state, clean, true);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.state, std::move(*restored), true);
    CHECK(findAnnotation(fixture.state, id)->geometry == moved);
    annotationCommand(fixture, "annotation.set", {{"comments", "Multiline\nCLI comment"}}, id);
    std::filesystem::create_directories(fixture.root);
    const auto path = fixture.root / "cli.woby";
    const auto document = createSceneDocument(fixture.state); writeSceneDocument(path, document);
    CHECK(readSceneDocument(path).annotations == document.annotations);
}

TEST_CASE("annotation icon tools share the dimensions row and toggle without editing scene content")
{
    AnnotationUiFixture fixture;
    CHECK(fixture.lineButton.x < fixture.rectangleButton.x);
    CHECK(fixture.rectangleButton.x < fixture.dimensions.x);
    CHECK(fixture.dimensions.y < fixture.rectangleButton.y);
    const auto revision = fixture.scene.state.sceneEditRevision;
    fixture.click(fixture.lineButton);
    CHECK(fixture.interaction.tool == AnnotationShape::line);
    fixture.click(fixture.rectangleButton);
    CHECK(fixture.interaction.tool == AnnotationShape::rectangle);
    fixture.click(fixture.rectangleButton);
    CHECK_FALSE(fixture.interaction.tool);
    fixture.disabled = true;
    fixture.click(fixture.lineButton);
    CHECK_FALSE(fixture.interaction.tool);
    CHECK(fixture.scene.state.sceneEditRevision == revision);
}
TEST_CASE("annotation row eye toggles visibility and X deletes long named items with undo")
{
    AnnotationUiFixture fixture;
    const auto id = fixture.scene.add();
    auto settings = findAnnotation(fixture.scene.state, id)->settings;
    settings.name = std::string(400, 'x'); setAnnotationSettings(fixture.scene.state, id, settings);
    fixture.showObjects = true; fixture.frame(); fixture.frame();
    fixture.click(fixture.visibilityButton);
    CHECK_FALSE(findAnnotation(fixture.scene.state, id)->settings.visible);
    fixture.click(fixture.visibilityButton);
    CHECK(findAnnotation(fixture.scene.state, id)->settings.visible);
    const auto clean = createSceneDocument(fixture.scene.state);
    SceneHistory history; resetSceneHistory(history, fixture.scene.state);
    fixture.click(fixture.removeButton);
    CHECK_FALSE(findAnnotation(fixture.scene.state, id));
    CHECK(fixture.scene.state.selectedSceneObjects.empty());
    REQUIRE(recordSceneHistory(history, fixture.scene.state));
    auto restored = prepareSceneHistoryStep(history, fixture.scene.state, clean, false);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.scene.state, std::move(*restored), false);
    REQUIRE(findAnnotation(fixture.scene.state, id));
    CHECK(findAnnotation(fixture.scene.state, id)->settings.name == settings.name);
}
TEST_CASE("annotation rename and duplicate preserve independent saved objects")
{
    Fixture fixture;
    const auto original = fixture.add();
    auto settings = findAnnotation(fixture.state, original)->settings;
    settings.note = "Review this edge";
    setAnnotationSettings(fixture.state, original, settings);
    const auto revision = fixture.state.sceneEditRevision;
    renameAnnotation(fixture.state, original, "Inspection edge");
    CHECK(findAnnotation(fixture.state, original)->settings.name == "Inspection edge");
    CHECK(fixture.state.sceneEditRevision == revision + 1);
    const auto copy = duplicateAnnotation(fixture.state, original);
    REQUIRE(copy != 0);
    REQUIRE(copy != original);
    const auto* copied = findAnnotation(fixture.state, copy);
    REQUIRE(copied);
    CHECK(copied->settings.name == "Inspection edge copy");
    CHECK(copied->settings.note == settings.note);
    CHECK(copied->geometry == findAnnotation(fixture.state, original)->geometry);
    CHECK(copied->targetId == findAnnotation(fixture.state, original)->targetId);
    CHECK(fixture.state.selectedSceneObjects == std::vector<SceneObjectId>{copy});
    renameAnnotation(fixture.state, copy, "");
    CHECK(findAnnotation(fixture.state, copy)->settings.name == "Annotation");
    CHECK(findAnnotation(fixture.state, original)->settings.name == "Inspection edge");
    const auto document = createSceneDocument(fixture.state);
    REQUIRE(document.annotations.size() == 2);
    CHECK(document.annotations[0].settings.name == "Inspection edge");
    CHECK(document.annotations[1].settings.name == "Annotation");
    const auto unchanged = fixture.state.sceneEditRevision;
    renameAnnotation(fixture.state, 0, "Missing");
    CHECK(duplicateAnnotation(fixture.state, 0) == 0);
    CHECK(fixture.state.sceneEditRevision == unchanged);
}
TEST_CASE("annotation Properties retains visibility and comments without a delete button")
{
    AnnotationUiFixture fixture;
    fixture.scene.add(); fixture.showInspector = true; fixture.frame();
    CHECK(fixture.inspectorContents.find("Visible") != std::string::npos);
    CHECK(fixture.inspectorContents.find("Comments") != std::string::npos);
    CHECK(fixture.inspectorContents.find("Delete annotation") == std::string::npos);
}
TEST_CASE("annotation F2 renames inline and Escape cancels edits")
{
    AnnotationUiFixture fixture;
    const auto id = fixture.scene.add();
    fixture.showObjects = true; fixture.frame(); fixture.frame();
    fixture.click({100, fixture.removeButton.y});
    const auto original = findAnnotation(fixture.scene.state, id)->settings.name;
    const auto revision = fixture.scene.state.sceneEditRevision;
    fixture.key(ImGuiKey_F2);
    REQUIRE(fixture.nameEdit.objectId == id);
    fixture.type("Inspection line");
    CHECK(findAnnotation(fixture.scene.state, id)->settings.name == original);
    fixture.key(ImGuiKey_Escape);
    CHECK(fixture.nameEdit.objectId == invalidSceneObjectId);
    CHECK(findAnnotation(fixture.scene.state, id)->settings.name == original);
    CHECK(fixture.scene.state.sceneEditRevision == revision);
    fixture.key(ImGuiKey_F2);
    REQUIRE(fixture.nameEdit.objectId == id);
    fixture.type("Inspection line");
    fixture.key(ImGuiKey_Enter);
    CHECK(fixture.nameEdit.objectId == invalidSceneObjectId);
    CHECK(findAnnotation(fixture.scene.state, id)->settings.name == "Inspection line");
    CHECK(fixture.scene.state.sceneEditRevision == revision + 1);
}
TEST_CASE("annotation context menu shares Rename Duplicate and Delete actions")
{
    AnnotationUiFixture fixture;
    const auto id = fixture.scene.add(); fixture.showObjects = true; fixture.frame(); fixture.frame();
    fixture.click({100,fixture.removeButton.y}, ImGuiMouseButton_Right);
    fixture.frame();
    CHECK(fixture.objectContents.find("Rename") != std::string::npos);
    CHECK(fixture.objectContents.find("Duplicate") != std::string::npos);
    CHECK(fixture.objectContents.find("Delete annotation") != std::string::npos);
    CHECK(fixture.objectContents.find("Properties") == std::string::npos);
    CHECK(fixture.objectContents.find("Frame annotation") == std::string::npos);
    REQUIRE_FALSE(fixture.context->OpenPopupStack.empty());
    const auto* popup = fixture.context->OpenPopupStack.back().Window;
    REQUIRE(popup);
    const auto start = popup->DC.CursorStartPos;
    SUBCASE("Rename opens inline editor") {
        fixture.click({start.x + 30, start.y + ImGui::GetTextLineHeight() * 0.5f});
        CHECK(fixture.nameEdit.objectId == id);
    }
    SUBCASE("Duplicate creates another annotation") {
        fixture.click({start.x + 30, start.y + ImGui::GetTextLineHeightWithSpacing()
            + ImGui::GetTextLineHeight() * 0.5f});
        REQUIRE(fixture.scene.state.annotations.size() == 2);
        CHECK(fixture.scene.state.annotations.back().settings.name == "Surface line 1 copy");
    }
}
TEST_CASE("annotation messages wrap at the top without overlapping the grid readout")
{
    bool errorMessage = true;
    SUBCASE("placement error") {}
    SUBCASE("drawing hint") { errorMessage = false; }
    for (float scale : {1.0f, 2.0f}) {
        for (float density : {1.0f, 2.0f}) {
            for (uint32_t width : {260u, 900u}) {
                AnnotationUiFixture fixture;
                ImGui::GetStyle().FontScaleMain = scale;
                ImGui::GetIO().DisplaySize = {1400, 600};
                fixture.pickView.pixelScale = density;
                fixture.pickView.width = static_cast<uint32_t>(static_cast<float>(width) * density);
                fixture.pickView.height = static_cast<uint32_t>(600 * density);
                fixture.showScaleOverlay = true;
                fixture.scene.state.showGrid = true;
                if (errorMessage) {
                    fixture.interaction.error = "Start on a visible surface of the selected model.";
                }
                else { fixture.interaction.tool = AnnotationShape::line; }
                fixture.frame(); fixture.frame();
                REQUIRE(fixture.messageBottom > 0);
                CHECK(fixture.messageBottom < 300);
                ImVec2 minimum{10000, 10000}, maximum{-10000, -10000};
                for (const auto& vertex : ImGui::GetForegroundDrawList()->VtxBuffer) {
                    if (vertex.col != IM_COL32(255,230,170,255)) { continue; }
                    minimum.x = std::min(minimum.x, vertex.pos.x); minimum.y = std::min(minimum.y, vertex.pos.y);
                    maximum.x = std::max(maximum.x, vertex.pos.x); maximum.y = std::max(maximum.y, vertex.pos.y);
                }
                CHECK(minimum.x < maximum.x);
                CHECK(minimum.x >= 300);
                CHECK(maximum.x <= 300 + static_cast<float>(width));
                CHECK(minimum.y >= 0);
                CHECK(maximum.y <= fixture.messageBottom);
                const auto* grid = ImGui::GetBackgroundDrawList();
                REQUIRE_FALSE(grid->VtxBuffer.empty());
                for (const auto& vertex : grid->VtxBuffer) { CHECK(vertex.pos.y > fixture.messageBottom); }
                cancelAnnotationPointer(fixture.interaction);
                fixture.frame();
                CHECK(fixture.messageBottom == 0);
                CHECK(ImGui::GetForegroundDrawList()->VtxBuffer.empty());
            }
        }
    }
}

TEST_CASE("annotation drawing crosshair stays in available canvas and clears on cancellation")
{
    AnnotationUiFixture fixture;
    fixture.interaction.tool = AnnotationShape::line;
    CHECK(fixture.cursor({400,100}) == ImGuiMouseCursor_None);
    CHECK(fixture.cursor({600,100}) == ImGuiMouseCursor_Arrow);
    fixture.pointerAllowed = false;
    CHECK(fixture.cursor({400,100}) == ImGuiMouseCursor_Arrow);
    fixture.pointerAllowed = true; fixture.captureMouse = true;
    CHECK(fixture.cursor({400,100}) == ImGuiMouseCursor_Arrow);
    fixture.captureMouse = false;
    REQUIRE(beginAnnotationPointer(fixture.scene.state, fixture.interaction, fixture.pickView, {20,140}));
    CHECK(fixture.cursor({400,100}) == ImGuiMouseCursor_None);
    cancelAnnotationPointer(fixture.interaction);
    CHECK(fixture.cursor({400,100}) == ImGuiMouseCursor_Arrow);
}
TEST_CASE("annotation move cursor follows visible editable handles and persists during a drag")
{
    AnnotationUiFixture fixture;
    const auto id = fixture.scene.add();
    CHECK(fixture.cursor({320,140}) == ImGuiMouseCursor_ResizeAll);
    CHECK(fixture.cursor({331,140}) == ImGuiMouseCursor_Arrow);
    fixture.pickView.width = fixture.pickView.height = 400; fixture.pickView.pixelScale = 2;
    CHECK(fixture.cursor({328,140}) == ImGuiMouseCursor_ResizeAll);
    CHECK(fixture.cursor({331,140}) == ImGuiMouseCursor_Arrow);
    REQUIRE(beginAnnotationPointer(fixture.scene.state, fixture.interaction, fixture.pickView, {40,280}));
    CHECK(fixture.cursor({400,100}) == ImGuiMouseCursor_ResizeAll);
    cancelAnnotationPointer(fixture.interaction);
    auto settings = findAnnotation(fixture.scene.state, id)->settings; settings.locked = true;
    setAnnotationSettings(fixture.scene.state, id, settings);
    CHECK(fixture.cursor({320,140}) == ImGuiMouseCursor_Arrow);
    settings.locked = false; setAnnotationSettings(fixture.scene.state, id, settings);
    setFileVisible(fixture.scene.state.files[0], false); markSceneDirty(fixture.scene.state);
    CHECK(fixture.cursor({320,140}) == ImGuiMouseCursor_Arrow);
}
TEST_CASE("annotation move cursor does not expose handles behind another model")
{
    AnnotationUiFixture fixture;
    fixture.scene.add();
    auto front = surface();
    for (auto& vertex : front.vertices) { vertex.position[2] = .1f; }
    fixture.scene.state.files.push_back(createUiFileState(fixture.scene.root / "front.obj", front, 1));
    appendDefaultSceneNodesForFiles(fixture.scene.state, 1); markSceneDirty(fixture.scene.state);
    CHECK(fixture.cursor({320,140}) == ImGuiMouseCursor_Arrow);
}

TEST_CASE("surface annotation picking locates face interiors without vertex snapping")
{
    Fixture fixture;
    const auto hit = annotationSurfaceHit(fixture.projection(), {-.7f, .3f});
    REQUIRE(hit); CHECK(hit->objectId == fixture.target());
    nearPoint(annotationPosition(fixture.state.files[0].mesh, 0, hit->triangle, hit->bary), {-.7f, .3f, .5f});
    CHECK_FALSE(annotationSurfaceHit(fixture.projection(), {2, 0}));
}
TEST_CASE("selected annotation edge drag translates a curved outline once with undo redo")
{
    Fixture fixture(true);
    auto shape = AnnotationShape::line;
    SUBCASE("line") {}
    SUBCASE("rectangle") { shape = AnnotationShape::rectangle; }
    const auto id = fixture.add(shape);
    const auto original = findAnnotation(fixture.state, id)->geometry;
    const auto clean = createSceneDocument(fixture.state);
    clearSceneDirty(fixture.state);
    SceneHistory history; resetSceneHistory(history, fixture.state);
    AnnotationInteraction interaction;
    const PickPoint grab{100, shape == AnnotationShape::line ? 100.0f : 140.0f};
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), grab));
    REQUIRE(interaction.editing == id); CHECK(interaction.handle == -1);
    moveAnnotationPointer(fixture.state, interaction, {grab[0] + 5, grab[1] - 5});
    REQUIRE(interaction.error.empty());
    moveAnnotationPointer(fixture.state, interaction, {grab[0] + 10, grab[1] - 10});
    REQUIRE(interaction.error.empty());
    CHECK_FALSE(fixture.state.isDirty);
    CHECK(findAnnotation(fixture.state, id)->geometry == original);
    CHECK_FALSE(recordSceneHistory(history, fixture.state));
    endAnnotationPointer(fixture.state, interaction, true);
    const auto moved = findAnnotation(fixture.state, id)->geometry;
    for (size_t axis = 0; axis < 2; ++axis) {
        CHECK(moved.start[axis] == doctest::Approx(original.start[axis] + .1f));
        CHECK(moved.end[axis] == doctest::Approx(original.end[axis] + .1f));
    }
    for (const auto& segment : moved.segments) {
        for (const auto& bary : {segment.a, segment.b}) {
            const auto p = annotationPosition(fixture.state.files[0].mesh, 0, segment.triangle, bary);
            CHECK(p[2] == doctest::Approx(.2f + .6f * std::abs(p[0])));
        }
    }
    REQUIRE(recordSceneHistory(history, fixture.state)); CHECK(history.snapshots.size() == 2);
    auto restored = prepareSceneHistoryStep(history, fixture.state, clean, false);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.state, std::move(*restored), false);
    CHECK(findAnnotation(fixture.state, id)->geometry == original);
    restored = prepareSceneHistoryStep(history, fixture.state, clean, true);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.state, std::move(*restored), true);
    CHECK(findAnnotation(fixture.state, id)->geometry == moved);
}
TEST_CASE("whole annotation movement requires a selected editable visible edge")
{
    Fixture fixture;
    const auto id = fixture.add(AnnotationShape::rectangle);
    PickPoint point{100,140};
    SUBCASE("unselected") { fixture.state.selectedSceneObjects.clear(); }
    SUBCASE("source selected") { selectSceneObject(fixture.state, fixture.target()); }
    SUBCASE("multiple selected") { selectSceneObject(fixture.state, fixture.target(), true); }
    SUBCASE("interior") { point = {100,100}; }
    SUBCASE("locked") {
        auto settings = findAnnotation(fixture.state, id)->settings; settings.locked = true;
        setAnnotationSettings(fixture.state, id, settings);
    }
    SUBCASE("hidden") { setFileVisible(fixture.state.files[0], false); }
    SUBCASE("unresolved") { fixture.state.annotations[0].targetValid = false; }
    SUBCASE("occluded") {
        auto mesh = surface(); for (auto& vertex : mesh.vertices) { vertex.position[2] = .1f; }
        fixture.state.files.push_back(createUiFileState(fixture.root / "front.obj", mesh, 1));
        appendDefaultSceneNodesForFiles(fixture.state, 1);
    }
    AnnotationInteraction interaction;
    CHECK_FALSE(beginAnnotationPointer(fixture.state, interaction, view(), point));
    CHECK_FALSE(interaction.dragging);
}
TEST_CASE("annotation edge drag cancellation and invalid moves preserve original geometry")
{
    Fixture fixture;
    const auto id = fixture.add();
    const auto original = findAnnotation(fixture.state, id)->geometry;
    AnnotationInteraction interaction;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {100,100}));
    moveAnnotationPointer(fixture.state, interaction, {110,90});
    REQUIRE(interaction.error.empty());
    SUBCASE("escape") { cancelAnnotationPointer(interaction); }
    SUBCASE("outline outside surface despite pointer on source") {
        moveAnnotationPointer(fixture.state, interaction, {150,100});
        CHECK_FALSE(interaction.error.empty());
        endAnnotationPointer(fixture.state, interaction, true);
    }
    SUBCASE("selection changed during drag") {
        selectSceneObject(fixture.state, fixture.target());
        moveAnnotationPointer(fixture.state, interaction, {110,90});
        CHECK_FALSE(interaction.error.empty());
        endAnnotationPointer(fixture.state, interaction, true);
    }
    SUBCASE("selection changed before release") {
        selectSceneObject(fixture.state, fixture.target());
        endAnnotationPointer(fixture.state, interaction, true);
    }
    CHECK(findAnnotation(fixture.state, id)->geometry == original);
}
TEST_CASE("annotation edge movement uses the original projector after changing the view")
{
    Fixture fixture;
    const auto id = fixture.add();
    auto rotated = view(); bx::mtxRotateZ(rotated.view.data(), bx::kPiHalf);
    const auto screen = [&](float x, float y) {
        const auto p = annotationTransform(rotated.view, {x,y,.5f,1});
        return PickPoint{(p[0]+1)*100, (1-p[1])*100};
    };
    AnnotationInteraction interaction;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, rotated, screen(0,0)));
    REQUIRE(interaction.handle == -1);
    moveAnnotationPointer(fixture.state, interaction, screen(.1f,.1f));
    REQUIRE(interaction.error.empty());
    endAnnotationPointer(fixture.state, interaction, true);
    const auto& geometry = findAnnotation(fixture.state, id)->geometry;
    CHECK(geometry.start[0] == doctest::Approx(-.7f));
    CHECK(geometry.start[1] == doctest::Approx(-.3f));
    CHECK(geometry.end[0] == doctest::Approx(.9f));
    CHECK(geometry.end[1] == doctest::Approx(.5f));
}
TEST_CASE("annotation vertex dragging takes priority over moving the whole outline")
{
    Fixture fixture;
    const auto id = fixture.add(AnnotationShape::rectangle);
    const auto original = findAnnotation(fixture.state, id)->geometry;
    AnnotationInteraction interaction;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    CHECK(interaction.handle == 0);
    moveAnnotationPointer(fixture.state, interaction, {30,130});
    REQUIRE(interaction.error.empty());
    endAnnotationPointer(fixture.state, interaction, true);
    CHECK(findAnnotation(fixture.state, id)->geometry.end == original.end);
    CHECK(findAnnotation(fixture.state, id)->geometry.start != original.start);
}
TEST_CASE("selected annotation edges show move cursor while interior and unselected edges do not")
{
    AnnotationUiFixture fixture;
    fixture.scene.add(AnnotationShape::rectangle);
    CHECK(fixture.cursor({400,140}) == ImGuiMouseCursor_ResizeAll);
    CHECK(fixture.cursor({400,100}) == ImGuiMouseCursor_Arrow);
    CHECK(fixture.cursor({400,140}) == ImGuiMouseCursor_ResizeAll);
    fixture.scene.state.selectedSceneObjects.clear();
    CHECK(fixture.cursor({400,140}) == ImGuiMouseCursor_Arrow);
}
TEST_CASE("annotation Properties vertices match surface endpoints and corners in model coordinates")
{
    Fixture fixture(true);
    auto shape = AnnotationShape::line;
    SUBCASE("line endpoints") {}
    SUBCASE("rectangle corners") { shape = AnnotationShape::rectangle; }
    const auto id = fixture.add(shape);
    const auto* item = findAnnotation(fixture.state, id);
    const auto vertices = annotationVertices(fixture.state, *item);
    REQUIRE(vertices.size() == (shape == AnnotationShape::line ? 2u : 4u));
    nearPoint(vertices[0], {-.8f, -.4f, .68f});
    nearPoint(vertices[shape == AnnotationShape::line ? 1 : 2], {.8f, .4f, .68f});
    if (shape == AnnotationShape::rectangle) {
        nearPoint(vertices[1], {.8f, -.4f, .68f});
        nearPoint(vertices[3], {-.8f, .4f, .68f});
    }
    const auto reshaped = projectAnnotation(fixture.projection(), shape, {-.5f,-.3f}, {.5f,.3f});
    setFileTranslation(fixture.state.files[0].fileSettings, {3,4,5});
    setGroupTranslation(fixture.state.files[0].groupSettings[0], {1,2,3});
    setFileVisible(fixture.state.files[0], false);
    clearSceneDirty(fixture.state);
    const auto revision = fixture.state.sceneEditRevision;
    CHECK(annotationVertices(fixture.state, *item) == vertices);
    CHECK_FALSE(fixture.state.isDirty);
    CHECK(fixture.state.sceneEditRevision == revision);
    reshapeAnnotation(fixture.state, id, reshaped);
    const auto updated = annotationVertices(fixture.state, *item);
    REQUIRE(updated.size() == vertices.size());
    nearPoint(updated[0], {-.5f, -.3f, .5f});
    nearPoint(updated[shape == AnnotationShape::line ? 1 : 2], {.5f, .3f, .5f});
}
TEST_CASE("annotation Properties never reports vertices for unresolved targets")
{
    Fixture fixture;
    const auto id = fixture.add();
    SUBCASE("missing source") { fixture.state.files.clear(); }
    SUBCASE("changed source") {
        fixture.state.files[0].mesh.vertices[0].position[2] += .1f;
        validateAnnotationTargets(fixture.state);
    }
    CHECK(annotationVertices(fixture.state, *findAnnotation(fixture.state, id)).empty());
}
TEST_CASE("multiline annotation comments preserve long UTF-8 text and coalesce undo redo")
{
    Fixture fixture;
    const auto id = fixture.add();
    const auto clean = createSceneDocument(fixture.state);
    SceneHistory history; resetSceneHistory(history, fixture.state);
    auto settings = findAnnotation(fixture.state, id)->settings;
    settings.note = "Inspect this range\nSecond line";
    setAnnotationSettings(fixture.state, id, settings);
    REQUIRE(recordSceneHistory(history, fixture.state, 123));
    settings.note += "\nQuoted \"comment\" \\ path\nUTF-8: \xc3\xa9 \xe2\x9c\x93\n";
    settings.note.append(6000, 'x');
    setAnnotationSettings(fixture.state, id, settings);
    REQUIRE(recordSceneHistory(history, fixture.state, 123));
    finishSceneHistoryInteraction(history);
    CHECK(history.snapshots.size() == 2);
    auto restored = prepareSceneHistoryStep(history, fixture.state, clean, false);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.state, std::move(*restored), false);
    CHECK(findAnnotation(fixture.state, id)->settings.note.empty());
    restored = prepareSceneHistoryStep(history, fixture.state, clean, true);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.state, std::move(*restored), true);
    CHECK(findAnnotation(fixture.state, id)->settings.note == settings.note);
    std::filesystem::create_directories(fixture.root);
    const auto path = fixture.root / "comments.woby";
    writeSceneDocument(path, createSceneDocument(fixture.state));
    CHECK(readSceneDocument(path).annotations[0].settings.note == settings.note);
    clearSceneDirty(fixture.state);
    setAnnotationSettings(fixture.state, id, settings);
    CHECK_FALSE(fixture.state.isDirty);
}
TEST_CASE("surface line follows a fold with connected segments on individual triangles")
{
    Fixture fixture(true);
    const auto geometry = fixture.geometry();
    REQUIRE(geometry.segments.size() >= 2);
    const auto& mesh = fixture.state.files[0].mesh;
    bool crossesRidge = false;
    std::optional<std::array<float, 3>> previous;
    for (const auto& segment : geometry.segments) {
        const auto a = annotationPosition(mesh, 0, segment.triangle, segment.a);
        const auto b = annotationPosition(mesh, 0, segment.triangle, segment.b);
        if (previous) { nearPoint(*previous, a); }
        for (const auto& p : {a, b}) {
            CHECK(p[2] == doctest::Approx(.2f + .6f * std::abs(p[0])));
            if (std::abs(p[0]) < 1e-5f) { crossesRidge = true; CHECK(p[2] == doctest::Approx(.2f)); }
        }
        previous = b;
    }
    CHECK(crossesRidge);
    CHECK(geometry == fixture.geometry());
}
TEST_CASE("surface rectangle keeps four projected sides on curved geometry")
{
    Fixture fixture(true);
    const auto geometry = fixture.geometry(AnnotationShape::rectangle);
    REQUIRE(geometry.segments.size() >= 6);
    const auto& mesh = fixture.state.files[0].mesh;
    for (size_t i = 0; i < geometry.segments.size(); ++i) {
        const auto& segment = geometry.segments[i];
        const auto& next = geometry.segments[(i + 1) % geometry.segments.size()];
        const auto a = annotationPosition(mesh, 0, segment.triangle, segment.a);
        const auto b = annotationPosition(mesh, 0, segment.triangle, segment.b);
        nearPoint(b, annotationPosition(mesh, 0, next.triangle, next.a));
        const float x = (a[0] + b[0]) * .5f, y = (a[1] + b[1]) * .5f;
        CHECK((std::abs(std::abs(x) - .8f) < 1e-5f || std::abs(std::abs(y) - .4f) < 1e-5f));
    }
}
TEST_CASE("surface annotation rejects unsupported controls and depth jumps")
{
    Fixture fixture;
    SUBCASE("endpoint outside model") { fixture.state.files[0].mesh.indices.erase(fixture.state.files[0].mesh.indices.begin() + 6, fixture.state.files[0].mesh.indices.end()); }
    SUBCASE("disconnected rear layer") {
        auto& mesh = fixture.state.files[0].mesh;
        mesh.vertices.push_back(mesh.vertices[2]); mesh.vertices.push_back(mesh.vertices[3]);
        mesh.vertices[6].position[2] = .8f; mesh.vertices[7].position[2] = .8f;
        mesh.vertices[4].position[2] = .8f; mesh.vertices[5].position[2] = .8f;
        mesh.indices = {0, 2, 3, 0, 3, 1, 6, 4, 5, 6, 5, 7};
    }
    CHECK_THROWS(fixture.geometry());
    CHECK_THROWS(fixture.geometry(AnnotationShape::rectangle));
}
TEST_CASE("duplicate vertices at exact mesh seams do not break surface annotations")
{
    Fixture fixture(true);
    auto& mesh = fixture.state.files[0].mesh;
    mesh.vertices.push_back(mesh.vertices[2]); mesh.vertices.push_back(mesh.vertices[3]);
    mesh.indices = {0, 2, 3, 0, 3, 1, 6, 4, 5, 6, 5, 7};
    CHECK_NOTHROW(fixture.geometry());
}
TEST_CASE("surface annotations bridge holes with surface anchored endpoints")
{
    Fixture fixture;
    bool multiple = false;
    SUBCASE("one hole") {}
    SUBCASE("two holes") { multiple = true; }
    fixture.state.files[0].mesh = holedSurface(multiple);
    for (const auto shape : {AnnotationShape::line, AnnotationShape::rectangle}) {
        for (const bool reversed : {false, true}) {
            const std::array<float, 2> start = reversed ? std::array<float, 2>{.8f,.4f} : std::array<float, 2>{-.8f,-.4f};
            const std::array<float, 2> end{-start[0], -start[1]};
            const auto geometry = projectAnnotation(fixture.projection(), shape, start, end);
            CHECK(geometry == projectAnnotation(fixture.projection(), shape, start, end));
            const auto id = createAnnotation(fixture.state, fixture.target(), geometry);
            const auto& item = *findAnnotation(fixture.state, id);
            const auto lines = annotationWorldLines(item, scenePickParts(fixture.state));
            REQUIRE(lines.size() == geometry.segments.size());
            size_t bridges = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i) { nearPoint(lines[i-1].b, lines[i].a); }
                for (const auto& point : {lines[i].a, lines[i].b}) {
                    CHECK(point[2] == doctest::Approx(.5f + .2f * point[0]));
                }
                if (geometry.segments[i].endTriangle) {
                    ++bridges;
                    const auto& a = lines[i].a; const auto& b = lines[i].b;
                    const std::array<float, 2> middle{(a[0] + b[0]) * .5f, (a[1] + b[1]) * .5f};
                    CHECK(pickAnnotationSurface(fixture.projection(), middle) == 0);
                    CHECK(annotationEdgeHit(item, scenePickParts(fixture.state), view(),
                        {(middle[0] + 1) * 100, (1 - middle[1]) * 100}));
                }
            }
            CHECK(bridges == (shape == AnnotationShape::line ? 1u : 2u) * (multiple ? 2u : 1u));
            const auto controls = annotationVertices(fixture.state, item);
            REQUIRE(controls.size() == (shape == AnnotationShape::line ? 2u : 4u));
            for (const auto& point : controls) {
                CHECK(pickAnnotationSurface(fixture.projection(), {point[0], point[1]}) == fixture.target());
            }
            nearPoint(controls.front(), {start[0], start[1], .5f + .2f * start[0]});
            if (shape == AnnotationShape::rectangle) { nearPoint(lines.back().b, lines.front().a); }
            deleteAnnotation(fixture.state, id);
        }
    }
}
TEST_CASE("annotation controls cannot land in a hole")
{
    Fixture fixture;
    fixture.state.files[0].mesh = holedSurface();
    const auto projection = fixture.projection();
    CHECK_THROWS_WITH((void)projectAnnotation(projection, AnnotationShape::line, {-.8f,0}, {0,0}),
        "Keep every endpoint or corner on the target surface.");
    CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::line, {0,0}, {.8f,0}));
    // Both drag controls hit the model, but the other rectangle corner hits the hole.
    CHECK(pickAnnotationSurface(projection, {0,-.8f}) == fixture.target());
    CHECK(pickAnnotationSurface(projection, {.8f,0}) == fixture.target());
    CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::rectangle, {0,-.8f}, {.8f,0}));
}
TEST_CASE("annotation bridges survive editing transforms scene files and history")
{
    Fixture fixture;
    fixture.state.files[0].mesh = holedSurface();
    const auto id = fixture.add(AnnotationShape::rectangle);
    const auto clean = createSceneDocument(fixture.state);
    SceneHistory history; resetSceneHistory(history, fixture.state);
    const auto parts = scenePickParts(fixture.state);
    const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& part) { return part.objectId == fixture.target(); });
    REQUIRE(target != parts.end());
    const auto projection = annotationEditProjection(*target, findAnnotation(fixture.state, id)->geometry);
    const auto edited = projectAnnotation(projection, AnnotationShape::rectangle, {-.7f,-.3f}, {.7f,.3f});
    reshapeAnnotation(fixture.state, id, edited);
    REQUIRE(recordSceneHistory(history, fixture.state));
    auto undo = prepareSceneHistoryStep(history, fixture.state, clean, false);
    REQUIRE(undo); commitSceneHistoryStep(history, fixture.state, std::move(*undo), false);
    CHECK(fixture.state.annotations[0].geometry == clean.annotations[0].geometry);
    auto redo = prepareSceneHistoryStep(history, fixture.state, clean, true);
    REQUIRE(redo); commitSceneHistoryStep(history, fixture.state, std::move(*redo), true);
    CHECK(fixture.state.annotations[0].geometry == edited);
    const auto before = annotationWorldLines(fixture.state.annotations[0], scenePickParts(fixture.state));
    setFileTranslation(fixture.state.files[0].fileSettings, {3,4,5});
    const auto moved = annotationWorldLines(fixture.state.annotations[0], scenePickParts(fixture.state));
    REQUIRE(moved.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        nearPoint(moved[i].a, {before[i].a[0]+3, before[i].a[1]+4, before[i].a[2]+5});
        nearPoint(moved[i].b, {before[i].b[0]+3, before[i].b[1]+4, before[i].b[2]+5});
    }
    std::filesystem::create_directories(fixture.root);
    const auto path = fixture.root / "bridges.woby";
    const auto document = createSceneDocument(fixture.state);
    writeSceneDocument(path, document);
    { std::ifstream stream(path); std::string line; std::getline(stream, line); std::getline(stream, line); CHECK(line == "version = 16"); }
    const auto read = readSceneDocument(path);
    CHECK(read.annotations == document.annotations);
    const auto restored = prepareSceneReplacement(fixture.state, fixture.state.files, read);
    REQUIRE(restored.annotations.size() == 1);
    CHECK(restored.annotations[0].targetValid);
    const auto loaded = annotationWorldLines(restored.annotations[0], scenePickParts(restored));
    REQUIRE(loaded.size() == moved.size());
    for (size_t i = 0; i < loaded.size(); ++i) { nearPoint(loaded[i].a, moved[i].a); nearPoint(loaded[i].b, moved[i].b); }
}
TEST_CASE("annotation bridge endpoint triangle is validated at operation and load boundaries")
{
    Fixture fixture;
    fixture.state.files[0].mesh = holedSurface();
    const auto id = fixture.add();
    auto malformed = findAnnotation(fixture.state, id)->geometry;
    auto bridge = std::find_if(malformed.segments.begin(), malformed.segments.end(), [](const auto& segment) { return segment.endTriangle.has_value(); });
    REQUIRE(bridge != malformed.segments.end());
    bridge->endTriangle = 10000;
    CHECK_THROWS(createAnnotation(fixture.state, fixture.target(), malformed));
    CHECK_THROWS(reshapeAnnotation(fixture.state, id, malformed));
    auto document = createSceneDocument(fixture.state);
    document.annotations[0].geometry = malformed;
    const auto restored = prepareSceneReplacement(fixture.state, fixture.state.files, document);
    REQUIRE(restored.annotations.size() == 1);
    CHECK_FALSE(restored.annotations[0].targetValid);
    CHECK(annotationWorldLines(restored.annotations[0], scenePickParts(restored)).empty());
}
TEST_CASE("annotation bridges remain on the drawn outline under perspective")
{
    Fixture fixture;
    fixture.state.files[0].mesh = holedSurface();
    auto perspective = view();
    perspective.projection[3] = .2f;
    const auto projection = annotationProjection(scenePickParts(fixture.state), perspective, fixture.target());
    const auto geometry = projectAnnotation(projection, AnnotationShape::line, {-.7f,-.3f}, {.7f,.3f});
    const auto id = createAnnotation(fixture.state, fixture.target(), geometry);
    const auto lines = annotationWorldLines(*findAnnotation(fixture.state, id), scenePickParts(fixture.state));
    REQUIRE(lines.size() == geometry.segments.size());
    bool bridge = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!geometry.segments[i].endTriangle) { continue; }
        bridge = true;
        for (float t : {0.0f, .5f, 1.0f}) {
            std::array<float, 4> p{0,0,0,1};
            for (size_t axis = 0; axis < 3; ++axis) { p[axis] = lines[i].a[axis] + t * (lines[i].b[axis] - lines[i].a[axis]); }
            const auto clip = annotationTransform(geometry.projector, p);
            CHECK(std::abs(.7f * clip[1] / clip[3] - .3f * clip[0] / clip[3]) < 1e-6f);
        }
    }
    CHECK(bridge);
}
TEST_CASE("annotation pointer draws across a hole and rejects moving a main vertex into it")
{
    Fixture fixture;
    fixture.state.files[0].mesh = holedSurface();
    AnnotationInteraction interaction; interaction.tool = AnnotationShape::line;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    moveAnnotationPointer(fixture.state, interaction, {180,60});
    REQUIRE(interaction.error.empty());
    CHECK(std::any_of(interaction.preview.geometry.segments.begin(), interaction.preview.geometry.segments.end(),
        [](const auto& segment) { return segment.endTriangle.has_value(); }));
    endAnnotationPointer(fixture.state, interaction, true);
    REQUIRE(fixture.state.annotations.size() == 1);
    const auto original = fixture.state.annotations[0].geometry;
    interaction.tool.reset();
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    moveAnnotationPointer(fixture.state, interaction, {100,100});
    CHECK_FALSE(interaction.error.empty());
    endAnnotationPointer(fixture.state, interaction, true);
    CHECK(fixture.state.annotations[0].geometry == original);
}
TEST_CASE("annotation placement honors target and opaque occlusion")
{
    Fixture fixture;
    auto front = surface();
    for (auto& vertex : front.vertices) { vertex.position[2] = .1f; }
    fixture.state.files.push_back(createUiFileState(fixture.root / "front.obj", front, 1));
    appendDefaultSceneNodesForFiles(fixture.state, 1);
    const auto projection = fixture.projection();
    CHECK(pickAnnotationSurface(projection, {0, 0}) == fixture.state.files[1].groupSettings[0].objectId);
    CHECK_THROWS(fixture.geometry());
    setFileOpacity(fixture.state.files[1].fileSettings, .3f);
    CHECK_NOTHROW(fixture.geometry());
    setGroupRenderMode(fixture.state.files[0].groupSettings[0], UiRenderMode::solidMesh, false);
    setGroupRenderMode(fixture.state.files[0].groupSettings[0], UiRenderMode::triangles, true);
    CHECK_NOTHROW(fixture.geometry());
}
TEST_CASE("annotation geometry rejects invalid controls and malformed persisted anchors")
{
    Fixture fixture;
    const auto projection = fixture.projection();
    CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::line, {0,0}, {0,0}));
    CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::rectangle, {0,0}, {0,.5f}));
    CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::line, {0,0}, {2,.5f}));
    CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::line, {0,0}, {std::numeric_limits<float>::quiet_NaN(),.5f}));
    auto geometry = fixture.geometry();
    geometry.segments[0].a = {1,1,1};
    CHECK_THROWS(createAnnotation(fixture.state, fixture.target(), geometry));
    CHECK(fixture.state.annotations.empty());
    geometry = fixture.geometry(); geometry.segments[0].triangle = 1000;
    CHECK_THROWS(createAnnotation(fixture.state, fixture.target(), geometry));
    CHECK(fixture.state.annotations.empty());
    geometry = fixture.geometry(); geometry.projector.fill(0);
    CHECK_THROWS(validateAnnotationGeometry(geometry));
}
TEST_CASE("surface annotation follows composed transforms and does not reproject during navigation")
{
    Fixture fixture;
    const auto id = fixture.add();
    const auto original = annotationWorldLines(*findAnnotation(fixture.state, id), scenePickParts(fixture.state));
    setFileTranslation(fixture.state.files[0].fileSettings, {3,4,5});
    setGroupTranslation(fixture.state.files[0].groupSettings[0], {1,2,3});
    const auto moved = annotationWorldLines(*findAnnotation(fixture.state, id), scenePickParts(fixture.state));
    REQUIRE(moved.size() == original.size());
    REQUIRE_FALSE(original.empty());
    for (size_t i = 0; i < original.size(); ++i) {
        nearPoint(moved[i].a, {original[i].a[0]+4, original[i].a[1]+6, original[i].a[2]+8});
        nearPoint(moved[i].b, {original[i].b[0]+4, original[i].b[1]+6, original[i].b[2]+8});
    }
    const auto document = createSceneDocument(fixture.state);
    clearSceneDirty(fixture.state); orbitUiCamera(fixture.state, 25, 30);
    CHECK(sceneContentEqual(createSceneDocument(fixture.state), document));
    CHECK_FALSE(fixture.state.isDirty);
    CHECK(selectedSceneBounds(fixture.state));
    setFileVisible(fixture.state.files[0], false);
    CHECK(annotationWorldLines(*findAnnotation(fixture.state, id), scenePickParts(fixture.state)).empty());
}
TEST_CASE("annotation drawing commits once and cancellation creates no history")
{
    Fixture fixture;
    const auto clean = createSceneDocument(fixture.state);
    SceneHistory history; resetSceneHistory(history, fixture.state);
    AnnotationInteraction interaction; interaction.tool = AnnotationShape::rectangle;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    moveAnnotationPointer(fixture.state, interaction, {180,60});
    REQUIRE(interaction.error.empty());
    CHECK(fixture.state.annotations.empty());
    CHECK_FALSE(recordSceneHistory(history, fixture.state));
    cancelAnnotationPointer(interaction);
    CHECK(sceneContentEqual(clean, createSceneDocument(fixture.state)));
    interaction.tool = AnnotationShape::rectangle;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    moveAnnotationPointer(fixture.state, interaction, {150,70});
    moveAnnotationPointer(fixture.state, interaction, {180,60});
    endAnnotationPointer(fixture.state, interaction, true);
    REQUIRE(fixture.state.annotations.size() == 1);
    REQUIRE(recordSceneHistory(history, fixture.state));
    CHECK(history.snapshots.size() == 2);
    auto restored = prepareSceneHistoryStep(history, fixture.state, clean, false);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.state, std::move(*restored), false);
    CHECK(fixture.state.annotations.empty());
    restored = prepareSceneHistoryStep(history, fixture.state, clean, true);
    REQUIRE(restored); commitSceneHistoryStep(history, fixture.state, std::move(*restored), true);
    REQUIRE(fixture.state.annotations.size() == 1);
    CHECK(fixture.state.annotations[0].targetValid);
}
TEST_CASE("invalid annotation drag cannot commit an earlier valid preview")
{
    Fixture fixture;
    AnnotationInteraction interaction; interaction.tool = AnnotationShape::line;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    moveAnnotationPointer(fixture.state, interaction, {180,60});
    REQUIRE(interaction.error.empty());
    moveAnnotationPointer(fixture.state, interaction, {250,60});
    CHECK_FALSE(interaction.error.empty());
    endAnnotationPointer(fixture.state, interaction, true);
    CHECK(fixture.state.annotations.empty());
}
TEST_CASE("annotation handles reshape with the frozen projector and respect locking")
{
    Fixture fixture;
    const auto id = fixture.add();
    AnnotationInteraction interaction;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    CHECK(interaction.editing == id);
    moveAnnotationPointer(fixture.state, interaction, {35,130});
    REQUIRE(interaction.error.empty());
    endAnnotationPointer(fixture.state, interaction, true);
    REQUIRE(findAnnotation(fixture.state, id));
    CHECK(findAnnotation(fixture.state, id)->geometry.start[0] == doctest::Approx(-.65f));
    auto style = findAnnotation(fixture.state, id)->settings; style.locked = true;
    setAnnotationSettings(fixture.state, id, style);
    CHECK_FALSE(beginAnnotationPointer(fixture.state, interaction, view(), {35,130}));
    CHECK_THROWS(reshapeAnnotation(fixture.state, id, fixture.geometry()));
}
TEST_CASE("annotation save load preserves shapes notes style and source attachment")
{
    Fixture fixture(true);
    const auto id = fixture.add(AnnotationShape::rectangle);
    auto style = findAnnotation(fixture.state, id)->settings;
    style.note = "Review \"this\" range\nsecond line"; style.width = 12; style.locked = true; style.color = {.2f,.4f,.6f,.8f};
    setAnnotationSettings(fixture.state, id, style);
    const auto savedView = createView(fixture.state);
    (void)savedView;
    std::filesystem::create_directories(fixture.root);
    const auto path = fixture.root / "scene.woby";
    const auto document = createSceneDocument(fixture.state);
    writeSceneDocument(path, document);
    auto read = readSceneDocument(path);
    CHECK(read.annotations == document.annotations);
    auto restored = prepareSceneReplacement(fixture.state, fixture.state.files, read);
    REQUIRE(restored.annotations.size() == 1);
    CHECK(restored.annotations[0].objectId != id);
    CHECK(restored.annotations[0].targetId == restored.files[0].groupSettings[0].objectId);
    CHECK(restored.annotations[0].targetValid);
    CHECK(restored.annotations[0].settings == style);
    CHECK(restored.annotations[0].geometry == document.annotations[0].geometry);
    auto changed = restored.annotations[0].settings; changed.width = 1; changed.visible = false;
    setAnnotationSettings(restored, restored.annotations[0].objectId, changed);
    REQUIRE(restored.views.size() == 1);
    applyView(restored, restored.views[0].id);
    CHECK(restored.annotations[0].settings.width == 12);
    CHECK(restored.annotations[0].settings.visible);
}
TEST_CASE("changed source geometry leaves annotation unresolved on open and history restoration")
{
    Fixture fixture;
    fixture.add();
    const auto document = createSceneDocument(fixture.state);
    auto changedFiles = fixture.state.files;
    changedFiles[0].mesh.vertices[0].position[0] -= .1f;
    const auto restored = prepareSceneReplacement(fixture.state, changedFiles, document);
    REQUIRE(restored.annotations.size() == 1);
    CHECK_FALSE(restored.annotations[0].targetValid);
    CHECK(annotationWorldLines(restored.annotations[0], scenePickParts(restored)).empty());
    SceneHistory history; resetSceneHistory(history, fixture.state);
    removeFileFromState(fixture.state, 0); recordSceneHistory(history, fixture.state);
    const auto prepared = prepareSceneHistoryStep(history, fixture.state, document, false, changedFiles);
    REQUIRE(prepared); REQUIRE(prepared->annotations.size() == 1);
    CHECK_FALSE(prepared->annotations[0].targetValid);
}
TEST_CASE("missing annotation target survives save and deletion undo restores attachment")
{
    Fixture fixture;
    const auto id = fixture.add();
    const auto files = fixture.state.files;
    const auto targetId = fixture.target();
    const auto clean = createSceneDocument(fixture.state);
    SceneHistory history; resetSceneHistory(history, fixture.state);
    REQUIRE(removeFileFromState(fixture.state, 0));
    recordSceneHistory(history, fixture.state);
    REQUIRE(findAnnotation(fixture.state, id));
    CHECK(annotationWorldLines(*findAnnotation(fixture.state, id), scenePickParts(fixture.state)).empty());
    const auto missing = createSceneDocument(fixture.state);
    REQUIRE(missing.annotations.size() == 1);
    CHECK(missing.annotations[0].fileIndex == -1); CHECK(missing.annotations[0].groupIndex == -1);
    std::filesystem::create_directories(fixture.root);
    writeSceneDocument(fixture.root / "missing.woby", missing);
    CHECK(readSceneDocument(fixture.root / "missing.woby").annotations == missing.annotations);
    const auto restored = prepareSceneHistoryStep(history, fixture.state, clean, false, files);
    REQUIRE(restored); CHECK(restored->annotations[0].targetValid);
    CHECK(restored->annotations[0].targetId == targetId);
}
TEST_CASE("annotations remain selectable as scene items and render depth controls picking")
{
    Fixture fixture;
    const auto id = fixture.add();
    auto parts = scenePickParts(fixture.state);
    std::vector<std::vector<DiagnosticEdge>> storage;
    appendAnnotationPickParts(parts, fixture.state, storage);
    CHECK(pickSceneObject(parts, view(), {100,100}) == id);
    CHECK(findSceneObject(fixture.state, id)->kind == SceneObjectKind::annotation);
    auto geometry = surface(); for (auto& v : geometry.vertices) { v.position[2] = .1f; }
    fixture.state.files.push_back(createUiFileState(fixture.root / "blocker.obj", geometry, 1));
    appendDefaultSceneNodesForFiles(fixture.state, 1);
    parts = scenePickParts(fixture.state); appendAnnotationPickParts(parts, fixture.state, storage);
    CHECK(pickSceneObject(parts, view(), {100,100}) == fixture.state.files[1].groupSettings[0].objectId);
}
TEST_CASE("old scenes load without annotations and malformed annotation data is rejected")
{
    Fixture fixture;
    std::filesystem::create_directories(fixture.root);
    const auto path = fixture.root / "old.woby";
    { std::ofstream out(path); out << "version = 7\n"; }
    CHECK(readSceneDocument(path).annotations.empty());
    { std::ofstream out(path); out << "version = 8\n[[annotations.segments]]\ntriangle = 0\n"; }
    CHECK_THROWS((void)readSceneDocument(path));
    { std::ofstream out(path); out << "version = 8\n[[annotations]]\nshape = \"circle\"\n"; }
    CHECK_THROWS((void)readSceneDocument(path));
    auto document = createSceneDocument(fixture.state);
    SceneAnnotationRecord record; record.geometry = fixture.geometry(); record.fileIndex = 99; record.groupIndex = 0;
    document.annotations.push_back(record);
    writeSceneDocument(path, document);
    CHECK_THROWS((void)readSceneDocument(path));
}

TEST_CASE("surface annotation projection handles perspective and near clipping")
{
    Fixture fixture(true);
    auto perspective = view();
    bx::mtxLookAt(perspective.view.data(), {0,0,-3}, {0,0,0}, {0,1,0});
    bx::mtxProj(perspective.projection.data(), 60, 1, .1f, 10, false);
    const auto projection = annotationProjection(scenePickParts(fixture.state), perspective, fixture.target());
    const auto geometry = projectAnnotation(projection, AnnotationShape::rectangle, {-.25f,-.2f}, {.25f,.2f});
    auto gesture = annotationGestureProjection(scenePickParts(fixture.state), perspective,
        fixture.target(), {-.25f, -.2f});
    expandAnnotationGestureProjection(gesture, scenePickParts(fixture.state), perspective,
        {-.25f, -.2f}, {.25f, .2f});
    CHECK(projectAnnotation(gesture, AnnotationShape::rectangle, {-.25f,-.2f}, {.25f,.2f}) == geometry);
    REQUIRE(geometry.segments.size() >= 4);
    const auto& mesh = fixture.state.files[0].mesh;
    for (const auto& segment : geometry.segments) {
        for (const auto& bary : {segment.a, segment.b}) {
            const auto local = annotationPosition(mesh, 0, segment.triangle, bary);
            const auto clip = annotationTransform(geometry.projector, {local[0],local[1],local[2],1});
            const double x = clip[0] / clip[3], y = clip[1] / clip[3];
            CHECK((std::abs(std::abs(x)-.25f) < 1e-5f || std::abs(std::abs(y)-.2f) < 1e-5f));
        }
    }
    fixture.state.files[0].mesh.vertices[0].position[2] = -.5f;
    const auto clipped = fixture.projection();
    CHECK(pickAnnotationSurface(clipped, {-.95f,-.95f}) == 0);
    CHECK_NOTHROW((void)projectAnnotation(clipped, AnnotationShape::line, {.1f,-.5f}, {.8f,.5f}));
}

TEST_CASE("surface annotation previews on a tessellated model")
{
    Fixture fixture;
    Mesh mesh;
    constexpr uint32_t n = 100;
    for (uint32_t y = 0; y <= n; ++y) {
        for (uint32_t x = 0; x <= n; ++x) {
            Vertex vertex;
            const float u = static_cast<float>(x) / n * 2 - 1, v = static_cast<float>(y) / n * 2 - 1;
            vertex.position = {u, v, .2f + .5f * u * u}; mesh.vertices.push_back(vertex);
        }
    }
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const uint32_t a = y * (n+1) + x, b = a + 1, c = a+n+1, d = c+1;
            mesh.indices.insert(mesh.indices.end(), {a,b,d,a,d,c});
        }
    }
    mesh.nodes = {{"surface",0,static_cast<uint32_t>(mesh.indices.size())}};
    mesh.bounds = calculateBounds(mesh.vertices);
    fixture.state.files[0].mesh = std::move(mesh);
    const auto begin = std::chrono::steady_clock::now();
    const auto projection = fixture.projection();
    const auto ready = std::chrono::steady_clock::now();
    const auto geometry = projectAnnotation(projection, AnnotationShape::rectangle, {-.79f,-.43f}, {.77f,.51f});
    const auto done = std::chrono::steady_clock::now();
    REQUIRE(geometry.segments.size() > 100);
    std::cout << "Annotation 20000-triangle Debug benchmark: projection "
        << std::chrono::duration<double,std::milli>(ready-begin).count() << " ms; rectangle "
        << std::chrono::duration<double,std::milli>(done-ready).count() << " ms\n";
}

// Opt-in workload: report timings, never assert machine-dependent thresholds.
// Run with --test-case="surface annotation large mesh benchmark" --no-skip.
TEST_CASE("surface annotation large mesh benchmark" * doctest::skip())
{
    for (const auto [width, layers] : {std::pair{100u, 1u}, std::pair{700u, 1u}, std::pair{250u, 8u}}) {
        Fixture fixture;
        fixture.state.files[0].mesh = tessellatedSurface(width, layers);
        std::vector<double> setupTimes, rectangleTimes, gestureTimes, previewTimes;
        size_t segments = 0;
        size_t cacheBytes = 0;
        for (size_t run = 0; run < 3; ++run) {
            const auto begin = std::chrono::steady_clock::now();
            const auto projection = fixture.projection();
            const auto ready = std::chrono::steady_clock::now();
            cacheBytes = projection.vertices.size() * sizeof(AnnotationProjectionVertex)
                + projection.triangles.size() * sizeof(AnnotationProjectionFace)
                + projection.clippedTriangles.size() * sizeof(AnnotationProjectedTriangle)
                + projection.order.size() * sizeof(size_t) + projection.nodes.size() * sizeof(AnnotationProjectionNode);
            for (size_t move = 0; move < 10; ++move) {
                const float delta = static_cast<float>(move) * .001f;
                segments += projectAnnotation(projection, AnnotationShape::rectangle, {-.791f,-.431f}, {.771f + delta,.511f + delta}).segments.size();
            }
            const auto done = std::chrono::steady_clock::now();
            AnnotationInteraction interaction;
            interaction.tool = AnnotationShape::rectangle;
            REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {21,143}));
            REQUIRE(interaction.dragging);
            const auto gesture = std::chrono::steady_clock::now();
            for (size_t move = 0; move < 10; ++move) {
                const float delta = static_cast<float>(move) * .1f;
                moveAnnotationPointer(fixture.state, interaction, {177.1f + delta,48.9f - delta});
                REQUIRE(interaction.error.empty());
            }
            const auto preview = std::chrono::steady_clock::now();
            setupTimes.push_back(std::chrono::duration<double,std::milli>(ready-begin).count());
            rectangleTimes.push_back(std::chrono::duration<double,std::milli>(done-ready).count() / 10);
            gestureTimes.push_back(std::chrono::duration<double,std::milli>(gesture-done).count());
            previewTimes.push_back(std::chrono::duration<double,std::milli>(preview-gesture).count() / 10);
        }
        std::sort(setupTimes.begin(), setupTimes.end());
        std::sort(rectangleTimes.begin(), rectangleTimes.end());
        std::sort(gestureTimes.begin(), gestureTimes.end());
        std::sort(previewTimes.begin(), previewTimes.end());
        std::cout << "Annotation benchmark: triangles=" << fixture.state.files[0].mesh.indices.size() / 3
            << " layers=" << layers << " setup_ms=" << setupTimes[1] << " rectangle_ms=" << rectangleTimes[1]
            << " unselected_gesture_ms=" << gestureTimes[1] << " drag_ms=" << previewTimes[1]
            << " cache_bytes=" << cacheBytes << " segments_checksum=" << segments << '\n';
    }
}

// Set WOBY_ANNOTATION_MODEL to a local OBJ, then opt in with --no-skip.
TEST_CASE("surface annotation external model benchmark" * doctest::skip())
{
    std::filesystem::path modelPath;
#ifdef _WIN32
    wchar_t* value = nullptr;
    size_t length = 0;
    REQUIRE(_wdupenv_s(&value, &length, L"WOBY_ANNOTATION_MODEL") == 0);
    if (value) { modelPath = value; }
    std::free(value);
#else
    if (const auto* value = std::getenv("WOBY_ANNOTATION_MODEL")) { modelPath = value; }
#endif
    REQUIRE_FALSE(modelPath.empty());
    Fixture fixture;
    fixture.state = {};
    fixture.state.files.push_back(createUiFileState(modelPath, loadObjMesh(modelPath), 0));
    appendDefaultSceneNodesForFiles(fixture.state, 0);
    const auto& mesh = fixture.state.files[0].mesh;
    const auto largest = std::max_element(mesh.nodes.begin(), mesh.nodes.end(),
        [](const auto& a, const auto& b) { return a.indexCount < b.indexCount; });
    REQUIRE(largest != mesh.nodes.end());
    const auto target = fixture.state.files[0].groupSettings[static_cast<size_t>(largest - mesh.nodes.begin())].objectId;
    const auto camera = cameraWithView(frameCameraBounds(mesh.bounds, SceneUpAxis::y), CameraView::top);
    const auto modelView = scenePickView(camera, SceneUpAxis::y, mesh.bounds, 1920, 1080, false, 1);
    std::cout << "External model: triangles=" << mesh.indices.size() / 3 << " groups=" << mesh.nodes.size()
        << " largest=" << largest->name << " bounds=" << mesh.bounds.min[0] << ',' << mesh.bounds.min[1]
        << ',' << mesh.bounds.min[2] << " to " << mesh.bounds.max[0] << ',' << mesh.bounds.max[1]
        << ',' << mesh.bounds.max[2] << std::endl;
    std::array<float, 2> start{}, end{};
    bool found = false;
    {
        const auto projection = annotationProjection(scenePickParts(fixture.state), modelView, target);
        for (float extent : {.15f, .07f, .03f, .01f}) {
            for (int y = -8; y <= 8 && !found; ++y) {
                for (int x = -8; x <= 8 && !found; ++x) {
                    start = {static_cast<float>(x) * .05f - extent, static_cast<float>(y) * .05f - extent};
                    end = {start[0] + 2 * extent, start[1] + 2 * extent};
                    try { (void)projectAnnotation(projection, AnnotationShape::rectangle, start, end); found = true; }
                    catch (const std::runtime_error&) { }
                }
            }
            if (found) { break; }
        }
    }
    REQUIRE(found);
    const auto pointer = [&](const auto& point) -> PickPoint {
        return {(point[0] + 1) * static_cast<float>(modelView.width) * .5f,
            (1 - point[1]) * static_cast<float>(modelView.height) * .5f};
    };
    std::vector<double> setupTimes, dragTimes, firstDragTimes, steadyDragTimes, exactTimes, commitTimes;
    size_t segments = 0, faces = 0;
    for (size_t run = 0; run < 3; ++run) {
        AnnotationInteraction interaction;
        interaction.tool = AnnotationShape::rectangle;
        const auto begin = std::chrono::steady_clock::now();
        REQUIRE(beginAnnotationPointer(fixture.state, interaction, modelView, pointer(start)));
        REQUIRE(interaction.dragging);
        const auto ready = std::chrono::steady_clock::now();
        moveAnnotationPointer(fixture.state, interaction, pointer(end));
        const auto firstMoved = std::chrono::steady_clock::now();
        for (size_t i = 1; i < 10; ++i) { moveAnnotationPointer(fixture.state, interaction, pointer(end)); }
        const auto moved = std::chrono::steady_clock::now();
        REQUIRE(interaction.error.empty());
        const auto exact = projectAnnotation(interaction.projection, AnnotationShape::rectangle, interaction.start, interaction.end);
        const auto resolved = std::chrono::steady_clock::now();
        faces = interaction.projection.triangles.size();
        endAnnotationPointer(fixture.state, interaction, true);
        const auto done = std::chrono::steady_clock::now();
        REQUIRE(interaction.error.empty());
        REQUIRE(fixture.state.annotations.size() == 1);
        CHECK(fixture.state.annotations.front().geometry == exact);
        segments = exact.segments.size();
        setupTimes.push_back(std::chrono::duration<double,std::milli>(ready-begin).count());
        dragTimes.push_back(std::chrono::duration<double,std::milli>(moved-ready).count() / 10);
        firstDragTimes.push_back(std::chrono::duration<double,std::milli>(firstMoved-ready).count());
        steadyDragTimes.push_back(std::chrono::duration<double,std::milli>(moved-firstMoved).count() / 9);
        exactTimes.push_back(std::chrono::duration<double,std::milli>(resolved-moved).count());
        commitTimes.push_back(std::chrono::duration<double,std::milli>(done-resolved).count());
        deleteAnnotation(fixture.state, fixture.state.annotations.front().objectId);
    }
    for (auto* times : {&setupTimes, &dragTimes, &firstDragTimes, &steadyDragTimes, &exactTimes, &commitTimes}) {
        std::sort(times->begin(), times->end());
    }
    std::cout << "External annotation benchmark: triangles=" << mesh.indices.size() / 3 << " groups=" << mesh.nodes.size()
        << " projected_faces=" << faces << " target=" << largest->name << " start=" << start[0] << ',' << start[1]
        << " end=" << end[0] << ',' << end[1] << " setup_ms=" << setupTimes[1] << " drag_ms=" << dragTimes[1]
        << " first_drag_ms=" << firstDragTimes[1] << " steady_drag_ms=" << steadyDragTimes[1]
        << " exact_ms=" << exactTimes[1] << " commit_ms=" << commitTimes[1] << " segments=" << segments << '\n';
}

TEST_CASE("surface annotations handle thousands of coincident faces without an overlap limit")
{
    Fixture fixture;
    const auto originalProjection = fixture.projection();
    const auto original = fixture.state.files[0].mesh.indices;
    auto& mesh = fixture.state.files[0].mesh;
    for (size_t i = 1; i < 2500; ++i) {
        mesh.indices.insert(mesh.indices.end(), original.begin(), original.end());
    }
    mesh.nodes[0].indexCount = static_cast<uint32_t>(mesh.indices.size());
    const auto projection = fixture.projection();
    for (const auto shape : {AnnotationShape::line, AnnotationShape::rectangle}) {
        const auto geometry = projectAnnotation(projection, shape, {-.8f,-.4f}, {.8f,.4f});
        REQUIRE_FALSE(geometry.segments.empty());
        for (const auto& segment : geometry.segments) {
            CHECK(segment.triangle < 4);
        }
        CHECK(geometry.segments == projectAnnotation(originalProjection, shape, {-.8f,-.4f}, {.8f,.4f}).segments);
        CHECK(geometry == projectAnnotation(projection, shape, {-.8f,-.4f}, {.8f,.4f}));
    }
}

TEST_CASE("surface annotation visibility ignores hidden crossings and detects narrow occluders")
{
    Fixture fixture;
    auto& mesh = fixture.state.files[0].mesh;
    for (auto& vertex : mesh.vertices) { vertex.position[2] = .1f; }
    const auto originalProjection = fixture.projection();
    for (size_t layer = 0; layer < 80; ++layer) {
        auto rear = surface();
        const float slope = static_cast<float>(layer) / 100;
        const auto offset = static_cast<uint32_t>(mesh.vertices.size());
        for (auto& vertex : rear.vertices) {
            vertex.position[2] = .55f + slope * .4f * vertex.position[0];
            mesh.vertices.push_back(vertex);
        }
        for (auto index : rear.indices) { mesh.indices.push_back(offset + index); }
    }
    mesh.nodes[0].indexCount = static_cast<uint32_t>(mesh.indices.size());
    SUBCASE("hidden surface intersections do not split the frontmost outline") {
        const auto geometry = fixture.geometry();
        const auto expected = projectAnnotation(originalProjection, AnnotationShape::line, {-.8f,-.4f}, {.8f,.4f});
        REQUIRE(geometry.segments.size() == expected.segments.size());
        for (size_t i = 0; i < geometry.segments.size(); ++i) {
            CHECK(geometry.segments[i].triangle == expected.segments[i].triangle);
            nearPoint(geometry.segments[i].a, expected.segments[i].a);
            nearPoint(geometry.segments[i].b, expected.segments[i].b);
        }
    }
    SUBCASE("a narrow foreground interval still rejects the complete outline") {
        auto front = surface();
        for (auto& vertex : front.vertices) { vertex.position[0] = .123f + vertex.position[0] * .00001f; vertex.position[2] = .01f; }
        front.bounds = calculateBounds(front.vertices);
        fixture.state.files.push_back(createUiFileState(fixture.root / "occluder.obj", std::move(front), 1));
        appendDefaultSceneNodesForFiles(fixture.state, 1);
        CHECK_THROWS_WITH(fixture.geometry(), "Keep the outline clear of other objects.");
    }
    SUBCASE("a depth crossing can make a different object become frontmost") {
        auto crossing = surface();
        for (auto& vertex : crossing.vertices) { vertex.position[2] = .12f - vertex.position[0] * .1f; }
        crossing.bounds = calculateBounds(crossing.vertices);
        fixture.state.files.push_back(createUiFileState(fixture.root / "crossing.obj", std::move(crossing), 1));
        appendDefaultSceneNodesForFiles(fixture.state, 1);
        CHECK_THROWS_WITH(fixture.geometry(), "Keep every endpoint or corner on the target surface.");
    }
}

TEST_CASE("annotation projection keeps shared mesh groups in their own transforms")
{
    const auto mesh = surface();
    ScenePickPart first;
    first.mesh = &mesh; first.objectId = 1; first.indexCount = mesh.indices.size(); first.solid = true;
    bx::mtxIdentity(first.model.data());
    auto second = first;
    second.objectId = 2;
    bx::mtxTranslate(second.model.data(), 1.5f, 0, -.1f);
    std::array<ScenePickPart, 2> parts{first, second};
    for (int order = 0; order < 2; ++order) {
        const auto projection = annotationProjection(parts, view(), first.objectId);
        CHECK(pickAnnotationSurface(projection, {-.8f, 0}) == first.objectId);
        CHECK(pickAnnotationSurface(projection, {.8f, 0}) == second.objectId);
        CHECK_NOTHROW((void)projectAnnotation(projection, AnnotationShape::line, {-.8f,0}, {.2f,0}));
        CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::line, {-.8f,0}, {.8f,0}));
        std::swap(parts[0], parts[1]);
    }
}


TEST_CASE("annotation projection shares vertices and owns them after the source changes")
{
    Fixture fixture;
    const auto projection = fixture.projection();
    REQUIRE(projection.vertices.size() == fixture.state.files[0].mesh.vertices.size());
    CHECK(projection.clippedTriangles.empty());
    CHECK(sizeof(AnnotationProjectionFace) < sizeof(AnnotationProjectedTriangle));
    const auto expected = projectAnnotation(projection, AnnotationShape::rectangle, {-.8f,-.4f}, {.8f,.4f});
    fixture.state.files[0].mesh = {};
    CHECK(projectAnnotation(projection, AnnotationShape::rectangle, {-.8f,-.4f}, {.8f,.4f}) == expected);
}

TEST_CASE("annotation discovery cache can select a target without reprojecting its vertices")
{
    Fixture fixture;
    auto front = surface();
    for (auto& vertex : front.vertices) { vertex.position[2] = .1f; }
    fixture.state.files.push_back(createUiFileState(fixture.root / "transparent.obj", std::move(front), 1));
    appendDefaultSceneNodesForFiles(fixture.state, 1);
    setFileOpacity(fixture.state.files[1].fileSettings, .5f);
    const auto parts = scenePickParts(fixture.state);
    for (const auto& part : parts) {
        auto discovered = annotationProjection(parts, view(), 0);
        const auto* vertices = discovered.vertices.data();
        setAnnotationProjectionTarget(discovered, parts, view(), part.objectId);
        CHECK(discovered.vertices.data() == vertices);
        const auto expected = annotationProjection(parts, view(), part.objectId);
        CHECK(projectAnnotation(discovered, AnnotationShape::rectangle, {-.8f,-.4f}, {.8f,.4f})
            == projectAnnotation(expected, AnnotationShape::rectangle, {-.8f,-.4f}, {.8f,.4f}));
    }
}

TEST_CASE("sampled annotation guides have bounded work and preserve corners under perspective")
{
    Fixture fixture;
    fixture.state.files[0].mesh = tessellatedSurface(160);
    auto perspective = view();
    perspective.projection[3] = .2f;
    const auto projection = annotationProjection(scenePickParts(fixture.state), perspective, fixture.target());
    const auto guide = previewAnnotation(projection, AnnotationShape::rectangle, {-.7f,-.3f}, {.7f,.3f});
    CHECK(guide.segments.size() <= 128);
    const auto exact = projectAnnotation(projection, AnnotationShape::rectangle, {-.7f,-.3f}, {.7f,.3f});
    REQUIRE(exact.segments.size() > guide.segments.size());
    const auto controls = annotationControlPositions(fixture.state.files[0].mesh, 0, guide);
    const auto expected = annotationControlPositions(fixture.state.files[0].mesh, 0, exact);
    REQUIRE(controls.size() == 4);
    for (size_t i = 0; i < controls.size(); ++i) { nearPoint(controls[i], expected[i]); }
    CHECK_THROWS((void)previewAnnotation(projection, AnnotationShape::rectangle, {0,0}, {0,.4f}));
    CHECK_THROWS((void)previewAnnotation(projection, AnnotationShape::line, {0,0}, {2,.4f}));
}

TEST_CASE("annotation point traversal agrees with a full scan including depth ties")
{
    Fixture fixture;
    fixture.state.files[0].mesh = tessellatedSurface(12, 3);
    auto& mesh = fixture.state.files[0].mesh;
    const auto original = mesh.indices;
    mesh.indices.insert(mesh.indices.end(), original.begin(), original.end());
    mesh.nodes[0].indexCount = static_cast<uint32_t>(mesh.indices.size());
    const auto projection = fixture.projection();
    auto scanned = projection;
    scanned.nodes.resize(1);
    scanned.nodes[0].left = scanned.nodes[0].right = 0;
    scanned.nodes[0].begin = 0; scanned.nodes[0].end = scanned.order.size();
    for (int y = -10; y <= 10; ++y) {
        for (int x = -10; x <= 10; ++x) {
            const std::array<float, 2> point{static_cast<float>(x) / 10, static_cast<float>(y) / 10};
            const auto hit = annotationSurfaceHit(projection, point), expected = annotationSurfaceHit(scanned, point);
            REQUIRE(hit);
            REQUIRE(expected);
            CHECK(hit->objectId == expected->objectId);
            CHECK(hit->triangle == expected->triangle);
            nearPoint(hit->bary, expected->bary);
        }
    }
}

TEST_CASE("dense annotation gestures resolve the sampled guide before committing or reshaping")
{
    Fixture fixture;
    fixture.state.files[0].mesh = tessellatedSurface(160);
    AnnotationInteraction interaction;
    interaction.tool = AnnotationShape::rectangle;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {21,143}));
    REQUIRE(interaction.sampledPreview);
    moveAnnotationPointer(fixture.state, interaction, {177,49});
    REQUIRE(interaction.error.empty());
    REQUIRE(interaction.preview.geometry.segments.size() <= 128);
    const auto expected = projectAnnotation(interaction.projection, AnnotationShape::rectangle, interaction.start, interaction.end);
    REQUIRE(expected.segments.size() > 128);
    SUBCASE("cancel leaves no annotation") {
        cancelAnnotationPointer(interaction);
        CHECK(fixture.state.annotations.empty());
    }
    SUBCASE("scene changes reject the sampled preview") {
        markSceneDirty(fixture.state);
        endAnnotationPointer(fixture.state, interaction, true);
        CHECK(fixture.state.annotations.empty());
        CHECK_FALSE(interaction.error.empty());
    }
    SUBCASE("release stores exact geometry and handle edits do too") {
        endAnnotationPointer(fixture.state, interaction, true);
        REQUIRE(fixture.state.annotations.size() == 1);
        CHECK(fixture.state.annotations.front().geometry == expected);
        REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {21,143}));
        REQUIRE(interaction.sampledPreview);
        REQUIRE(interaction.editing == fixture.state.annotations.front().objectId);
        moveAnnotationPointer(fixture.state, interaction, {31,133});
        REQUIRE(interaction.error.empty());
        const auto edited = projectAnnotation(interaction.projection, AnnotationShape::rectangle, interaction.start, interaction.end);
        endAnnotationPointer(fixture.state, interaction, true);
        CHECK(fixture.state.annotations.front().geometry == edited);
    }
}

TEST_CASE("sampled annotation gestures cannot commit narrow occluders missed by the guide")
{
    Fixture fixture;
    fixture.state.files[0].mesh = tessellatedSurface(160);
    auto front = surface();
    for (auto& vertex : front.vertices) {
        vertex.position[0] = .12345f + vertex.position[0] * .00001f;
        vertex.position[2] = .01f;
    }
    front.bounds = calculateBounds(front.vertices);
    fixture.state.files.push_back(createUiFileState(fixture.root / "thin.obj", std::move(front), 1));
    appendDefaultSceneNodesForFiles(fixture.state, 1);
    AnnotationInteraction interaction;
    interaction.tool = AnnotationShape::rectangle;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {20,140}));
    REQUIRE(interaction.sampledPreview);
    moveAnnotationPointer(fixture.state, interaction, {180,60});
    REQUIRE(interaction.error.empty());
    endAnnotationPointer(fixture.state, interaction, true);
    CHECK(fixture.state.annotations.empty());
    CHECK(interaction.error == "Keep the outline clear of other objects.");
}

TEST_CASE("annotation handle visibility waits until camera interaction ends")
{
    AnnotationUiFixture fixture;
    fixture.scene.add(AnnotationShape::rectangle);
    fixture.frame(); fixture.frame();
    REQUIRE(fixture.interaction.overlayReady);
    REQUIRE(fixture.interaction.overlayHandles.size() == 4);

    fixture.pointerAllowed = false;
    fixture.pickView.view[12] += .05f;
    fixture.frame();
    REQUIRE_FALSE(fixture.interaction.overlayReady);
    CHECK(fixture.interaction.overlayHandles.empty());
    // Several frames without motion still belong to the same camera drag.
    for (int frame = 0; frame < 3; ++frame) {
        fixture.frame();
        CHECK_FALSE(fixture.interaction.overlayReady);
        CHECK(fixture.interaction.overlayHandles.empty());
    }
    fixture.pointerAllowed = true;
    fixture.frame();
    CHECK(fixture.interaction.overlayReady);
    CHECK(fixture.interaction.overlayHandles.size() == 4);
}

// Set WOBY_ANNOTATION_SCENE to a local scene, then opt in with --no-skip.
TEST_CASE("annotation navigation overlay external scene benchmark" * doctest::skip())
{
    std::filesystem::path scenePath;
#ifdef _WIN32
    wchar_t* value = nullptr;
    size_t length = 0;
    REQUIRE(_wdupenv_s(&value, &length, L"WOBY_ANNOTATION_SCENE") == 0);
    if (value) { scenePath = value; }
    std::free(value);
#else
    if (const auto* value = std::getenv("WOBY_ANNOTATION_SCENE")) { scenePath = value; }
#endif
    REQUIRE_FALSE(scenePath.empty());
    auto document = readSceneDocument(scenePath);
    for (auto& file : document.files) { file.path = sceneAbsolutePath(scenePath, file.path); }
    AnnotationUiFixture fixture;
    std::vector<UiFileState> files;
    for (const auto& file : document.files) {
        files.push_back(createUiFileState(file.path, loadObjMesh(file.path), files.size()));
    }
    fixture.scene.state = prepareSceneReplacement(fixture.scene.state, std::move(files), document);
    auto& state = fixture.scene.state;
    REQUIRE_FALSE(state.annotations.empty());
    state.selectedSceneObjects = {state.annotations.front().objectId};
    fixture.pointerAllowed = false;
    for (int run = 0; run < 3; ++run) {
        for (int axis = 0; axis < 2; ++axis) {
            double navigationMs = 0, settleMs = 0;
            for (int step = 0; step < 6; ++step) {
                orbitUiCamera(state, axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f);
                fixture.pickView = scenePickView(state.camera, state.upAxis, state.sceneBounds, 1920, 1080, false, 1);
                const auto start = std::chrono::steady_clock::now();
                fixture.frame();
                // Input events need not arrive every render frame during a drag.
                fixture.frame();
                navigationMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                fixture.pointerAllowed = true;
                const auto settle = std::chrono::steady_clock::now();
                fixture.frame();
                settleMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - settle).count();
                fixture.pointerAllowed = false;
            }
            std::cout << "Annotation overlay axis=" << (axis == 0 ? "horizontal" : "vertical")
                << " navigation_ms_per_frame=" << navigationMs / 12
                << " settled_visibility_ms=" << settleMs / 6 << std::endl;
        }
    }
}

TEST_CASE("annotation overlays offset their handles and input below the main menu")
{
    AnnotationUiFixture fixture;
    fixture.interaction.tool = AnnotationShape::line;
    fixture.frame();
    const float originalBottom = fixture.messageBottom;
    fixture.windowY = 40;
    fixture.frame();
    CHECK(fixture.messageBottom == doctest::Approx(originalBottom + 40));
    CHECK(fixture.cursor({400, 20}) != ImGuiMouseCursor_None);
    CHECK(fixture.cursor({400, 100}) == ImGuiMouseCursor_None);
    fixture.interaction.tool.reset();
    fixture.interaction.overlayReady = true;
    fixture.interaction.overlayHandles = {{100, 100}};
    CHECK(fixture.cursor({400, 140}) == ImGuiMouseCursor_ResizeAll);
    CHECK(fixture.cursor({400, 100}) != ImGuiMouseCursor_ResizeAll);
}

TEST_CASE("annotations cross touching transformed siblings without matching seam vertices")
{
    for (const auto shape : {AnnotationShape::line, AnnotationShape::rectangle}) {
        Fixture fixture; siblingSurface(fixture);
        const auto id = drawSiblingAnnotation(fixture, shape);
        const auto& item = *findAnnotation(fixture.state, id);
        REQUIRE(item.targetIds.size() == 2);
        REQUIRE(item.geometry.sources.size() == 2);
        CHECK(item.targetValid);
        const auto lines = annotationWorldLines(item, scenePickParts(fixture.state));
        REQUIRE(lines.size() == item.geometry.segments.size());
        bool left = false, right = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            left |= item.geometry.segments[i].source == 0;
            right |= item.geometry.segments[i].source == 1;
            if (i) { nearPoint(lines[i-1].b, lines[i].a); }
            CHECK(lines[i].a[2] == doctest::Approx(.5));
            CHECK(lines[i].b[2] == doctest::Approx(.5));
        }
        CHECK(left); CHECK(right);
        const auto controls = annotationVertices(fixture.state, item);
        REQUIRE(controls.size() == (shape == AnnotationShape::line ? 2u : 4u));
        nearPoint(controls.front(), {-.8f,-.4f,.5f});
        nearPoint(controls[shape == AnnotationShape::line ? 1 : 2], {.8f,.4f,.5f});
    }
}
TEST_CASE("sibling gap bridges retain separate endpoint attachments after transforms")
{
    Fixture fixture; siblingSurface(fixture, .2f);
    const auto id = drawSiblingAnnotation(fixture);
    const auto item = *findAnnotation(fixture.state, id);
    const auto before = annotationWorldLines(item, scenePickParts(fixture.state));
    REQUIRE(std::any_of(item.geometry.segments.begin(), item.geometry.segments.end(), [](const auto& s) {
        return s.endTriangle && s.endSource && *s.endSource != s.source;
    }));
    fixture.state.files[0].groupSettings[1].translation[2] += 2;
    const auto after = annotationWorldLines(item, scenePickParts(fixture.state));
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        const auto& segment = item.geometry.segments[i];
        nearPoint(after[i].a, {before[i].a[0], before[i].a[1], before[i].a[2] + (segment.source == 1 ? 2.0f : 0.0f)});
        nearPoint(after[i].b, {before[i].b[0], before[i].b[1], before[i].b[2] + (segment.endSource.value_or(segment.source) == 1 ? 2.0f : 0.0f)});
    }
    fixture.state.files[0].groupSettings[1].visible = false;
    CHECK(annotationWorldLines(item, scenePickParts(fixture.state)).empty());
    const auto hiddenControls = annotationVertices(fixture.state, item);
    REQUIRE(hiddenControls.size() == 2);
    nearPoint(hiddenControls.back(), {.8f,.4f,2.5f});
}
TEST_CASE("group annotations still reject depth jumps and unrelated foreground objects")
{
    Fixture fixture;
    SUBCASE("different sibling layer") { siblingSurface(fixture, 0, .2f); }
    SUBCASE("unrelated object covers the middle") {
        siblingSurface(fixture);
        auto blocker = surface();
        for (auto& vertex : blocker.vertices) { vertex.position[0] *= .1f; vertex.position[2] = .1f; }
        fixture.state.files.push_back(createUiFileState(fixture.root / "blocker.obj", blocker, 1));
        appendDefaultSceneNodesForFiles(fixture.state, 1);
    }
    const auto projection = annotationProjection(scenePickParts(fixture.state), view(), fixture.target(),
        annotationGroupTargets(fixture.state, fixture.target()));
    CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::line, {-.8f,-.4f}, {.8f,.4f}));
    CHECK_THROWS((void)projectAnnotation(projection, AnnotationShape::rectangle, {-.8f,-.4f}, {.8f,.4f}));
}
TEST_CASE("multi source annotation editing save load and undo preserve attachments")
{
    Fixture fixture; siblingSurface(fixture, .15f);
    const auto id = drawSiblingAnnotation(fixture, AnnotationShape::rectangle);
    const auto clean = createSceneDocument(fixture.state);
    SceneHistory history; resetSceneHistory(history, fixture.state);
    const auto original = *findAnnotation(fixture.state, id);
    const auto projection = annotationEditProjection(scenePickParts(fixture.state), original);
    const auto edited = projectAnnotation(projection, AnnotationShape::rectangle, {-.7f,-.3f}, {.7f,.3f});
    reshapeAnnotation(fixture.state, id, edited);
    REQUIRE(recordSceneHistory(history, fixture.state));
    auto undo = prepareSceneHistoryStep(history, fixture.state, clean, false);
    REQUIRE(undo); commitSceneHistoryStep(history, fixture.state, std::move(*undo), false);
    CHECK(fixture.state.annotations[0].geometry == original.geometry);
    CHECK(fixture.state.annotations[0].targetIds == original.targetIds);
    auto redo = prepareSceneHistoryStep(history, fixture.state, clean, true);
    REQUIRE(redo); commitSceneHistoryStep(history, fixture.state, std::move(*redo), true);
    CHECK(fixture.state.annotations[0].geometry == edited);
    std::filesystem::create_directories(fixture.root);
    const auto path = fixture.root / "multi.woby";
    const auto document = createSceneDocument(fixture.state);
    writeSceneDocument(path, document);
    const auto read = readSceneDocument(path);
    CHECK(read.annotations == document.annotations);
    const auto restored = prepareSceneReplacement(fixture.state, fixture.state.files, read);
    REQUIRE(restored.annotations.size() == 1);
    REQUIRE(restored.annotations[0].targetValid);
    REQUIRE(restored.annotations[0].targetIds.size() == 2);
    CHECK(restored.annotations[0].targetIds[1] == restored.files[0].groupSettings[1].objectId);
    const auto before = annotationWorldLines(fixture.state.annotations[0], scenePickParts(fixture.state));
    const auto after = annotationWorldLines(restored.annotations[0], scenePickParts(restored));
    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i) { nearPoint(before[i].a, after[i].a); nearPoint(before[i].b, after[i].b); }
    auto changed = restored;
    changed.files[0].mesh.vertices[4].position[2] += .1f;
    validateAnnotationTargets(changed);
    CHECK_FALSE(changed.annotations[0].targetValid);
    CHECK(annotationWorldLines(changed.annotations[0], scenePickParts(changed)).empty());
}
TEST_CASE("multi source annotation rejects malformed references atomically")
{
    Fixture fixture; siblingSurface(fixture);
    const auto id = drawSiblingAnnotation(fixture);
    const auto original = *findAnnotation(fixture.state, id);
    auto geometry = original.geometry;
    SUBCASE("invalid start source") { geometry.segments.front().source = 99; }
    SUBCASE("invalid end source") { geometry.segments.front().endTriangle = 0; geometry.segments.front().endSource = 99; }
    SUBCASE("invalid secondary triangle") { geometry.segments.back().triangle = 99; }
    SUBCASE("changed secondary attachment") { geometry.sources[1].fingerprint = "changed"; }
    SUBCASE("singular secondary projector") { geometry.sources[1].projector.fill(0); }
    CHECK_THROWS(reshapeAnnotation(fixture.state, id, geometry));
    CHECK(findAnnotation(fixture.state, id)->geometry == original.geometry);
}
TEST_CASE("sibling endpoint handles edit across source parts")
{
    Fixture fixture; siblingSurface(fixture);
    const auto id = drawSiblingAnnotation(fixture);
    AnnotationInteraction interaction;
    REQUIRE(beginAnnotationPointer(fixture.state, interaction, view(), {180,60}));
    REQUIRE(interaction.dragging);
    CHECK(interaction.handle == 1);
    moveAnnotationPointer(fixture.state, interaction, {170,70});
    REQUIRE(interaction.error.empty());
    endAnnotationPointer(fixture.state, interaction, true);
    REQUIRE(interaction.error.empty());
    const auto vertices = annotationVertices(fixture.state, *findAnnotation(fixture.state, id));
    REQUIRE(vertices.size() == 2);
    nearPoint(vertices[1], {.7f,.3f,.5f});
}

TEST_CASE("annotation scope follows a selected parent without combining unrelated selections")
{
    Fixture fixture; siblingSurface(fixture);
    auto left = fixture.state.files[0].mesh;
    auto right = left;
    left.nodes = {{"left",0,6}}; right.nodes = {{"right",6,6}};
    fixture.state = {};
    fixture.state.files.push_back(createUiFileState(fixture.root / "left.obj", left, 0));
    fixture.state.files.push_back(createUiFileState(fixture.root / "right.obj", right, 1));
    appendDefaultSceneNodesForFiles(fixture.state, 0);
    fixture.state.files[1].groupSettings[0].translation = {-2,0,-.3f};
    const auto second = fixture.state.files[1].groupSettings[0].objectId;
    SUBCASE("selected parent includes children from both files") {
        UiSceneNode parent; parent.kind = UiSceneNodeKind::folder; parent.name = "Assembly";
        parent.children = std::move(fixture.state.sceneNodes);
        fixture.state.sceneNodes = {std::move(parent)};
        assignSceneObjectIds(fixture.state);
        selectSceneObject(fixture.state, fixture.state.sceneNodes[0].objectId);
        const auto id = drawSiblingAnnotation(fixture);
        CHECK(findAnnotation(fixture.state, id)->targetIds == std::vector<SceneObjectId>{fixture.target(), second});
        const auto clean = createSceneDocument(fixture.state);
        const auto files = fixture.state.files;
        SceneHistory history; resetSceneHistory(history, fixture.state);
        REQUIRE(removeFileFromState(fixture.state, 1));
        REQUIRE(recordSceneHistory(history, fixture.state));
        CHECK(annotationWorldLines(*findAnnotation(fixture.state, id), scenePickParts(fixture.state)).empty());
        std::filesystem::create_directories(fixture.root);
        const auto path = fixture.root / "missing-sibling.woby";
        const auto missing = createSceneDocument(fixture.state);
        REQUIRE(missing.annotations[0].targets.size() == 2);
        CHECK(missing.annotations[0].targets[1].fileIndex == -1);
        writeSceneDocument(path, missing);
        CHECK(readSceneDocument(path).annotations == missing.annotations);
        auto undo = prepareSceneHistoryStep(history, fixture.state, clean, false, files);
        REQUIRE(undo); commitSceneHistoryStep(history, fixture.state, std::move(*undo), false);
        REQUIRE(fixture.state.annotations[0].targetValid);
        CHECK(fixture.state.annotations[0].targetIds[1] == second);
    }
    SUBCASE("unrelated selected objects cannot share an annotation") {
        fixture.state.selectedSceneObjects = {fixture.target(), second};
        CHECK(annotationGroupTargets(fixture.state, fixture.target()) == std::vector<SceneObjectId>{fixture.target()});
        // Even a caller that bypasses the drawing scope cannot commit foreign sources.
        const std::vector<SceneObjectId> targets{fixture.target(), second};
        const auto projection = annotationProjection(scenePickParts(fixture.state), view(), fixture.target(), targets);
        auto geometry = projectAnnotation(projection, AnnotationShape::line, {-.8f,-.4f}, {.8f,.4f});
        CHECK_THROWS(createAnnotation(fixture.state, fixture.target(), geometry, projection.targetIds));
        CHECK(fixture.state.annotations.empty());
    }
}
TEST_CASE("sampled annotation previews keep sibling bridge source identities")
{
    Fixture fixture; siblingSurface(fixture, .2f);
    const auto projection = annotationProjection(scenePickParts(fixture.state), view(), fixture.target(),
        annotationGroupTargets(fixture.state, fixture.target()));
    UiAnnotation preview;
    preview.targetId = fixture.target(); preview.targetIds = projection.targetIds; preview.targetValid = true;
    preview.geometry = previewAnnotation(projection, AnnotationShape::line, {-.8f,-.4f}, {.8f,.4f});
    const auto lines = annotationWorldLines(preview, scenePickParts(fixture.state));
    REQUIRE_FALSE(lines.empty());
    nearPoint(lines.front().a, {-.8f,-.4f,.5f}); nearPoint(lines.back().b, {.8f,.4f,.5f});
    CHECK(std::any_of(preview.geometry.segments.begin(), preview.geometry.segments.end(), [](const auto& segment) {
        return segment.endSource && segment.source != *segment.endSource;
    }));
}
TEST_CASE("annotation scene loading rejects malformed multi source references")
{
    Fixture fixture; siblingSurface(fixture);
    drawSiblingAnnotation(fixture);
    std::filesystem::create_directories(fixture.root);
    const auto path = fixture.root / "invalid-sources.woby";
    writeSceneDocument(path, createSceneDocument(fixture.state));
    std::ifstream stream(path);
    std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    stream.close();
    SUBCASE("source table references a missing file index") {
        const auto index = text.find("file_index = 0", text.find("[[annotations.sources]]"));
        REQUIRE(index != std::string::npos);
        text.replace(index, std::string("file_index = 0").size(), "file_index = 999");
    }
    SUBCASE("segment references an absent source") {
        const auto index = text.find("source = 0", text.find("[[annotations.segments]]"));
        REQUIRE(index != std::string::npos);
        text.replace(index, std::string("source = 0").size(), "source = 999");
    }
    { std::ofstream output(path); output << text; }
    CHECK_THROWS((void)readSceneDocument(path));
}
TEST_CASE("annotation CLI creates and reshapes across sibling surfaces")
{
    Fixture fixture; siblingSurface(fixture);
    annotationCamera(fixture);
    const auto created = annotationCommand(fixture, "annotation.create",
        {{"shape", "line"}, {"start", {-.2,-.1}}, {"end", {.2,.1}}}, fixture.target());
    REQUIRE(fixture.state.annotations.size() == 1);
    const auto id = fixture.state.annotations[0].objectId;
    REQUIRE(fixture.state.annotations[0].targetIds.size() == 2);
    CHECK(created["object"]["vertexSpace"] == "world");
    CHECK(created["object"]["sourceIds"].size() == 2);
    const auto moved = annotationCommand(fixture, "annotation.move", {{"delta", {0,.05}}}, id);
    CHECK(moved["object"]["targetValid"] == true);
    CHECK(fixture.state.annotations[0].geometry.start[1] == doctest::Approx(-.05));
}

TEST_CASE("annotation render scratch reuses lines and clears hidden or missing sources")
{
    Fixture fixture;
    siblingSurface(fixture, .2f);
    const auto id = drawSiblingAnnotation(fixture);
    auto item = *findAnnotation(fixture.state, id);
    std::vector<ScenePickPart> parts;
    std::vector<DiagnosticEdge> lines;
    std::vector<const ScenePickPart*> sources;
    const auto prepare = [&] {
        scenePickParts(fixture.state, parts);
        annotationWorldLines(item, parts, lines, sources);
    };
    prepare();
    REQUIRE_FALSE(lines.empty());
    REQUIRE(sources.size() == 2);
    const auto original = lines;
    const auto* storage = lines.data();
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto allocations = woby::test::countAllocations([&] {
        for (int frame = 0; frame < 100; ++frame) { prepare(); }
    });
    CHECK(allocations == 0);
#endif
    fixture.state.files[0].groupSettings[1].translation[2] += 2;
    prepare();
    REQUIRE(lines.size() == original.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& segment = item.geometry.segments[i];
        nearPoint(lines[i].a, {original[i].a[0], original[i].a[1], original[i].a[2] + (segment.source == 1 ? 2.0f : 0.0f)});
        nearPoint(lines[i].b, {original[i].b[0], original[i].b[1], original[i].b[2] + (segment.endSource.value_or(segment.source) == 1 ? 2.0f : 0.0f)});
    }
    item.settings.visible = false;
    prepare();
    CHECK(lines.empty());
    CHECK(sources.empty());
    item.settings.visible = true;
    prepare();
    CHECK_FALSE(lines.empty());
    fixture.state.files[0].groupSettings[1].visible = false;
    prepare();
    CHECK(lines.empty());
    CHECK(sources.empty());
    fixture.state.files[0].groupSettings[1].visible = true;
    prepare();
    CHECK_FALSE(lines.empty());
    CHECK(lines.data() == storage);
    item.targetIds[1] = invalidSceneObjectId;
    prepare();
    CHECK(lines.empty());
    CHECK(sources.empty());
}
