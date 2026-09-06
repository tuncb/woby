#include "control_scene.h"
#include "comparison_scene.h"
#include "command_line.h"
#include "ui_operations.h"
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>

namespace {
using Json = nlohmann::json;
woby::ControlArguments parse(std::vector<std::string> words)
{
    words.insert(words.begin(), {"woby", "ctl", "--instance", "test"});
    std::vector<char*> argv;
    for (auto& word : words) { argv.push_back(word.data()); }
    return woby::parseCommandLine(static_cast<int>(argv.size()), argv.data()).control;
}
woby::UiState scene()
{
    woby::UiState state;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes = {{"triangle", 0, 3}};
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    state.files.push_back(woby::createUiFileState("triangle.obj", mesh, 0));
    woby::appendFolderTreeSceneNode(state, std::filesystem::current_path(), 0, 1);
    woby::recalculateSceneBounds(state);
    woby::frameCameraToScene(state);
    woby::clearSceneDirty(state);
    return state;
}
const woby::ObjectIdFormatter formatId = [](woby::SceneObjectId id) { return std::to_string(id); };
Json run(woby::UiState& state, const woby::SceneDocument& clean, const std::string& method,
    Json params = Json::object(), woby::SceneObjectId id = 0)
{
    if (id) { params["target"] = formatId(id); }
    auto command = woby::parseControlOperation(*woby::findControlMethod(method), params);
    command.objectId = id;
    if (command.a) { command.aId = std::stoull(*command.a); }
    if (command.b) { command.bId = std::stoull(*command.b); }
    if (command.object) { command.memberId = std::stoull(*command.object); }
    return woby::applyControlSceneOperation(state, clean, command, formatId, 200, 800);
}
}

TEST_CASE("ctl parses every extended command family with explicit units and reordered common options")
{
    const auto path = std::filesystem::current_path().string();
    const std::vector<std::vector<std::string>> commands = {
        {"status"}, {"capabilities"}, {"scene", "info"}, {"scene", "tree"}, {"scene", "bounds"},
        {"visibility", "set", "scene", "--visible", "false"},
        {"render", "set", "scene", "--solid", "false", "--triangles", "true", "--vertices", "false"},
        {"transform", "get", "object"}, {"transform", "reset", "object"},
        {"transform", "set", "object", "--translation", "-1", "2", "3", "--rotation-degrees", "90", "0", "0", "--scale", "2"},
        {"opacity", "set", "object", "--value", "0.3"}, {"color", "set", "object", "--rgb", "0.2", "0.3", "0.4"},
        {"color", "reset", "object"}, {"vertex-size", "set", "scene", "--pixels", "10"},
        {"grid", "set", "--visible", "false"}, {"origin", "set", "--visible", "true"}, {"up-axis", "set", "y"},
        {"camera", "get"}, {"camera", "frame"}, {"camera", "orbit", "--yaw-degrees", "30", "--pitch-degrees", "-20"},
        {"camera", "pan", "--right", "2", "--up", "-3"}, {"camera", "roll", "--roll-degrees", "90"},
        {"camera", "dolly", "--factor", "0.5"}, {"camera", "move", "--forward", "1"},
        {"model", "add", path}, {"model", "remove", "object"}, {"folder", "add", path, "--tree"},
        {"importers", "list"}, {"importers", "add", path, "--remember"}, {"importers", "scan", path},
        {"importers", "forget", path}, {"stats"}, {"performance", "get"}, {"pane", "set", "--visible", "false", "--width", "450"},
        {"comparison", "create", "--name", "Repair check", "--a", "source-a", "--b", "source-b"},
        {"comparison", "delete", "object"},
        {"comparison", "set", "object", "--name", "Renamed", "--mode", "overlay", "--visible", "false", "--distance-on-a", "true",
            "--tolerance", "0.1", "--color-range", "1", "--show-edges", "true", "--show-boundaries", "false", "--show-non-manifold", "true"},
        {"comparison", "add", "object", "--side", "a", "--object", "source"},
        {"comparison", "remove", "object", "--side", "b", "--object", "source"},
        {"comparison", "clear", "object", "--side", "a"}, {"comparison", "swap", "object"}, {"comparison", "results", "object"},
    };
    CHECK(commands.size() == woby::controlMethods().size());
    for (auto words : commands) {
        words.insert(words.begin() + 1, {"--json", "--timeout", "12", "--request-key", "test:1", "--wait"});
        CAPTURE(words);
        const auto parsed = parse(words);
        CHECK(parsed.command == woby::ControlCommand::operation);
        CHECK(parsed.json);
        CHECK(parsed.timeoutSeconds == 12);
        CHECK(parsed.requestKey == "test:1");
        const auto params = woby::controlOperationParams(parsed.operation);
        const auto roundtrip = woby::parseControlOperation(woby::controlMethod(parsed.operation.action), params);
        CHECK(woby::controlOperationParams(roundtrip) == params);
    }
    CHECK(parse({"vertex-size", "set", "object", "--scale", "3"}).operation.scale == 3);
}

