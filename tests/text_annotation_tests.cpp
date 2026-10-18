#include "text_annotation.h"
#include "text_annotation_ui.h"
#include "annotation_ui.h"
#include "ui_operations.h"
#include "scene_history.h"
#include "control_scene.h"
#include "command_line.h"
#include "automation_registry.h"
#include "math_utils.h"
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <fstream>
#include <limits>

namespace {
using namespace woby;
using Json = nlohmann::json;
struct TextFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / ("woby-text-" + automationRandomHex(8));
    TextFixture() { std::filesystem::create_directories(root); }
    ~TextFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
struct TextUiFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    TextUiFixture() {
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {800, 600}; io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
    }
    ~TextUiFixture() { ImGui::EndFrame(); ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
};
ScenePickView textView(Vec3 eye = {0, 0, 10})
{
    ScenePickView view; view.width = 800; view.height = 600; view.homogeneousDepth = true;
    lookAtMatrix(view.view.data(), eye, {0, 0, 0}, {0, 1, 0});
    perspectiveMatrix(view.projection.data(), 60, 4.0f / 3, .1f, 100, true);
    return view;
}
Json textCommand(UiState& state, const char* method, Json params, SceneObjectId id = 0)
{
    if (id) { params["target"] = std::to_string(id); }
    auto command = parseControlOperation(*findControlMethod(method), params); command.objectId = id;
    return applyControlSceneOperation(state, {}, command, [](SceneObjectId value) { return std::to_string(value); }, 800, 600);
}
}

