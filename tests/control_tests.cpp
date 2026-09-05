#include "control_scene.h"
#include "command_line.h"
#include "ui_operations.h"
#include <doctest/doctest.h>
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
    CHECK(state.files[0].groupSettings[0].showTriangles);
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