TEST_CASE("ctl rejects ambiguous incomplete conflicting and nonfinite edit parameters")
{
    for (const auto& words : std::vector<std::vector<std::string>>{
        {"render", "set", "scene"}, {"transform", "set", "object"}, {"pane", "set"},
        {"visibility", "set", "scene", "--visible", "yes"}, {"up-axis", "set", "x"},
        {"opacity", "set", "object", "--value", "nan"}, {"camera", "dolly", "--factor", "0"},
        {"camera", "pan", "--right", "inf"}, {"camera", "move", "--forward", "1e300"},
        {"camera", "orbit", "--yaw-degrees", "20px"}, {"color", "set", "object", "--rgb", "1", "0"},
        {"transform", "set", "object", "--translation", "1", "2", "--scale", "3"},
        {"grid", "set", "--visible", "false", "--visible", "true"},
        {"vertex-size", "set", "scene", "--scale", "2"}, {"vertex-size", "set", "object", "--pixels", "4"},
        {"vertex-size", "set", "scene", "--pixels", "4", "--scale", "2"},
        {"transform", "get", "scene"}, {"status", "--timeout", "2.0"}, {"status", "--timeout", "3601"},
        {"comparison", "set", "object"}, {"comparison", "set", "object", "--mode", "unknown"},
        {"comparison", "set", "object", "--tolerance", "nan"}, {"comparison", "set", "object", "--color-range", "1e100"},
        {"comparison", "set", "object", "--show-edges", "yes"}, {"comparison", "create", "--name", ""},
        {"comparison", "add", "object", "--side", "a"}, {"comparison", "clear", "object", "--side", "c"},
        {"comparison", "delete", "scene"}, {"comparison", "results", "object", "--mode", "a"},
        {"status", "--remember"}, {"stats", "extra"}}) {
        CAPTURE(words);
        CHECK_THROWS(parse(words));
    }
    const auto* opacity = woby::findControlMethod("opacity.set");
    CHECK_THROWS(woby::parseControlOperation(*opacity, {{"target", "object"}, {"value", "0.5"}}));
    CHECK_THROWS(woby::parseControlOperation(*opacity, {{"target", "object"}, {"value", 0.5}, {"unknown", true}}));
    CHECK_THROWS(woby::parseControlOperation(*woby::findControlMethod("model.add"), {{"path", "relative.obj"}}));
}