TEST_CASE("text annotations validate edits and retain independent scene state")
{
    UiState state;
    TextAnnotationSettings text; text.text = "Review\n\xc3\xa9"; text.position = {1, 2, 3}; text.size = .2f;
    const auto id = createTextAnnotation(state, text);
    REQUIRE(findAnnotation(state, id)); CHECK(findAnnotation(state, id)->targetValid);
    CHECK(state.selectedSceneObjects == std::vector<SceneObjectId>{id});
    CHECK_FALSE(setSelectedAnnotationStyle(state, {}, 4.0f));
    auto changed = text; changed.text = "Updated"; changed.faceCamera = false; changed.rotationDegrees = {450, 0, 0};
    setTextAnnotationSettings(state, id, changed);
    CHECK(findAnnotation(state, id)->text->rotationDegrees[0] == 90);
    auto style = findAnnotation(state, id)->settings; style.locked = true; style.color = {.2f, .4f, .6f, .7f};
    setAnnotationSettings(state, id, style);
    changed = *findAnnotation(state, id)->text; changed.position[0] = 100;
    setTextAnnotationSettings(state, id, changed);
    CHECK(findAnnotation(state, id)->text->position == text.position);
    const auto copy = duplicateAnnotation(state, id);
    CHECK(findAnnotation(state, copy)->text == findAnnotation(state, id)->text);
    validateAnnotationTargets(state);
    CHECK(findAnnotation(state, copy)->targetValid);
    deleteAnnotation(state, id); CHECK(state.annotations.size() == 1);
    for (const auto invalid : {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        changed.size = invalid; CHECK_THROWS(createTextAnnotation(state, changed));
    }
    changed = text; changed.position[1] = std::numeric_limits<float>::infinity(); CHECK_THROWS(createTextAnnotation(state, changed));
    changed = text; changed.text = std::string(8193, 'a'); CHECK_THROWS(createTextAnnotation(state, changed));
    changed.text = std::string("a\0b", 3); CHECK_THROWS(createTextAnnotation(state, changed));
    CHECK(state.annotations.size() == 1);
}

TEST_CASE("text annotations preserve text style placement and facing through save load and undo")
{
    TextFixture fixture;
    UiState state; SceneHistory history; resetSceneHistory(history, state);
    TextAnnotationSettings text; text.text = "Valve \"A\"\nLine 2 \\ \xc3\xa9"; text.size = 3.5f;
    text.position = {2, 3, 4}; text.rotationDegrees = {30, 45, 60}; text.faceCamera = false;
    const auto id = createTextAnnotation(state, text);
    auto style = findAnnotation(state, id)->settings; style.color = {.2f, .4f, .6f, .8f}; style.visible = false;
    setAnnotationSettings(state, id, style);
    recordSceneHistory(history, state);
    const auto document = createSceneDocument(state);
    const auto path = fixture.root / "labels.woby"; writeSceneDocument(path, document);
    const auto read = readSceneDocument(path);
    CHECK(read.annotations == document.annotations);
    UiState restored; loadSceneAnnotations(restored, read);
    REQUIRE(restored.annotations.size() == 1);
    CHECK(restored.annotations[0].text == text); CHECK(restored.annotations[0].settings == style);
    CHECK(restored.annotations[0].targetValid);
    auto prepared = prepareSceneHistoryStep(history, state, {}, false);
    REQUIRE(prepared); commitSceneHistoryStep(history, state, std::move(*prepared), false);
    CHECK(state.annotations.empty());
    prepared = prepareSceneHistoryStep(history, state, {}, true);
    REQUIRE(prepared); commitSceneHistoryStep(history, state, std::move(*prepared), true);
    REQUIRE(state.annotations.size() == 1); CHECK(state.annotations[0].text == text);
    CHECK(state.annotations[0].settings == style); CHECK(state.annotations[0].targetValid);
    { std::ofstream out(path); out << "version = 26\n[[annotations]]\nshape = \"text\"\ntext_size = -1\n"; }
    CHECK_THROWS((void)readSceneDocument(path));
}

TEST_CASE("text annotation CLI creates reads edits and deletes text with validated options")
{
    UiState state;
    const auto selected = createTextAnnotation(state, {});
    const auto created = textCommand(state, "annotation.create", {{"target", "scene"}, {"shape", "text"},
        {"text", "Check this"}, {"position", {1, 2, 3}}, {"size", 2}, {"faceCamera", false}, {"rgb", {.2, .3, .4}}});
    const auto id = std::stoull(created["target"].get<std::string>());
    CHECK(state.selectedSceneObjects == std::vector<SceneObjectId>{selected});
    CHECK(created["object"]["shape"] == "text"); CHECK(created["object"]["size"] == 2);
    CHECK(created["object"]["faceCamera"] == false);
    CHECK(textCommand(state, "annotation.get", {}, id)["object"] == created["object"]);
    const auto result = textCommand(state, "annotation.set", {{"text", "Line 1\nLine 2"}, {"size", 5}, {"position", {4, 5, 6}},
        {"faceCamera", true}, {"rotationDegrees", {0, 90, 0}}, {"rgb", {.9, .8, .7}}}, id);
    CHECK(result["object"]["text"] == "Line 1\nLine 2"); CHECK(result["object"]["size"] == 5);
    CHECK(result["object"]["faceCamera"] == true); CHECK(result["object"]["position"] == Json::array({4, 5, 6}));
    textCommand(state, "annotation.set", {{"locked", true}}, id);
    CHECK_THROWS(textCommand(state, "annotation.set", {{"position", {0, 0, 0}}}, id));
    CHECK_THROWS(textCommand(state, "annotation.move", {{"delta", {0, 0}}}, id));
    const auto before = createSceneDocument(state);
    CHECK_THROWS(textCommand(state, "annotation.set", {{"text", "Bad"}, {"lift", 5}}, id));
    CHECK(sceneContentEqual(before, createSceneDocument(state)));
    textCommand(state, "annotation.set", {{"text", ""}}, id);
    CHECK_FALSE(textCommand(state, "annotation.get", {}, id)["object"]["effectiveVisible"].get<bool>());
    CHECK(textCommand(state, "annotation.list", Json::object())["annotations"].size() == 2);
    textCommand(state, "annotation.delete", {}, id); CHECK(state.annotations.size() == 1);
    for (auto params : {Json{{"target", "scene"}, {"shape", "text"}, {"position", {0, 0, 0}}},
        Json{{"target", "scene"}, {"shape", "text"}, {"text", "No position"}},
        Json{{"target", "scene"}, {"shape", "text"}, {"text", "Bad size"}, {"position", {0, 0, 0}}, {"size", 0}},
        Json{{"target", "scene"}, {"shape", "text"}, {"text", "Bad option"}, {"position", {0, 0, 0}}, {"start", {0, 0}}}}) {
        CHECK_THROWS(textCommand(state, "annotation.create", params));
    }
    std::vector<std::string> args{"woby", "ctl", "--instance", "test", "annotation", "create", "scene", "--shape", "text",
        "--text", "From CLI", "--position", "1", "2", "3", "--size", "2", "--face-camera", "false", "--rgb", "1", "0", "0"};
    std::vector<char*> argv; for (auto& arg : args) { argv.push_back(arg.data()); }
    const auto operation = parseCommandLine(static_cast<int>(argv.size()), argv.data()).control.operation;
    CHECK(operation.text == "From CLI"); CHECK(operation.faceCamera == false);
    const auto parsed = parseControlOperation(controlMethod(ControlAction::annotationCreate), controlOperationParams(operation));
    CHECK(parsed.position == operation.position); CHECK(parsed.text == operation.text); CHECK(parsed.faceCamera == false);
}

TEST_CASE("text facing mode stays readable while fixed text follows the scene plane")
{
    TextAnnotationSettings text; text.size = 1;
    for (const auto eye : {Vec3{0, 0, 10}, Vec3{10, 0, 0}, Vec3{0, 0, -10}, Vec3{-5, 4, 3}}) {
        const auto view = textView(eye);
        const auto transform = textAnnotationTransform(text, view);
        const auto a = projectTextAnnotationPoint(transform, view, {0, 0});
        const auto b = projectTextAnnotationPoint(transform, view, {1, 0});
        const auto c = projectTextAnnotationPoint(transform, view, {0, 1});
        REQUIRE(a); REQUIRE(b); REQUIRE(c);
        CHECK((*b)[0] > (*a)[0]); CHECK((*b)[1] == doctest::Approx((*a)[1]));
        CHECK((*c)[1] > (*a)[1]); CHECK((*c)[0] == doctest::Approx((*a)[0]));
    }
    text.faceCamera = false;
    const auto view = textView({10, 0, 0});
    const auto transform = textAnnotationTransform(text, view);
    REQUIRE(projectTextAnnotationPoint(transform, view, {1, 0}));
    CHECK((*projectTextAnnotationPoint(transform, view, {1, 0}))[0] == doctest::Approx(400));
    text.position = {20, 0, 0};
    CHECK_FALSE(projectTextAnnotationPoint(textAnnotationTransform(text, view), view, {0, 0}));
}

TEST_CASE("text placement uses the hit triangle and otherwise the anchor depth")
{
    Mesh mesh; auto& geometry = editMeshGeometry(mesh);
    for (const auto p : {std::array<float, 3>{-3, -3, 0}, {3, -3, 0}, {0, 3, 0}}) { Vertex v; v.position = p; geometry.vertices.push_back(v); }
    geometry.indices = {0, 1, 2}; geometry.nodes = {{"triangle", 0, 3}}; publishMeshGeometry(mesh);
    ScenePickPart part; part.mesh = &mesh; part.objectId = 1; part.indexCount = 3; part.solid = true;
    part.model = identityMatrix(); part.model[14] = 2;
    const auto view = textView();
    const auto hit = textAnnotationPlacement(std::span(&part, 1), view, {400, 300}, {0, 0, 0});
    REQUIRE(hit); CHECK((*hit)[2] == doctest::Approx(2));
    const auto empty = textAnnotationPlacement({}, view, {450, 320}, {0, 0, 0});
    REQUIRE(empty); CHECK((*empty)[2] == doctest::Approx(0).epsilon(.001)); CHECK((*empty)[0] < 0);
}

TEST_CASE("text pointer creates moves cancels and renders colored glyphs")
{
    TextUiFixture ui;
    UiState state; AnnotationInteraction interaction; interaction.textTool = true;
    const auto view = textView();
    REQUIRE(beginAnnotationPointer(state, interaction, view, {400, 300}));
    endAnnotationPointer(state, interaction, true);
    REQUIRE(state.annotations.size() == 1); REQUIRE(state.annotations[0].text);
    const auto id = state.annotations[0].objectId;
    const auto before = *state.annotations[0].text;
    REQUIRE(beginAnnotationPointer(state, interaction, view, {400, 300}));
    moveAnnotationPointer(state, interaction, {450, 330}); endAnnotationPointer(state, interaction, false);
    CHECK(*state.annotations[0].text == before);
    REQUIRE(beginAnnotationPointer(state, interaction, view, {400, 300}));
    moveAnnotationPointer(state, interaction, {450, 330}); endAnnotationPointer(state, interaction, true);
    CHECK(state.annotations[0].text->position != before.position);
    auto style = state.annotations[0].settings; style.color = {1, 0, 0, 1}; setAnnotationSettings(state, id, style);
    ImDrawList draw(ImGui::GetDrawListSharedData()); draw._ResetForNewFrame();
    draw.PushClipRect({0, 0}, {800, 600}); draw.PushTexture(ImGui::GetIO().Fonts->TexRef);
    drawTextAnnotations(draw, state, view, {0, 0}, 1, interaction.textDrawCache);
    CHECK(draw.VtxBuffer.Size > 0);
    const auto builds = interaction.textDrawCache.builds;
    drawTextAnnotations(draw, state, textView({10, 0, 0}), {0, 0}, 1, interaction.textDrawCache);
    CHECK(interaction.textDrawCache.builds == builds);
    for (const auto& vertex : draw.VtxBuffer) { CHECK(vertex.col == IM_COL32(255, 0, 0, 255)); }
    const int count = draw.VtxBuffer.Size;
    style.visible = false; setAnnotationSettings(state, id, style);
    drawTextAnnotations(draw, state, view, {0, 0}, 1, interaction.textDrawCache); CHECK(draw.VtxBuffer.Size == count);
    draw.PopTexture(); draw.PopClipRect();
    interaction.textTool = true;
    REQUIRE(beginAnnotationPointer(state, interaction, view, {300, 300}));
    cancelAnnotationPointer(interaction); CHECK(state.annotations.size() == 1);
}
