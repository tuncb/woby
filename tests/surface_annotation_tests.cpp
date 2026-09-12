#include "surface_annotation.h"
#include "annotation_ui.h"
#include "ui_operations.h"
#include "scene_history.h"
#include "automation_registry.h"
#include "control_scene.h"
#include <nlohmann/json.hpp>

#include <bx/math.h>
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <fstream>
#include <limits>
#include <chrono>
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
ScenePickView view()
{
    ScenePickView result;
    bx::mtxIdentity(result.view.data()); bx::mtxIdentity(result.projection.data());
    result.width = result.height = 200;
    return result;
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
void nearPoint(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    for (size_t k = 0; k < 3; ++k) { CHECK(a[k] == doctest::Approx(b[k]).epsilon(1e-5)); }
}
struct AnnotationUiFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    Fixture scene;
    AnnotationInteraction interaction;
    ScenePickView pickView = view();
    ImVec2 lineButton{}, rectangleButton{}, dimensions{};
    bool pointerAllowed = true, captureMouse = false, disabled = false;
    bool showObjects = false, showInspector = false;
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
            drawAnnotationObjects(scene.state);
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
        drawAnnotationOverlay(scene.state, interaction, pickView, 300, 1 / pickView.pixelScale, pointerAllowed);
        ImGui::EndFrame();
    }
    void click(ImVec2 p, ImGuiMouseButton button = ImGuiMouseButton_Left)
    {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(p.x,p.y); frame();
        io.AddMouseButtonEvent(button,true); frame(); io.AddMouseButtonEvent(button,false); frame();
    }
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
TEST_CASE("annotation Properties retains visibility and comments without a delete button")
{
    AnnotationUiFixture fixture;
    fixture.scene.add(); fixture.showInspector = true; fixture.frame();
    CHECK(fixture.inspectorContents.find("Visible") != std::string::npos);
    CHECK(fixture.inspectorContents.find("Comments") != std::string::npos);
    CHECK(fixture.inspectorContents.find("Delete annotation") == std::string::npos);
}
TEST_CASE("annotation context menu omits Properties and Frame annotation")
{
    AnnotationUiFixture fixture;
    fixture.scene.add(); fixture.showObjects = true; fixture.frame(); fixture.frame();
    fixture.click({100,fixture.removeButton.y}, ImGuiMouseButton_Right);
    fixture.frame();
    CHECK(fixture.objectContents.find("Delete annotation") != std::string::npos);
    CHECK(fixture.objectContents.find("Properties") == std::string::npos);
    CHECK(fixture.objectContents.find("Frame annotation") == std::string::npos);
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
    settings.note += "\nQuoted \"comment\" \\ path\nUTF-8: \xc3\xa9 \xe2\x9c\x93\n" + std::string(6000, 'x');
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
TEST_CASE("surface annotation rejects outlines spanning missing triangles and depth jumps")
{
    Fixture fixture;
    SUBCASE("hole") { fixture.state.files[0].mesh.indices.erase(fixture.state.files[0].mesh.indices.begin() + 6, fixture.state.files[0].mesh.indices.end()); }
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
    nearPoint(moved.front().a, {original.front().a[0]+4, original.front().a[1]+6, original.front().a[2]+8});
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
    REQUIRE(geometry.segments.size() >= 4);
    const auto& mesh = fixture.state.files[0].mesh;
    for (const auto& segment : geometry.segments) {
        for (const auto& bary : {segment.a, segment.b}) {
            const auto local = annotationPosition(mesh, 0, segment.triangle, bary);
            const auto clip = annotationTransform(geometry.projector, {local[0],local[1],local[2],1});
            const float x = clip[0] / clip[3], y = clip[1] / clip[3];
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
