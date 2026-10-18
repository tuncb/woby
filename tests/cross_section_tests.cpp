#include "cross_section.h"
#include "cross_section_runtime.h"
#include "cross_section_ui.h"
#include "scene_queries.h"
#include "scene_history.h"
#include "ui_operations.h"
#include "automation_registry.h"
#include "control_scene.h"
#include "command_line.h"
#include <nlohmann/json.hpp>
#include <doctest/doctest.h>
#include <chrono>
#include <fstream>
#include <limits>
#include <thread>
#include <imgui.h>
#include <imgui_internal.h>

namespace {
using namespace woby;
using Json = nlohmann::json;
ControlOperation sectionCommand(std::vector<std::string> words)
{
    words.insert(words.begin(), {"woby", "ctl", "--instance", "test"});
    std::vector<char*> argv;
    for (auto& word : words) { argv.push_back(word.data()); }
    return parseCommandLine(static_cast<int>(argv.size()), argv.data()).control.operation;
}
Json runSectionCommand(UiState& state, const std::string& method, Json params, SceneObjectId id = 0)
{
    if (id) { params["target"] = std::to_string(id); }
    auto command = parseControlOperation(*findControlMethod(method), params);
    command.objectId = id;
    return applyControlSceneOperation(state, {}, command, [](SceneObjectId value) { return std::to_string(value); }, 200, 800);
}
Mesh sheet(float height)
{
    Mesh mesh;
    auto& geometry = editMeshGeometry(mesh);
    for (const auto p :
         std::array<std::array<float, 3>, 4>{{{-1, -1, height}, {1, -1, height}, {1, 1, height}, {-1, 1, height}}}) {
        Vertex v;
        v.position = p;
        geometry.vertices.push_back(v);
    }
    geometry.indices = {0, 1, 2, 0, 2, 3};
    geometry.nodes = {{"sheet", 0, 6}};
    mesh.bounds = calculateBounds(geometry.vertices);
    publishMeshGeometry(mesh);
    return mesh;
}
struct Fixture {
    std::filesystem::path root =
        std::filesystem::absolute(std::filesystem::temp_directory_path()) / ("woby-section-" + automationRandomHex(8));
    UiState state;
    Fixture()
    {
        std::filesystem::create_directories(root);
        state.files.push_back(createUiFileState(root / "a.obj", sheet(0), 0));
        state.files.push_back(createUiFileState(root / "b.obj", sheet(.25f), 1));
        appendDefaultSceneNodesForFiles(state, 0);
        recalculateSceneBounds(state);
    }
    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::vector<SceneObjectId> sources() const
    {
        return {state.files[0].groupSettings[0].objectId, state.files[1].groupSettings[0].objectId};
    }
    CrossSectionSettings settings() const
    {
        CrossSectionSettings s;
        s.a = {-2, 0, 0};
        s.b = {2, 0, 0};
        s.lift = 3;
        s.showDifferences = true;
        return s;
    }
    SceneObjectId add()
    {
        return createCrossSection(state, settings(), sources());
    }
};
std::shared_ptr<const CrossSectionResult> waitResult(SceneQueryRuntime& runtime, const UiState& state, SceneObjectId id)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        updateSceneAnnotationQueries(runtime, state);
        if (auto result = crossSectionResult(runtime, id)) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return {};
}
} // namespace
TEST_CASE("cross section CLI parses world controls and preserves all options through the protocol")
{
    for (auto words : std::vector<std::vector<std::string>>{
        {"annotation", "create", "scene", "--shape", "section", "--plane-start", "-20", "0", "2", "--plane-end", "20", "0", "2",
         "--lift", "15", "--section-up-axis", "z", "--show-differences", "true", "--name", "Section S"},
        {"annotation", "set", "object", "--plane-start", "-30", "0", "0", "--plane-end", "30", "0", "0", "--lift", "0",
         "--show-plane", "false", "--show-connections", "false", "--show-differences", "false", "--face-on", "true",
         "--source-index", "0", "--source-visible", "false", "--reference-index", "1", "--compared-index", "0"}}) {
        const auto command = sectionCommand(std::move(words));
        const auto params = controlOperationParams(command);
        CHECK(controlOperationParams(parseControlOperation(controlMethod(command.action), params)) == params);
    }
    CHECK_THROWS(sectionCommand({"annotation", "create", "scene", "--shape", "section"}));
    CHECK_THROWS(sectionCommand({"annotation", "create", "object", "--shape", "line"}));
    CHECK_THROWS(sectionCommand({"annotation", "set", "object", "--lift", "-1"}));
    CHECK_THROWS(sectionCommand({"annotation", "set", "object", "--section-up-axis", "x"}));
    CHECK_THROWS(sectionCommand({"annotation", "set", "object", "--source-index", "0"}));
    CHECK_THROWS(sectionCommand({"annotation", "set", "object", "--source-visible", "false"}));
    CHECK_THROWS(sectionCommand({"annotation", "set", "object", "--compared-index", "-1"}));
    CHECK_THROWS(sectionCommand({"annotation", "set", "object", "--plane-start", "0", "0"}));
    CHECK_THROWS(parseControlOperation(controlMethod(ControlAction::annotationSet),
        {{"target", "object"}, {"planeStart", {0, 0, std::numeric_limits<double>::infinity()}}}));
}
TEST_CASE("cross section CLI creates edits persists and deletes one shared annotation")
{
    Fixture f;
    selectSceneObject(f.state, f.sources()[1]);
    const auto selection = f.state.selectedSceneObjects;
    auto result = runSectionCommand(f.state, "annotation.create", {{"target", "scene"}, {"shape", "section"},
        {"planeStart", {-2, 0, 0}}, {"planeEnd", {2, 0, 0}}, {"lift", 3}, {"name", "CLI section"}});
    REQUIRE(f.state.annotations.size() == 1);
    const auto id = f.state.annotations.front().objectId;
    CHECK(f.state.selectedSceneObjects == selection);
    CHECK(result["object"]["shape"] == "section");
    CHECK(result["object"]["sourceIds"].size() == 2);
    CHECK(f.state.annotations.front().targetIds == f.sources());
    result = runSectionCommand(f.state, "annotation.set", {{"planeStart", {-3, .1, 0}}, {"planeEnd", {3, .1, 4}},
        {"lift", 7}, {"showPlane", false}, {"showConnections", false}, {"showDifferences", true}, {"faceOn", true},
        {"sourceIndex", 0}, {"sourceVisible", false}, {"referenceIndex", 1}, {"comparedIndex", 0},
        {"comments", "Review\nsection"}, {"width", 4}, {"opacity", .5}}, id);
    const auto saved = *findAnnotation(f.state, id)->section;
    CHECK(saved.a == std::array<float, 3>{-3, .1f, 0});
    CHECK(saved.b == std::array<float, 3>{3, .1f, 0});
    CHECK(saved.lift == 7);
    CHECK_FALSE(saved.showPlane);
    CHECK_FALSE(saved.showConnections);
    CHECK(saved.showDifferences);
    CHECK(saved.faceOn);
    CHECK(saved.sourceVisible == std::vector<bool>{false, true});
    CHECK(saved.reference == 1);
    CHECK(saved.compared == 0);
    CHECK(result["object"]["settings"]["color"][3] == .5);
    CHECK(runSectionCommand(f.state, "annotation.get", {}, id)["object"] == result["object"]);
    CHECK(runSectionCommand(f.state, "annotation.list", Json::object())["annotations"].size() == 1);
    const auto path = f.root / "cli.woby";
    writeSceneDocument(path, createSceneDocument(f.state));
    const auto restored = prepareSceneReplacement(f.state, f.state.files, readSceneDocument(path));
    REQUIRE(restored.annotations.size() == 1);
    CHECK(restored.annotations.front().section == saved);
    CHECK(restored.annotations.front().settings.note == "Review\nsection");
    runSectionCommand(f.state, "annotation.delete", {}, id);
    CHECK(f.state.annotations.empty());
}
TEST_CASE("cross section CLI validates edits before changing any annotation state")
{
    Fixture f;
    const auto id = f.add();
    const auto before = *findAnnotation(f.state, id)->section;
    const auto name = findAnnotation(f.state, id)->settings.name;
    for (Json params : {Json{{"sourceIndex", 2}, {"sourceVisible", false}}, Json{{"referenceIndex", 2}},
                       Json{{"comparedIndex", 2}}, Json{{"planeStart", {2, 0, 0}}}}) {
        params["name"] = "Invalid edit";
        CHECK_THROWS(runSectionCommand(f.state, "annotation.set", params, id));
        CHECK(findAnnotation(f.state, id)->section == before);
        CHECK(findAnnotation(f.state, id)->settings.name == name);
    }
    runSectionCommand(f.state, "annotation.set", {{"locked", true}}, id);
    CHECK_THROWS(runSectionCommand(f.state, "annotation.set", {{"planeEnd", {2, 1, 0}}, {"name", "Invalid"}}, id));
    CHECK(findAnnotation(f.state, id)->section == before);
    CHECK(findAnnotation(f.state, id)->settings.name == name);
    runSectionCommand(f.state, "annotation.set", {{"lift", 9}}, id);
    CHECK(findAnnotation(f.state, id)->section->lift == 9);
    runSectionCommand(f.state, "annotation.set", {{"locked", false}}, id);
    runSectionCommand(f.state, "annotation.set", {{"planeEnd", {2, 1, 0}}}, id);
    CHECK(findAnnotation(f.state, id)->section->b[1] == 1);
    CHECK_THROWS(runSectionCommand(f.state, "annotation.reshape", {{"end", {.5, .5}}}, id));
    CHECK_THROWS(runSectionCommand(f.state, "annotation.move", {{"delta", {.1, .1}}}, id));
    f.state.annotations.front().section.reset();
    CHECK_THROWS(runSectionCommand(f.state, "annotation.set", {{"lift", 4}}, id));
}
TEST_CASE("cross section CLI captures explicit scopes and defaults to the scene up axis")
{
    Fixture f;
    f.state.upAxis = SceneUpAxis::y;
    for (const auto target : {f.sources()[0], f.state.files[1].objectId}) {
        auto result = runSectionCommand(f.state, "annotation.create", {{"shape", "section"},
            {"planeStart", {-2, 5, 0}}, {"planeEnd", {2, 8, 0}}}, target);
        CHECK(result["object"]["sourceIds"].size() == 1);
        CHECK(result["object"]["upAxis"] == "y");
        CHECK(result["object"]["end"][1] == 5);
    }
    CHECK_THROWS(runSectionCommand(f.state, "annotation.create", {{"shape", "section"},
        {"planeStart", {-2, 0, 0}}, {"planeEnd", {2, 0, 0}}}, f.state.annotations.front().objectId));
    CHECK(f.state.annotations.size() == 2);
    UiState empty;
    CHECK_THROWS(runSectionCommand(empty, "annotation.create", {{"target", "scene"}, {"shape", "section"},
        {"planeStart", {-2, 0, 0}}, {"planeEnd", {2, 0, 0}}}));
    CHECK(empty.annotations.empty());
}
TEST_CASE("cross section CLI captures hidden sources and inherits visibility for scene and explicit scopes")
{
    Fixture f;
    f.state.sceneNodes.clear();
    appendFolderTreeSceneNode(f.state, f.root, 0, f.state.files.size());
    const auto folder = f.state.sceneNodes.front().objectId;
    setGroupVisible(f.state, f.state.files[1], f.state.files[1].groupSettings[0], false);
    const Json points{{"shape", "section"}, {"planeStart", {-2, 0, 0}}, {"planeEnd", {2, 0, 0}}};
    auto scene = points;
    scene["target"] = "scene";
    for (const auto& result : {runSectionCommand(f.state, "annotation.create", scene),
                             runSectionCommand(f.state, "annotation.create", points, folder)}) {
        CHECK(result["object"]["sourceIds"].size() == 2);
        CHECK(result["object"]["sourceVisible"] == Json::array({true, false}));
    }
    CHECK(runSectionCommand(f.state, "annotation.create", points, f.sources()[1])["object"]["sourceVisible"] == Json::array({false}));
    scene["sourceIndex"] = 1;
    scene["sourceVisible"] = true;
    CHECK(runSectionCommand(f.state, "annotation.create", scene)["object"]["sourceVisible"] == Json::array({true, true}));
}
TEST_CASE("cross section creation inherits model eye states through parent folders and files")
{
    Fixture f;
    f.state.files[0].path = f.root / "nested" / "a.obj";
    f.state.sceneNodes.clear();
    appendFolderTreeSceneNode(f.state, f.root, 0, f.state.files.size());
    const auto sources = f.sources();
    const std::vector<SceneObjectId> reversed{sources[1], sources[0]};
    std::vector<bool> expected{true, false};
    SUBCASE("hidden mesh") { f.state.files[0].groupSettings[0].visible = false; }
    SUBCASE("hidden file with a visible mesh") { f.state.files[0].fileSettings.visible = false; }
    SUBCASE("hidden nested folder with visible descendants") { f.state.sceneNodes[0].children[0].settings.visible = false; }
    SUBCASE("hidden root folder") { f.state.sceneNodes[0].settings.visible = false; expected = {false, false}; }
    SUBCASE("implicit scene tree") { f.state.sceneNodes.clear(); f.state.files[0].fileSettings.visible = false; }
    SUBCASE("implicit file children") {
        f.state.sceneNodes[0].children[0].children[0].children.clear();
        f.state.files[0].groupSettings[0].visible = false;
    }
    SUBCASE("opacity and render modes do not change eye states") {
        f.state.files[0].fileSettings.opacity = 0;
        auto& group = f.state.files[0].groupSettings[0];
        group.showSolidMesh = group.showTriangles = group.showVertices = false;
        expected = {true, true};
    }
    const auto id = createCrossSection(f.state, f.settings(), reversed);
    CHECK(findAnnotation(f.state, id)->targetIds == reversed);
    CHECK(findAnnotation(f.state, id)->section->sourceVisible == expected);
}
TEST_CASE("cross section inherited visibility stays independent and survives duplication and save load")
{
    Fixture f;
    setGroupVisible(f.state, f.state.files[1], f.state.files[1].groupSettings[0], false);
    const auto id = f.add();
    REQUIRE(findAnnotation(f.state, id)->section->sourceVisible == std::vector<bool>{true, false});
    setGroupVisible(f.state, f.state.files[0], f.state.files[0].groupSettings[0], false);
    setGroupVisible(f.state, f.state.files[1], f.state.files[1].groupSettings[0], true);
    CHECK(findAnnotation(f.state, id)->section->sourceVisible == std::vector<bool>{true, false});
    const auto copy = duplicateAnnotation(f.state, id);
    CHECK(findAnnotation(f.state, copy)->section->sourceVisible == std::vector<bool>{true, false});
    const auto path = f.root / "inherited-visibility.woby";
    writeSceneDocument(path, createSceneDocument(f.state));
    const auto restored = prepareSceneReplacement(f.state, f.state.files, readSceneDocument(path));
    REQUIRE(restored.annotations.size() == 2);
    for (const auto& item : restored.annotations) {
        CHECK(item.section->sourceVisible == std::vector<bool>{true, false});
    }
    auto settings = *findAnnotation(f.state, id)->section;
    settings.sourceVisible = {true, true};
    setCrossSectionSettings(f.state, id, settings);
    CHECK(findAnnotation(f.state, id)->section->sourceVisible == std::vector<bool>{true, true});
    CHECK_FALSE(f.state.files[0].groupSettings[0].visible);
    CHECK(f.state.files[1].groupSettings[0].visible);
}
TEST_CASE("cross section explicit initial visibility overrides inherited defaults")
{
    Fixture f;
    setGroupVisible(f.state, f.state.files[1], f.state.files[1].groupSettings[0], false);
    auto settings = f.settings();
    settings.sourceVisible = {false};
    auto id = createCrossSection(f.state, settings, f.sources());
    CHECK(findAnnotation(f.state, id)->section->sourceVisible == std::vector<bool>{false, false});
    settings.sourceVisible = {false, true};
    id = createCrossSection(f.state, settings, f.sources());
    CHECK(findAnnotation(f.state, id)->section->sourceVisible == std::vector<bool>{false, true});
}
TEST_CASE("cross section cuts every source in one plane and measures a symmetric 2D gap")
{
    Fixture f;
    const auto parts = scenePickParts(f.state);
    const auto sources = f.sources();
    const auto result = cutCrossSection(f.settings(), sources, parts);
    REQUIRE(result.contours.size() == 2);
    for (const auto& contour : result.contours) {
        REQUIRE(contour.lines.size() == 2);
        for (const auto& line : contour.lines) {
            CHECK(line.a[1] == 0);
            CHECK(line.b[1] == 0);
        }
    }
    REQUIRE(result.largestGap);
    CHECK(result.gap == doctest::Approx(.25));
    auto s = f.settings();
    s.lift = 1000;
    const auto lifted = cutCrossSection(s, sources, parts);
    CHECK(lifted.gap == result.gap);
    CHECK(lifted.contours[0].lines[0].a == result.contours[0].lines[0].a);
    s.a[1] = s.b[1] = 3;
    const auto empty = cutCrossSection(s, sources, parts);
    CHECK(empty.contours[0].lines.empty());
    CHECK(empty.contours[1].lines.empty());
    CHECK_FALSE(empty.largestGap);
}
TEST_CASE("cross section respects model transforms and distinguishes missing sources")
{
    Fixture f;
    f.state.files[1].fileSettings.translation[2] = 2;
    auto parts = scenePickParts(f.state);
    const auto ids = f.sources();
    auto result = cutCrossSection(f.settings(), ids, parts);
    CHECK(result.gap == doctest::Approx(2.25));
    parts.pop_back();
    result = cutCrossSection(f.settings(), ids, parts);
    REQUIRE(result.contours.size() == 2);
    CHECK(result.contours[1].missing);
    CHECK_FALSE(result.largestGap);
}
TEST_CASE("cross sections retain small contours and gaps after nested transforms and large translations")
{
    Fixture f;
    for (auto& file : f.state.files) {
        auto& group = file.groupSettings[0];
        group.center = {.2f,.4f,0}; group.rotationDegrees = {0,0,37}; group.scale = 2;
        file.fileSettings.center = {.3f,-.2f,0}; file.fileSettings.rotationDegrees = {0,0,-19}; file.fileSettings.scale = .5f;
    }
    f.state.sceneNodes.clear();
    appendFolderTreeSceneNode(f.state,f.root,0,f.state.files.size());
    auto& folder = f.state.sceneNodes.front().settings;
    folder.center = {.1f,.2f,0}; folder.rotationDegrees = {0,0,24}; folder.scale = 3;
    const auto baseline = cutCrossSection(f.settings(),f.sources(),scenePickParts(f.state));
    REQUIRE(baseline.largestGap);
    CHECK(baseline.gap == doctest::Approx(.75));
    folder.translation = {1e8f,0,1e8f};
    auto settings = f.settings();
    settings.a = {99999984,0,0}; settings.b = {100000016,0,0};
    const auto moved = cutCrossSection(settings,f.sources(),scenePickParts(f.state));
    REQUIRE(moved.largestGap);
    CHECK(moved.gap == doctest::Approx(baseline.gap));
    const auto id = createCrossSection(f.state,settings,f.sources());
    SceneQueryRuntime runtime;
    const auto published = waitResult(runtime,f.state,id);
    REQUIRE(published);
    CHECK(published->gap == doctest::Approx(moved.gap));
    REQUIRE(published->contours.size() == moved.contours.size());
    CHECK(published->contours[0].minimum == moved.contours[0].minimum);
    CHECK(published->contours[0].maximum == moved.contours[0].maximum);
    REQUIRE(moved.contours.size() == baseline.contours.size());
    for (size_t i = 0; i < moved.contours.size(); ++i) {
        const auto& before = baseline.contours[i]; const auto& after = moved.contours[i];
        REQUIRE_FALSE(before.lines.empty());
        REQUIRE(after.lines.size() == before.lines.size());
        CHECK(after.maximum[0]-after.minimum[0] == doctest::Approx(before.maximum[0]-before.minimum[0]));
        CHECK(after.minimum[1]-1e8 == doctest::Approx(before.minimum[1]).epsilon(1e-6));
        for (size_t j = 0; j < before.lines.size(); ++j) {
            for (size_t axis = 0; axis < 3; ++axis) {
                const double shift = axis == 1 ? 0 : 1e8;
                CHECK(after.lines[j].a[axis]-shift == doctest::Approx(before.lines[j].a[axis]).epsilon(1e-6));
                CHECK(after.lines[j].b[axis]-shift == doctest::Approx(before.lines[j].b[axis]).epsilon(1e-6));
            }
        }
    }
}
TEST_CASE("cross sections retain precise source vertices when render vertices coincide")
{
    auto mesh = sheet(0);
    auto& geometry = editMeshGeometry(mesh);
    for (auto& vertex : geometry.vertices) {
        geometry.precisePositions.push_back({1e8+vertex.position[0],vertex.position[1],vertex.position[2]});
        vertex.position = renderPosition(geometry.precisePositions.back());
    }
    publishMeshGeometry(mesh);
    ScenePickPart part;
    part.mesh = &mesh; part.objectId = 1; part.indexCount = 6; part.model = identityMatrix();
    CrossSectionSettings settings; settings.a = {-2,0,0}; settings.b = {2,0,0};
    SUBCASE("transverse cut") {}
    SUBCASE("coplanar boundary") { settings.upAxis = SceneUpAxis::y; }
    const auto result = cutCrossSection(settings,std::array<SceneObjectId,1>{1},std::span(&part,1));
    REQUIRE(result.contours.size() == 1);
    CHECK(result.contours[0].lines.size() == (settings.upAxis == SceneUpAxis::y ? 4u : 2u));
    CHECK(result.contours[0].maximum[0]-result.contours[0].minimum[0] == doctest::Approx(2));
}
TEST_CASE("cross section keeps coplanar boundaries and removes internal triangle diagonals")
{
    auto mesh = sheet(0);
    ScenePickPart part;
    part.mesh = &mesh;
    part.objectId = 1;
    part.indexCount = 6;
    part.model = identityMatrix();
    CrossSectionSettings s;
    s.a = {-2, 0, 0};
    s.b = {2, 0, 0};
    s.upAxis = SceneUpAxis::y;
    const std::array<SceneObjectId, 1> sources{1};
    const auto result = cutCrossSection(s, sources, std::span(&part, 1));
    REQUIRE(result.contours[0].lines.size() == 4);
    for (const auto& line : result.contours[0].lines) {
        CHECK((line.a[0] == line.b[0] || line.a[1] == line.b[1]));
    }
}
TEST_CASE("cross section ignores collapsed faces in the cutting plane")
{
    auto mesh = sheet(0);
    auto& geometry = editMeshGeometry(mesh);
    geometry.indices = {0, 0, 0, 0, 1, 1};
    publishMeshGeometry(mesh);
    ScenePickPart part;
    part.mesh = &mesh; part.objectId = 1; part.indexCount = 6; part.model = identityMatrix();
    CrossSectionSettings s; s.a = {-2, 0, 0}; s.b = {2, 0, 0}; s.upAxis = SceneUpAxis::y;
    const std::array<SceneObjectId, 1> sources{1};
    CHECK(cutCrossSection(s, sources, std::span(&part, 1)).contours[0].lines.empty());
}
TEST_CASE("cross section through mesh vertices and boundary edges has no zero length segments")
{
    Fixture f;
    auto s = f.settings();
    s.a[1] = s.b[1] = 1;
    auto result = cutCrossSection(s, f.sources(), scenePickParts(f.state));
    REQUIRE(result.contours[0].lines.size() == 1);
    CHECK(result.contours[0].lines[0].a != result.contours[0].lines[0].b);
    s.a = {-2, -2, 0};
    s.b = {2, 2, 0};
    result = cutCrossSection(s, f.sources(), scenePickParts(f.state));
    REQUIRE(result.contours[0].lines.size() == 1);
}
TEST_CASE("cross section validates plane settings at operation boundaries")
{
    Fixture f;
    auto s = f.settings();
    s.b = s.a;
    CHECK_THROWS(createCrossSection(f.state, s, f.sources()));
    CHECK(f.state.annotations.empty());
    s = f.settings();
    s.a[1] = std::numeric_limits<float>::infinity();
    CHECK_THROWS((void)normalizedCrossSectionSettings(s, 2));
    s = f.settings();
    s.lift = -10;
    s.b[2] = 500;
    s.reference = 90;
    s.compared = -1;
    s = normalizedCrossSectionSettings(s, 2);
    CHECK(s.lift == 0);
    CHECK(s.b[2] == s.a[2]);
    CHECK(s.reference == 1);
    CHECK(s.compared == 0);
    CHECK(s.sourceVisible == std::vector<bool>{true, true});
    CHECK_THROWS(createCrossSection(f.state, s, {f.sources()[0], f.sources()[0]}));
}
TEST_CASE("cross section settings and sources survive save load with one temporary root")
{
    Fixture f;
    const auto id = f.add();
    auto s = *findAnnotation(f.state, id)->section;
    s.sourceVisible[1] = false;
    s.showConnections = false;
    s.showPlane = false;
    s.faceOn = true;
    s.reference = 1;
    s.compared = 0;
    setCrossSectionSettings(f.state, id, s);
    const auto path = f.root / "section.woby";
    writeSceneDocument(path, createSceneDocument(f.state));
    const auto doc = readSceneDocument(path);
    REQUIRE(doc.annotations.size() == 1);
    REQUIRE(doc.annotations[0].section);
    CHECK(*doc.annotations[0].section == s);
    CHECK(doc.annotations[0].targets.size() == 2);
    loadSceneAnnotations(f.state, doc);
    REQUIRE(f.state.annotations.size() == 1);
    CHECK(f.state.annotations[0].section == s);
    CHECK(f.state.annotations[0].targetIds == f.sources());
    CHECK(f.state.annotations[0].targetValid);
}
TEST_CASE("cross section edits participate in undo and preserve locked planes")
{
    Fixture f;
    const auto id = f.add();
    SceneHistory history;
    resetSceneHistory(history, f.state);
    const auto clean = createSceneDocument(f.state);
    auto s = *findAnnotation(f.state, id)->section;
    s.lift = 8;
    setCrossSectionSettings(f.state, id, s);
    CHECK(recordSceneHistory(history, f.state));
    auto before = prepareSceneHistoryStep(history, f.state, clean, false);
    REQUIRE(before);
    CHECK(findAnnotation(*before, id)->section->lift == 3);
    auto style = findAnnotation(f.state, id)->settings;
    style.locked = true;
    setAnnotationSettings(f.state, id, style);
    s.a[1] = 1;
    setCrossSectionSettings(f.state, id, s);
    CHECK(findAnnotation(f.state, id)->section->a[1] == 0);
    const auto copy = duplicateAnnotation(f.state, id);
    REQUIRE(copy != id);
    CHECK(findAnnotation(f.state, copy)->section == findAnnotation(f.state, id)->section);
}
TEST_CASE("cross section worker caches geometry across display edits and source visibility")
{
    Fixture f;
    const auto id = f.add();
    SceneQueryRuntime runtime;
    const auto first = waitResult(runtime, f.state, id);
    REQUIRE(first);
    auto s = *findAnnotation(f.state, id)->section;
    s.lift = 6;
    s.sourceVisible[1] = false;
    setCrossSectionSettings(f.state, id, s);
    updateSceneAnnotationQueries(runtime, f.state);
    CHECK(crossSectionResult(runtime, id) == first);
    f.state.files[0].groupSettings[0].visible = false;
    notifySceneEdit(f.state, SceneChange::visibility);
    updateSceneAnnotationQueries(runtime, f.state);
    CHECK(crossSectionResult(runtime, id) == first);
    s.a[1] = s.b[1] = 5;
    setCrossSectionSettings(f.state, id, s);
    const auto moved = waitResult(runtime, f.state, id);
    REQUIRE(moved);
    CHECK(moved != first);
    CHECK(moved->contours[0].lines.empty());
}
TEST_CASE("cross section worker discards stale planes and refreshes transformed sources")
{
    Fixture f;
    const auto id = f.add();
    SceneQueryRuntime runtime;
    updateSceneAnnotationQueries(runtime, f.state);
    auto s = *findAnnotation(f.state, id)->section;
    s.a[1] = s.b[1] = 5;
    setCrossSectionSettings(f.state, id, s);
    auto result = waitResult(runtime, f.state, id);
    REQUIRE(result);
    CHECK(result->contours[0].lines.empty());
    s.a[1] = s.b[1] = 0;
    setCrossSectionSettings(f.state, id, s);
    f.state.files[1].groupSettings[0].translation[2] = 2;
    notifySceneEdit(f.state, SceneChange::geometry);
    result = waitResult(runtime, f.state, id);
    REQUIRE(result);
    CHECK(result->gap == doctest::Approx(2.25));
}
TEST_CASE("cross section projection does not require visible source parts")
{
    Fixture f;
    const auto id = f.add();
    SceneQueryRuntime runtime;
    REQUIRE(waitResult(runtime, f.state, id));
    auto& query = runtime.annotations.objects.at(id);
    const auto view = scenePickView(f.state.camera, f.state.upAxis, f.state.sceneBounds, 800, 600, true, 1);
    CHECK_NOTHROW(updateAnnotationProjection(query, *findAnnotation(f.state, id), {}, view));
    CHECK_FALSE(query.batches.empty());
}
TEST_CASE("cross section pointer creates a shared section and edits endpoints without surface attachment")
{
    Fixture f;
    setGroupVisible(f.state, f.state.files[1], f.state.files[1].groupSettings[0], false);
    ScenePickView view;
    view.view = identityMatrix();
    view.projection = identityMatrix();
    view.renderProjection = identityMatrix();
    view.width = 1000;
    view.height = 1000;
    view.homogeneousDepth = true;
    AnnotationInteraction interaction;
    interaction.sectionTool = true;
    REQUIRE(beginAnnotationPointer(f.state, interaction, view, {100, 500}));
    REQUIRE(interaction.dragging);
    CHECK(interaction.preview.targetIds == f.sources());
    REQUIRE(interaction.preview.section);
    CHECK(interaction.preview.section->sourceVisible == std::vector<bool>{true, false});
    moveAnnotationPointer(f.state, interaction, {900, 500});
    endAnnotationPointer(f.state, interaction, true);
    REQUIRE(f.state.annotations.size() == 1);
    const auto id = f.state.annotations[0].objectId;
    CHECK(f.state.annotations[0].targetIds == f.sources());
    CHECK(f.state.annotations[0].section->sourceVisible == std::vector<bool>{true, false});
    CHECK(f.state.annotations[0].section->a[0] == doctest::Approx(-.8));
    REQUIRE(beginAnnotationPointer(f.state, interaction, view, {900, 500}));
    CHECK(interaction.handle == 1);
    moveAnnotationPointer(f.state, interaction, {800, 400});
    endAnnotationPointer(f.state, interaction, true);
    CHECK(findAnnotation(f.state, id)->section->b[0] == doctest::Approx(.6));
    CHECK(findAnnotation(f.state, id)->section->b[1] == doctest::Approx(.2));
    const auto before = *findAnnotation(f.state, id)->section;
    REQUIRE(beginAnnotationPointer(f.state, interaction, view, {100, 500}));
    moveAnnotationPointer(f.state, interaction, {200, 500});
    endAnnotationPointer(f.state, interaction, false);
    CHECK(*findAnnotation(f.state, id)->section == before);
}
TEST_CASE("cross section inspector displays all sources and the common face on profile")
{
    Fixture f;
    f.state.sceneNodes.clear();
    appendFolderTreeSceneNode(f.state, f.root, 0, f.state.files.size());
    REQUIRE(renameSceneObject(f.state, f.state.sceneNodes[0].objectId, "Section models"));
    const auto sources = f.sources();
    const auto id = createCrossSection(f.state, f.settings(), {sources[1], sources[0]});
    auto s = *findAnnotation(f.state, id)->section;
    s.faceOn = true;
    setCrossSectionSettings(f.state, id, s);
    SceneQueryRuntime queries;
    REQUIRE(waitResult(queries, f.state, id));
    auto* previous = ImGui::GetCurrentContext();
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {640, 1400};
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::string content;
    ImGuiID folderToggle = 0, contourToggle = 0;
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({500, 1350});
        ImGui::Begin("Section properties");
        ImGui::PushID(static_cast<int>(id));
        ImGui::PushID("cut_meshes");
        ImGui::PushID(static_cast<int>(f.state.sceneNodes[0].objectId));
        folderToggle = ImGui::GetID("source");
        ImGui::PushID("source");
        ImGui::PushID(static_cast<int>(f.state.files[0].objectId));
        ImGui::PushID("source");
        ImGui::PushID(static_cast<int>(sources[0]));
        contourToggle = ImGui::GetID("##contour");
        for (int i = 0; i < 7; ++i) { ImGui::PopID(); }
        ImGui::LogToBuffer(0); // Do not force closed tree branches open while logging.
        drawCrossSectionInspector(f.state, &queries);
        content = context->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::EndFrame();
    };
    frame(); frame();
    CHECK(content.find("a.obj / sheet") != std::string::npos);
    CHECK(content.find("b.obj / sheet") != std::string::npos);
    const auto cutContent = [&] {
        const auto start = content.find("Cut meshes");
        CHECK(start != std::string::npos);
        return start == std::string::npos ? std::string{} : content.substr(start, content.find("Display", start) - start);
    };
    const auto cutMeshes = cutContent();
    CHECK(cutMeshes.find("Section models") != std::string::npos);
    CHECK(cutMeshes.find("a.obj") != std::string::npos);
    CHECK(cutMeshes.find("b.obj") != std::string::npos);
    CHECK(cutMeshes.find("sheet | Cut") != std::string::npos);
    CHECK(cutMeshes.find("a.obj / sheet") == std::string::npos);
    CHECK(cutMeshes.find("b.obj / sheet") == std::string::npos);
    CHECK(content.find("Largest sampled 2D gap") != std::string::npos);
    CHECK(content.find("Face-on 2D | equal axis scale") != std::string::npos);
    ImGui::ActivateItemByID(folderToggle); frame();
    CHECK(cutContent().find("Section models") != std::string::npos);
    CHECK(cutContent().find("a.obj") == std::string::npos);
    CHECK(cutContent().find("b.obj") == std::string::npos);
    ImGui::ActivateItemByID(folderToggle); frame();
    CHECK(cutContent().find("sheet | Cut") != std::string::npos);
    ImGui::ActivateItemByID(contourToggle); frame();
    CHECK(findAnnotation(f.state, id)->section->sourceVisible == std::vector<bool>{true, false});
    CHECK(f.state.files[0].groupSettings[0].visible);
    CHECK(findAnnotation(f.state, id)->targetIds == std::vector<SceneObjectId>{sources[1], sources[0]});
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}
TEST_CASE("cross section source tree preserves nested model order and captured source indices")
{
    Fixture f;
    auto mesh = sheet(0);
    auto& geometry = editMeshGeometry(mesh);
    geometry.nodes = {{"first", 0, 6}, {"second", 0, 6}, {"uncaptured", 0, 6}};
    geometry.hierarchy = {{"Assembly", UINT32_MAX, UINT32_MAX}, {"Nested", 0, UINT32_MAX},
        {"second", 1, 1}, {"first", 0, 0}, {"Unused", 0, UINT32_MAX}, {"uncaptured", 4, 2}};
    f.state.files[0] = createUiFileState(f.root / "a.obj", std::move(mesh), 0);
    f.state.sceneNodes.clear();
    appendFolderTreeSceneNode(f.state, f.root, 0, f.state.files.size());
    const auto& groups = f.state.files[0].groupSettings;
    const std::vector<SceneObjectId> sources{groups[0].objectId, groups[1].objectId};
    const auto id = createCrossSection(f.state, f.settings(), sources);
    auto settings = *findAnnotation(f.state, id)->section;
    settings.sourceVisible = {false, true};
    setCrossSectionSettings(f.state, id, settings);
    setSceneNodeSubtreeVisible(f.state, f.state.sceneNodes[0], false);
    REQUIRE(renameSceneObject(f.state, f.state.files[0].objectId, "Model"));
    REQUIRE(renameSceneObject(f.state, sources[1], "Second mesh"));

    CrossSectionSourceTree tree;
    updateCrossSectionSourceTree(tree, f.state, *findAnnotation(f.state, id));
    REQUIRE(tree.roots.size() == 1);
    const auto& folder = tree.roots[0];
    CHECK(folder.objectId == f.state.sceneNodes[0].objectId);
    REQUIRE(folder.children.size() == 1); // The uncaptured file has no branch.
    const auto& file = folder.children[0];
    CHECK(file.name == "Model");
    REQUIRE(file.children.size() == 1);
    const auto& assembly = file.children[0];
    CHECK(assembly.name == "Assembly");
    REQUIRE(assembly.children.size() == 2); // The unused imported folder is omitted.
    const auto& nested = assembly.children[0];
    CHECK(nested.name == "Nested");
    CHECK_FALSE(nested.sourceIndex);
    REQUIRE(nested.children.size() == 1);
    CHECK(nested.children[0].name == "Second mesh");
    CHECK(nested.children[0].objectId == sources[1]);
    CHECK(nested.children[0].sourceIndex == 1);
    CHECK(assembly.children[1].objectId == sources[0]);
    CHECK(assembly.children[1].sourceIndex == 0);
    CHECK(findAnnotation(f.state, id)->targetIds == sources);
    CHECK(findAnnotation(f.state, id)->section->sourceVisible == std::vector<bool>{false, true});
}
TEST_CASE("cross section source tree refreshes labels and removed sources without rebuilding for display edits")
{
    Fixture f;
    const auto sources = f.sources();
    const auto id = f.add();
    CrossSectionSourceTree tree;
    const auto update = [&] { updateCrossSectionSourceTree(tree, f.state, *findAnnotation(f.state, id)); };
    update();
    const auto builds = tree.builds;
    update();
    setGroupVisible(f.state, f.state.files[0], f.state.files[0].groupSettings[0], false);
    auto settings = *findAnnotation(f.state, id)->section;
    settings.sourceVisible[0] = false;
    settings.lift += 1;
    setCrossSectionSettings(f.state, id, settings);
    update();
    CHECK(tree.builds == builds);
    REQUIRE(tree.roots.size() == 2);
    REQUIRE(renameSceneObject(f.state, sources[0], "Renamed mesh"));
    update();
    CHECK(tree.builds == builds + 1);
    REQUIRE(tree.roots[0].children.size() == 1);
    CHECK(tree.roots[0].children[0].name == "Renamed mesh");

    removeFileFromState(f.state, 0);
    update();
    CHECK(tree.builds == builds + 2);
    REQUIRE(tree.roots.size() == 2);
    CHECK(tree.roots[0].name == "b.obj");
    REQUIRE(tree.roots[0].children.size() == 1);
    CHECK(tree.roots[0].children[0].sourceIndex == 1);
    CHECK(tree.roots[1].objectId == sources[0]);
    CHECK(tree.roots[1].sourceIndex == 0);
    CHECK(tree.roots[1].missing);
    CHECK(tree.roots[1].name == "Missing source");
    CHECK(findAnnotation(f.state, id)->targetIds == sources);

    auto replacement = f.state;
    updateCrossSectionSourceTree(tree, replacement, *findAnnotation(replacement, id));
    CHECK(tree.builds == builds + 3);
    ++replacement.sceneGeneration;
    updateCrossSectionSourceTree(tree, replacement, *findAnnotation(replacement, id));
    CHECK(tree.builds == builds + 4);
}
TEST_CASE("cross section source tree handles implicit file trees and changed captures")
{
    Fixture f;
    SUBCASE("implicit scene roots") { f.state.sceneNodes.clear(); }
    SUBCASE("implicit file children") {
        for (auto& node : f.state.sceneNodes) { node.children.clear(); }
    }
    const auto sources = f.sources();
    const auto id = f.add();
    CrossSectionSourceTree tree;
    updateCrossSectionSourceTree(tree, f.state, *findAnnotation(f.state, id));
    REQUIRE(tree.roots.size() == 2);
    for (size_t i = 0; i < tree.roots.size(); ++i) {
        REQUIRE(tree.roots[i].children.size() == 1);
        CHECK(tree.roots[i].children[0].objectId == sources[i]);
        CHECK(tree.roots[i].children[0].sourceIndex == i);
        CHECK_FALSE(tree.roots[i].children[0].missing);
    }
    auto capture = *findAnnotation(f.state, id);
    capture.targetIds = {sources[1]};
    updateCrossSectionSourceTree(tree, f.state, capture);
    REQUIRE(tree.roots.size() == 1);
    CHECK(tree.roots[0].name == "b.obj");
    CHECK(tree.roots[0].children[0].sourceIndex == 0);
    capture.targetIds.clear();
    updateCrossSectionSourceTree(tree, f.state, capture);
    CHECK(tree.roots.empty());
}
TEST_CASE("cross section lift handle changes display only and the line moves the plane")
{
    Fixture f;
    const auto id = f.add();
    ScenePickView view;
    view.width = 800;
    view.height = 600;
    view.homogeneousDepth = true;
    lookAtMatrix(view.view.data(), {0, -5, 3}, {0, 0, 0}, {0, 0, 1});
    orthographicMatrix(view.projection.data(), -5, 5, -5, 5, .1f, 30, 0, true);
    const auto project = [&](std::array<float, 3> p) {
        const auto clip = annotationTransform(annotationCompose(view.view, view.projection), {p[0], p[1], p[2], 1});
        return PickPoint{(clip[0] / clip[3] + 1) * 400, (1 - clip[1] / clip[3]) * 300};
    };
    AnnotationInteraction interaction;
    REQUIRE(beginAnnotationPointer(f.state, interaction, view, project({0, 0, 3})));
    REQUIRE(interaction.handle == 2);
    moveAnnotationPointer(f.state, interaction, project({0, 0, 5}), true);
    endAnnotationPointer(f.state, interaction, true);
    CHECK(findAnnotation(f.state, id)->section->lift == doctest::Approx(5));
    CHECK(findAnnotation(f.state, id)->section->a == f.settings().a);
    REQUIRE(beginAnnotationPointer(f.state, interaction, view, project({0, 0, 0})));
    REQUIRE(interaction.handle == 3);
    moveAnnotationPointer(f.state, interaction, project({0, 1, 0}), true);
    endAnnotationPointer(f.state, interaction, true);
    CHECK(findAnnotation(f.state, id)->section->a[1] == doctest::Approx(1));
    CHECK(findAnnotation(f.state, id)->section->b[1] == doctest::Approx(1));
    CHECK(findAnnotation(f.state, id)->section->lift == doctest::Approx(5));
}