TEST_CASE("ctl scene edits apply clamps refresh bounds and preserve save mapping")
{
    auto state = scene();
    const auto clean = woby::createSceneDocument(state);
    const auto file = state.files[0].objectId, group = state.files[0].groupSettings[0].objectId, folder = state.sceneNodes[0].objectId;
    run(state, clean, "transform.set", {{"translation", {10, 0, 0}}}, file);
    CHECK(state.sceneBounds.min[0] == doctest::Approx(10));
    CHECK(state.isDirty);
    CHECK(run(state, clean, "opacity.set", {{"value", 4}}, group)["applied"]["opacity"] == 1);
    run(state, clean, "opacity.set", {{"value", 0.3}}, file);
    CHECK(run(state, clean, "transform.reset", {}, file)["applied"]["opacity"] == 1);
    CHECK_FALSE(state.isDirty);
    run(state, clean, "transform.set", {{"scale", 2}, {"rotationDegrees", {0, 0, 20}}}, folder);
    run(state, clean, "opacity.set", {{"value", 0.5}}, folder);
    run(state, clean, "render.set", {{"target", "scene"}, {"solid", false}, {"vertices", false}});
    CHECK_FALSE(state.files[0].groupSettings[0].showSolidMesh);
    CHECK_FALSE(state.files[0].groupSettings[0].showTriangles);
    run(state, clean, "color.set", {{"rgb", {-1, 0.25, 2}}}, group);
    CHECK(state.files[0].groupSettings[0].color == std::array<float, 4>{0, 0.25f, 1, 1});
    run(state, clean, "vertex-size.set", {{"scale", 2}}, file);
    run(state, clean, "vertex-size.set", {{"scale", 3}}, group);
    run(state, clean, "vertex-size.set", {{"target", "scene"}, {"pixels", 10}});
    run(state, clean, "grid.set", {{"visible", false}});
    run(state, clean, "origin.set", {{"visible", false}});
    run(state, clean, "up-axis.set", {{"axis", "y"}});
    const auto saved = woby::createSceneDocument(state);
    auto restoredFiles = state.files;
    for (size_t i = 0; i < restoredFiles.size(); ++i) { woby::applySceneFileRecord(restoredFiles[i], saved.files[i]); }
    const auto restored = woby::prepareSceneReplacement(state, std::move(restoredFiles), saved);
    CHECK(woby::createSceneDocument(restored) == saved);
    CHECK_THROWS(run(state, clean, "color.set", {{"rgb", {1, 0, 0}}}, file));
    CHECK_THROWS(run(state, clean, "vertex-size.set", {{"scale", 2}}, folder));
    CHECK(woby::createSceneDocument(state) == saved);
}

TEST_CASE("ctl visibility refreshes ancestors and object queries distinguish repeated occurrences")
{
    auto state = scene();
    const auto fileId = state.files[0].objectId, groupId = state.files[0].groupSettings[0].objectId;
    auto second = state.sceneNodes[0];
    second.objectId = 0;
    second.settings.translation = {10, 0, 0};
    second.settings.opacity = 0.25f;
    state.sceneNodes.push_back(second);
    woby::assignSceneObjectIds(state);
    const auto clean = woby::createSceneDocument(state);
    run(state, clean, "visibility.set", {{"visible", false}}, fileId);
    CHECK_FALSE(state.files[0].groupSettings[0].visible);
    CHECK_FALSE(state.sceneNodes[0].settings.visible);
    run(state, clean, "visibility.set", {{"visible", true}}, groupId);
    CHECK(state.files[0].fileSettings.visible);
    CHECK(state.sceneNodes[0].settings.visible);
    const auto detail = woby::controlObjectDetails(state, groupId, formatId);
    REQUIRE(detail["occurrences"].size() == 2);
    CHECK(detail["occurrences"][0]["effective"]["opacity"] == 1);
    CHECK(detail["occurrences"][1]["effective"]["opacity"] == 0.25);
    CHECK(detail["occurrences"][0]["effective"]["worldMatrix"] != detail["occurrences"][1]["effective"]["worldMatrix"]);
    CHECK(detail["occurrences"][0]["occurrence"] != detail["occurrences"][1]["occurrence"]);
    state.sceneNodes[1].children.clear();
    CHECK(woby::controlObjectDetails(state, groupId, formatId)["occurrences"].size() == 1);
}

TEST_CASE("ctl camera navigation has explicit units finite results and no persisted changes")
{
    for (auto axis : {woby::SceneUpAxis::y, woby::SceneUpAxis::z}) {
        auto state = scene();
        woby::setSceneUpAxis(state, axis);
        woby::clearSceneDirty(state);
        const auto clean = woby::createSceneDocument(state);
        const auto camera = state.camera;
        run(state, clean, "camera.orbit", {{"yawDegrees", 30}, {"pitchDegrees", 200}});
        CHECK(state.camera.yawRadians - camera.yawRadians == doctest::Approx(0.5235988));
        CHECK(state.camera.pitchRadians == doctest::Approx(1.45));
        run(state, clean, "camera.roll", {{"rollDegrees", 90}});
        CHECK(state.camera.rollRadians == doctest::Approx(1.5707963));
        const auto oldTarget = state.camera.target;
        const auto up = woby::cameraUp(state.camera, axis);
        run(state, clean, "camera.pan", {{"up", 2}});
        CHECK(state.camera.target[0] - oldTarget[0] == doctest::Approx(up.x * 2));
        CHECK(state.camera.target[1] - oldTarget[1] == doctest::Approx(up.y * 2));
        CHECK(state.camera.target[2] - oldTarget[2] == doctest::Approx(up.z * 2));
        run(state, clean, "camera.dolly", {{"factor", 0.5}});
        CHECK(state.camera.distance == doctest::Approx(camera.distance * 0.5));
        const auto beforeBad = woby::controlCameraInfo(state);
        woby::CameraNavigation bad;
        bad.forward = std::numeric_limits<float>::infinity();
        CHECK_THROWS(woby::navigateUiCamera(state, bad));
        CHECK(woby::controlCameraInfo(state) == beforeBad);
        run(state, clean, "pane.set", {{"visible", false}, {"width", 1}});
        CHECK(state.viewerPaneWidth == 200);
        CHECK_FALSE(state.viewerPaneVisible);
        CHECK(woby::createSceneDocument(state) == clean);
        CHECK_FALSE(state.isDirty);
        run(state, clean, "camera.frame");
        CHECK(woby::controlCameraInfo(state)["target"] == state.sceneBounds.center);
    }
}

TEST_CASE("ctl comparison lifecycle expands inputs edits independently and persists all settings")
{
    auto state = scene();
    const auto clean = woby::createSceneDocument(state);
    const auto file = state.files[0].objectId, group = state.files[0].groupSettings[0].objectId, folder = state.sceneNodes[0].objectId;
    const auto created = run(state, clean, "comparison.create", {{"name", "First"}, {"a", formatId(folder)}, {"b", formatId(group)}});
    const auto id = std::stoull(created["target"].get<std::string>());
    CHECK(created["object"]["valid"] == true);
    CHECK(created["object"]["aPartCount"] == 1);
    CHECK(created["object"]["bPartCount"] == 1);
    CHECK(created["object"]["name"] == "First");
    CHECK(state.isDirty);
    const auto second = run(state, clean, "comparison.create");
    const auto secondId = std::stoull(second["target"].get<std::string>());
    CHECK(secondId != id);
    CHECK(second["object"]["valid"] == false);
    run(state, clean, "comparison.add", {{"side", "a"}, {"object", formatId(file)}}, id);
    CHECK(woby::findComparison(state, id)->a.size() == 1); // File/group references deduplicate.
    run(state, clean, "comparison.remove", {{"side", "a"}, {"object", formatId(folder)}}, id);
    CHECK(woby::findComparison(state, id)->a.empty());
    run(state, clean, "comparison.swap", {}, id);
    CHECK(woby::findComparison(state, id)->a.size() == 1);
    CHECK(woby::findComparison(state, id)->b.empty());
    run(state, clean, "comparison.add", {{"side", "b"}, {"object", formatId(group)}}, id);
    const auto edited = run(state, clean, "comparison.set", {{"name", "Measured"}, {"visible", false}, {"mode", "overlay"},
        {"distanceOnA", true}, {"tolerance", 2}, {"colorRange", -1}, {"showEdges", true},
        {"showBoundaries", false}, {"showNonManifold", false}}, id);
    const auto settings = edited["object"]["settings"];
    CHECK(settings["tolerance"] == 2);
    CHECK(settings["colorRange"] == 2);
    CHECK(settings["mode"] == "overlay");
    CHECK(settings["distanceOnA"] == true);
    CHECK(settings["showEdges"] == true);
    CHECK(settings["showBoundaries"] == false);
    CHECK(settings["showNonManifold"] == false);
    CHECK(settings["visible"] == false);
    CHECK(woby::findComparison(state, secondId)->name != "Measured");
    run(state, clean, "comparison.set", {{"mode", "a"}}, id);
    CHECK(woby::comparisonSettings(state, id).mode == woby::ComparisonMode::original);
    run(state, clean, "comparison.set", {{"mode", "b"}}, id);
    CHECK(woby::comparisonSettings(state, id).mode == woby::ComparisonMode::repaired);
    run(state, clean, "comparison.set", {{"mode", "distance"}}, id);
    CHECK(woby::comparisonSettings(state, id).tolerance == 2); // Omitted settings survive.
    const auto saved = woby::createSceneDocument(state);
    const auto restored = woby::prepareSceneReplacement(state, state.files, saved);
    CHECK(woby::createSceneDocument(restored) == saved);
    run(state, clean, "comparison.clear", {{"side", "b"}}, id);
    CHECK(woby::findComparison(state, id)->b.empty());
    CHECK(run(state, clean, "comparison.delete", {}, id)["removed"] == formatId(id));
    CHECK(woby::findComparison(state, id) == nullptr);
    CHECK(state.files.size() == 1);
    CHECK(woby::findComparison(state, secondId) != nullptr);
}