TEST_CASE("cross section direction snapping uses all eight workplane directions with either up axis")
{
    for (auto up : {SceneUpAxis::z, SceneUpAxis::y}) {
        const size_t horizontal = up == SceneUpAxis::z ? 1 : 2;
        const size_t vertical = up == SceneUpAxis::z ? 2 : 1;
        const std::array<float, 3> anchor{7, -3, 11};
        CHECK(snapSectionEndpoint(anchor, anchor, up) == anchor);
        for (const auto direction : std::array<std::array<float, 2>, 8>{{{1,0}, {1,1}, {0,1}, {-1,1},
                 {-1,0}, {-1,-1}, {0,-1}, {1,-1}}}) {
            auto point = anchor;
            point[0] += direction[0] * 4 - direction[1] * .2f;
            point[horizontal] += direction[1] * 4 + direction[0] * .2f;
            point[vertical] += 8;
            const auto snapped = snapSectionEndpoint(anchor, point, up);
            CHECK(snapped[0] == doctest::Approx(anchor[0] + direction[0] * 4));
            CHECK(snapped[horizontal] == doctest::Approx(anchor[horizontal] + direction[1] * 4));
            CHECK(snapped[vertical] == anchor[vertical]);
        }
    }
}

TEST_CASE("cross section Shift toggles the preview at a stationary pointer and commits the snapped plane")
{
    Fixture f;
    ScenePickView view;
    view.view = identityMatrix(); view.projection = identityMatrix(); view.renderProjection = identityMatrix();
    view.width = 1000; view.height = 1000; view.homogeneousDepth = true;
    AnnotationInteraction interaction; interaction.sectionTool = true;
    REQUIRE(beginAnnotationPointer(f.state, interaction, view, {100, 500}));
    moveAnnotationPointer(f.state, interaction, {900, 400});
    REQUIRE(interaction.preview.section);
    CHECK(interaction.preview.section->b[1] == doctest::Approx(.2));
    moveAnnotationPointer(f.state, interaction, {900, 400}, true);
    CHECK(interaction.preview.section->b[1] == 0);
    moveAnnotationPointer(f.state, interaction, {900, 400}, false);
    CHECK(interaction.preview.section->b[1] == doctest::Approx(.2));
    moveAnnotationPointer(f.state, interaction, {900, 400}, true);
    endAnnotationPointer(f.state, interaction, true);
    REQUIRE(f.state.annotations.size() == 1);
    const auto id = f.state.annotations.front().objectId;
    CHECK(findAnnotation(f.state, id)->section->b[1] == 0);

    SUBCASE("moving the end anchors the snapped ray at the start") {
        REQUIRE(beginAnnotationPointer(f.state, interaction, view, {900, 500}));
        REQUIRE(interaction.handle == 1);
        moveAnnotationPointer(f.state, interaction, {800, 100}, true);
        endAnnotationPointer(f.state, interaction, true);
        const auto& s = *findAnnotation(f.state, id)->section;
        CHECK(s.a[0] == doctest::Approx(-.8)); CHECK(s.a[1] == 0);
        CHECK(s.b[0] - s.a[0] == doctest::Approx(s.b[1] - s.a[1]));
    }
    SUBCASE("moving the start anchors the snapped ray at the end") {
        REQUIRE(beginAnnotationPointer(f.state, interaction, view, {100, 500}));
        REQUIRE(interaction.handle == 0);
        moveAnnotationPointer(f.state, interaction, {400, 250}, true);
        endAnnotationPointer(f.state, interaction, true);
        const auto& s = *findAnnotation(f.state, id)->section;
        CHECK(s.b[0] == doctest::Approx(.8)); CHECK(s.b[1] == 0);
        CHECK(s.b[0] - s.a[0] == doctest::Approx(s.a[1] - s.b[1]));
    }
}