TEST_CASE("ctl comparison invalid inputs never partially mutate the scene")
{
    auto state = scene();
    const auto clean = woby::createSceneDocument(state);
    const auto file = state.files[0].objectId;
    CHECK_THROWS(run(state, clean, "comparison.create", {{"a", formatId(file)}, {"b", "99999"}}));
    CHECK(woby::createSceneDocument(state) == clean);
    CHECK_FALSE(state.isDirty);
    CHECK_THROWS(run(state, clean, "comparison.set", {{"tolerance", 1}}, file));
    const auto created = run(state, clean, "comparison.create");
    const auto id = std::stoull(created["target"].get<std::string>());
    const auto snapshot = woby::createSceneDocument(state);
    CHECK_THROWS(run(state, clean, "comparison.add", {{"side", "a"}, {"object", formatId(id)}}, id));
    CHECK_THROWS(run(state, clean, "comparison.delete", {}, file));
    CHECK(woby::createSceneDocument(state) == snapshot);
    run(state, clean, "comparison.set", {{"tolerance", -1}, {"colorRange", -1}}, id);
    CHECK(woby::comparisonSettings(state, id).tolerance == 0);
    CHECK(woby::comparisonSettings(state, id).colorRange == doctest::Approx(1e-6));
    for (const Json& value : {Json(42), Json(""), Json(std::string(512, 'x')), Json(std::string("bad\0name", 8))}) {
        CHECK_THROWS(woby::parseControlOperation(*woby::findControlMethod("comparison.create"), {{"name", value}}));
    }
    CHECK_THROWS(woby::parseControlOperation(*woby::findControlMethod("comparison.set"), {{"target", "id"}, {"showEdges", 1}}));
    const auto capabilities = woby::controlCapabilities();
    for (const auto& method : capabilities["methods"]) {
        if (method["method"] == "comparison.set") {
            CHECK(method["parameters"]["mode"]["type"] == "string");
            CHECK(method["parameters"]["distanceOnA"]["type"] == "boolean");
        }
    }
}

TEST_CASE("ctl comparison numeric results describe both directions and tolerance without changing geometry")
{
    auto state = scene();
    const auto file = state.files[0].objectId;
    const auto id = woby::createComparison(state);
    woby::setComparisonObjects(state, {file}, woby::ComparisonSide::a, true, id);
    const auto a = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
    auto b = a;
    for (auto& vertex : b.vertices) { vertex.position[2] += 2; }
    const auto result = woby::compareMeshes(a, b);
    const auto json = woby::controlComparisonResults(result, 1);
    for (const auto* direction : {"aToB", "bToA"}) {
        CHECK(json[direction]["maximum"].get<double>() == doctest::Approx(2));
        CHECK(json[direction]["mean"].get<double>() == doctest::Approx(2));
        CHECK(json[direction]["percentile95"].get<double>() == doctest::Approx(2));
        CHECK(json[direction]["percentAboveTolerance"] == 100);
        CHECK(json[direction]["triangleCount"] == 1);
        CHECK(json[direction]["sampleCount"] == 4);
        CHECK(json[direction]["diagnostics"]["boundaryEdges"] == 3);
        CHECK(json[direction]["diagnostics"]["nonManifoldEdges"] == 0);
    }
    CHECK(woby::controlComparisonResults(result, 2)["aToB"]["percentAboveTolerance"] == 0);
    CHECK(a.vertices[0].position[2] == 0);
    CHECK(b.vertices[0].position[2] == 2);
}
