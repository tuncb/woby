#include "ui_operations.h"
#include "scene_history.h"
#include "scene_inspector.h"
#include "scene_draw_plan.h"
#include "automation_registry.h"
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <fstream>

namespace {
using namespace woby;
struct NameFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-names-" + automationRandomHex(8));
    UiState state;
    NameFixture() {
        std::filesystem::create_directories(root);
        Mesh mesh;
        editMeshGeometry(mesh).vertices = {{{0,0,0}, {}, {}}, {{1,0,0}, {}, {}}, {{0,1,0}, {}, {}}};
        editMeshGeometry(mesh).indices = {0,1,2,0,1,2};
        editMeshGeometry(mesh).nodes = {{"source-1",0,3}, {"source-2",3,3}};
        editMeshGeometry(mesh).nodes[0].displayName = "solid_007";
        editMeshGeometry(mesh).nodes[1].displayName = "rotor_002";
        editMeshGeometry(mesh).hierarchy = {{"Imported assembly",UINT32_MAX,UINT32_MAX},
            {"first",0,0}, {"second",0,1}};
        mesh.bounds = calculateBounds(meshGeometry(mesh).vertices);
        state.files.push_back(createUiFileState(root / "pump.obj", std::move(mesh), 0, "fixture"));
        appendDefaultSceneNodesForFiles(state, 0);
        clearSceneDirty(state);
    }
    ~NameFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    SceneObjectId model() const { return state.files[0].objectId; }
    SceneObjectId part(size_t i = 0) const { return state.files[0].groupSettings[i].objectId; }
    SceneObjectId folder() const { return state.sceneNodes[0].children[0].objectId; }
    std::string name(SceneObjectId id) const { return findSceneObject(state, id)->name; }
};

struct NameInputFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    SceneNameEdit edit;
    SceneNameInputResult result = SceneNameInputResult::editing;
    ImVec2 outside;
    NameInputFixture() {
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {800,600}; io.DeltaTime = 1.0f/60.0f;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        beginSceneNameEdit(edit, {1,SceneObjectKind::group,"Housing",{},0,"solid_007"}, 2);
        frame(); frame(); frame();
    }
    ~NameInputFixture() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
    void frame() {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({20,20}); ImGui::SetNextWindowSize({500,300});
        ImGui::Begin("Name input");
        const auto next = drawSceneNameInput("##name", edit, 300);
        if (next != SceneNameInputResult::editing) { result = next; }
        drawSceneNameError(edit);
        ImGui::Button("Outside"); const auto low=ImGui::GetItemRectMin(), high=ImGui::GetItemRectMax();
        outside={(low.x+high.x)*.5f,(low.y+high.y)*.5f};
        ImGui::End(); ImGui::EndFrame();
    }
    void key(ImGuiKey value) {
        ImGui::GetIO().AddKeyEvent(value,true); frame();
        ImGui::GetIO().AddKeyEvent(value,false); frame(); frame();
    }
    void type(const char* text) { ImGui::GetIO().AddInputCharactersUTF8(text); frame(); }
    void clickOutside() {
        auto& io=ImGui::GetIO(); io.AddMousePosEvent(outside.x,outside.y); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left,true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left,false); frame(); frame();
    }
};
} // namespace

TEST_CASE("source names preserve identity and reset only the named object") {
    NameFixture f;
    const auto source = meshGeometry(f.state.files[0].mesh).nodes;
    const auto path = f.state.files[0].path;
    const auto geometry = f.state.revisions.geometry, analysis = f.state.revisions.analysis;
    const auto labels = f.state.revisions.labels, revision = f.state.sceneEditRevision;
    selectSceneObject(f.state, f.part());
    REQUIRE(renameSceneObject(f.state, f.model(), "  Pump assembly  "));
    REQUIRE(renameSceneObject(f.state, f.part(), "Housing"));
    REQUIRE(renameSceneObject(f.state, f.folder(), "Body"));
    CHECK(f.name(f.model()) == "Pump assembly"); CHECK(f.name(f.part()) == "Housing");
    CHECK(f.name(f.folder()) == "Body");
    CHECK(f.state.isDirty); CHECK(f.state.sceneEditRevision == revision + 3);
    CHECK(f.state.revisions.labels == labels + 3);
    CHECK(f.state.revisions.geometry == geometry); CHECK(f.state.revisions.analysis == analysis);
    CHECK(f.state.files[0].path == path);
    CHECK(meshGeometry(f.state.files[0].mesh).nodes[0].name == source[0].name);
    CHECK(meshGeometry(f.state.files[0].mesh).nodes[0].displayName == source[0].displayName);
    CHECK(f.state.selectedSceneObjects == std::vector<SceneObjectId>{f.part()});
    CHECK(sceneNodeDisplayName(f.state, f.state.sceneNodes[0]) == "Pump assembly");
    CHECK(sceneNodeDisplayName(f.state, f.state.sceneNodes[0].children[0]) == "Body");
    CHECK(sceneNodeDisplayName(f.state, f.state.sceneNodes[0].children[0].children[0]) == "Housing");
    REQUIRE(restoreSceneObjectName(f.state, f.model()));
    CHECK(f.name(f.model()) == "pump.obj"); CHECK(f.name(f.part()) == "Housing"); CHECK(f.name(f.folder()) == "Body");
    REQUIRE(restoreSceneObjectName(f.state, f.folder())); CHECK(f.name(f.folder()) == "Imported assembly");
    REQUIRE(renameSceneObject(f.state, f.part(), "solid_007"));
    CHECK(f.state.files[0].groupSettings[0].customName.empty());
    CHECK(f.name(f.part()) == "solid_007");
}

TEST_CASE("source name validation handles blank duplicate unicode and stale edits") {
    NameFixture f;
    const auto revision = f.state.sceneEditRevision;
    for (const auto name : {"", "  \t\r\n", "two\nlines", "bad\tname"}) {
        CHECK_FALSE(renameSceneObject(f.state, f.part(), name));
    }
    CHECK_FALSE(renameSceneObject(f.state, 0, "Missing"));
    CHECK_FALSE(renameSceneObject(f.state, 999999, "Missing"));
    CHECK_FALSE(restoreSceneObjectName(f.state, f.part()));
    CHECK_FALSE(renameSceneObject(f.state, f.part(), "solid_007"));
    CHECK_FALSE(f.state.isDirty); CHECK(f.state.sceneEditRevision == revision);
    const std::string unicode = "Geh\xc3\xa4use \xe6\xb3\xb5";
    REQUIRE(renameSceneObject(f.state, f.part(), unicode));
    REQUIRE(renameSceneObject(f.state, f.part(1), unicode));
    CHECK(f.name(f.part()) == f.name(f.part(1)));
    const auto duplicateRevision = f.state.sceneEditRevision;
    CHECK_FALSE(renameSceneObject(f.state, f.part(), "  " + unicode + "  "));
    CHECK(f.state.sceneEditRevision == duplicateRevision);
    REQUIRE(renameSceneObject(f.state, f.part(), std::string(2048, 'x')));
    CHECK(f.name(f.part()).size() == 2048);
    const auto comparison = createComparison(f.state);
    CHECK_FALSE(renameSceneObject(f.state, comparison, "Unsupported"));
    CHECK_FALSE(restoreSceneObjectName(f.state, comparison));
}

TEST_CASE("source names refresh references and labels without invalidating analysis geometry") {
    NameFixture f;
    const auto comparison = createAnalysisFromObjects(f.state, AnalysisTask::meshChecks, {f.model()});
    UiAnnotation annotation; annotation.targetId = f.part(); annotation.targetName = "pump.obj / solid_007";
    f.state.annotations.push_back(annotation);
    ComparisonInspectorCache cache; updateComparisonInspectorCache(cache, f.state, comparison);
    const auto signature = cache.signature, signatureBuilds = cache.signatureBuilds;
    SceneDrawCache draw; REQUIRE(updateSceneDrawPlan(draw, f.state));
    REQUIRE(renameSceneObject(f.state, f.part(), "Housing"));
    REQUIRE(renameSceneObject(f.state, f.model(), "Pump assembly"));
    updateComparisonInspectorCache(cache, f.state, comparison);
    CHECK(cache.signature == signature); CHECK(cache.signatureBuilds == signatureBuilds);
    CHECK(cache.inputs[0].summary.sourceNames == "Pump assembly");
    CHECK(cache.inputs[0].roots[0].name == "Pump assembly");
    CHECK(findComparison(f.state, comparison)->a[0].name == "Pump assembly / Housing");
    CHECK(f.state.annotations[0].targetName == "Pump assembly / Housing");
    CHECK_FALSE(updateSceneDrawPlan(draw, f.state));
    REQUIRE(restoreSceneObjectName(f.state, f.part()));
    CHECK(findComparison(f.state, comparison)->a[0].name == "Pump assembly / solid_007");
    CHECK(f.state.annotations[0].targetName == "Pump assembly / solid_007");
}

TEST_CASE("source custom names survive scene save load and retain imported lookup names") {
    NameFixture f;
    REQUIRE(renameSceneObject(f.state, f.model(), "Pump \"A\""));
    REQUIRE(renameSceneObject(f.state, f.folder(), "Body"));
    REQUIRE(renameSceneObject(f.state, f.part(), "Housing"));
    const auto document = createSceneDocument(f.state);
    const auto path = f.root / "names.woby";
    writeSceneDocument(path, document);
    auto saved = readSceneDocument(path);
    REQUIRE(saved.files.size() == 1);
    CHECK(saved.files[0].customName == "Pump \"A\"");
    CHECK(saved.files[0].groups[0].name == "source-1");
    CHECK(saved.files[0].groups[0].customName == "Housing");
    saved.files[0].path = sceneAbsolutePath(path, saved.files[0].path);
    CHECK(saved == document);
    auto files = f.state.files;
    files[0].customName.clear(); for(auto& group : files[0].groupSettings) { group.customName.clear(); }
    applySceneFileRecord(files[0], saved.files[0]);
    auto restored = prepareSceneReplacement({}, std::move(files), saved);
    CHECK(modelDisplayName(restored.files[0]) == "Pump \"A\"");
    CHECK(groupDisplayName(restored.files[0], 0) == "Housing");
    const auto folder = restored.sceneNodes[0].children[0].objectId;
    CHECK(findSceneObject(restored, folder)->name == "Body");
    CHECK(findSceneObject(restored, folder)->originalName == "Imported assembly");
    CHECK(findSceneObject(restored, restored.files[0].groupSettings[0].objectId)->originalName == "solid_007");
    REQUIRE(restoreSceneObjectName(restored, restored.files[0].objectId));
    REQUIRE(restoreSceneObjectName(restored, restored.files[0].groupSettings[0].objectId));
    REQUIRE(restoreSceneObjectName(restored, folder));
    CHECK(modelDisplayName(restored.files[0]) == "pump.obj"); CHECK(groupDisplayName(restored.files[0], 0) == "solid_007");
    CHECK(findSceneObject(restored, folder)->name == "Imported assembly");
}

TEST_CASE("source name load defaults validate new fields and old scenes") {
    NameFixture f;
    const auto path = f.root / "legacy.woby";
    auto document = createSceneDocument(f.state);
    writeSceneDocument(path, document);
    std::string text;
    { std::ifstream input(path); text.assign(std::istreambuf_iterator<char>(input), {}); }
    text.replace(text.find("version = 26"), 12, "version = 24");
    { std::ofstream output(path); output << text; }
    const auto legacy = readSceneDocument(path);
    CHECK(legacy.files[0].customName.empty()); CHECK(legacy.files[0].groups[0].customName.empty());
    REQUIRE(renameSceneObject(f.state, f.model(), "Changed"));
    REQUIRE(renameSceneObject(f.state, f.part(), "Changed"));
    applySceneFileRecord(f.state.files[0], legacy.files[0]);
    CHECK(f.name(f.model()) == "pump.obj"); CHECK(f.name(f.part()) == "solid_007");
    document.files[0].customName = "  Renamed  "; document.files[0].groups[0].customName = " \t\n ";
    document.files[0].groups[1].customName = "rotor_002";
    applySceneFileRecord(f.state.files[0], document.files[0]);
    CHECK(f.name(f.model()) == "Renamed"); CHECK(f.state.files[0].groupSettings[0].customName.empty());
    CHECK(f.state.files[0].groupSettings[1].customName.empty());
    for (const auto section : {"[[files]]\npath = \"pump.obj\"\n", "[[files]]\npath = \"pump.obj\"\n[[files.groups]]\n",
            "[[nodes]]\nkind = \"folder\"\n"}) {
        { std::ofstream output(path); output << "version = 26\n" << section << "custom_name = true\n"; }
        CHECK_THROWS((void)readSceneDocument(path));
    }
}

TEST_CASE("source rename and reset have separate undo steps and views do not change names") {
    NameFixture f;
    const auto view = createView(f.state);
    const auto clean = createSceneDocument(f.state);
    SceneHistory history; resetSceneHistory(history, f.state);
    const auto* vertices = meshGeometry(f.state.files[0].mesh).vertices.data();
    REQUIRE(renameSceneObject(f.state, f.model(), "Pump")); recordSceneHistory(history, f.state);
    REQUIRE(renameSceneObject(f.state, f.part(), "Housing")); recordSceneHistory(history, f.state);
    REQUIRE(renameSceneObject(f.state, f.folder(), "Body")); recordSceneHistory(history, f.state);
    REQUIRE(restoreSceneObjectName(f.state, f.part())); recordSceneHistory(history, f.state);
    REQUIRE(history.snapshots.size() == 5);
    const auto step = [&](bool redo) { auto next = prepareSceneHistoryStep(history,f.state,clean,redo);
        REQUIRE(next); commitSceneHistoryStep(history,f.state,std::move(*next),redo); };
    step(false); CHECK(f.name(f.part()) == "Housing"); CHECK(f.name(f.folder()) == "Body");
    step(false); CHECK(f.name(f.folder()) == "Imported assembly");
    step(false); CHECK(f.name(f.part()) == "solid_007");
    step(false); CHECK(f.name(f.model()) == "pump.obj"); CHECK_FALSE(f.state.isDirty);
    step(true); step(true); step(true); step(true);
    CHECK(f.name(f.model()) == "Pump"); CHECK(f.name(f.folder()) == "Body"); CHECK(f.name(f.part()) == "solid_007");
    CHECK(meshGeometry(f.state.files[0].mesh).vertices.data() == vertices);
    applyView(f.state, view); CHECK(f.name(f.model()) == "Pump"); CHECK(f.name(f.folder()) == "Body");
}

TEST_CASE("name inspector snapshots and edits use the selected source identity") {
    NameFixture f;
    SceneInspectorRuntime runtime; selectSceneObject(f.state, f.part()); updateSceneInspector(runtime, f.state);
    const auto old = runtime.snapshot;
    REQUIRE(old.targets.size() == 1); CHECK(old.targets[0].originalName == "solid_007");
    CHECK(old.targets[0].fileName == "pump.obj");
    const std::array edits{InspectorEdit{InspectorEditKind::name,{},0,{},f.part(),f.state.sceneGeneration,"Housing"}};
    applyInspectorEdits(f.state, edits); CHECK(runtime.snapshot.targets[0].name == "solid_007");
    updateSceneInspector(runtime, f.state); CHECK(runtime.snapshot.targets[0].name == "Housing");
    const auto builds = runtime.builds; updateSceneInspector(runtime, f.state); CHECK(runtime.builds == builds);
    selectSceneObject(f.state, f.model()); applyInspectorEdits(f.state, edits); CHECK(f.name(f.model()) == "pump.obj");
    selectSceneObject(f.state, f.part(), true); applyInspectorEdits(f.state, edits); CHECK(f.name(f.model()) == "pump.obj");
    selectSceneObject(f.state, f.part()); ++f.state.sceneGeneration;
    const std::array stale{InspectorEdit{InspectorEditKind::restoreName,{},0,{},f.part(),old.generation}};
    applyInspectorEdits(f.state, stale); CHECK(f.name(f.part()) == "Housing");
    const std::array reset{InspectorEdit{InspectorEditKind::restoreName,{},0,{},f.part(),f.state.sceneGeneration}};
    applyInspectorEdits(f.state, reset); CHECK(f.name(f.part()) == "solid_007");
}

TEST_CASE("source name text input commits cancels and rejects empty text") {
    NameInputFixture f;
    REQUIRE(f.edit.active);
    SUBCASE("Enter applies selected replacement text") {
        f.type("Outer housing"); f.key(ImGuiKey_Enter);
        CHECK(f.result == SceneNameInputResult::apply); CHECK(f.edit.text == "Outer housing");
    }
    SUBCASE("Escape restores the previous label") {
        f.type("Discard"); f.key(ImGuiKey_Escape);
        CHECK(f.result == SceneNameInputResult::cancel); CHECK(f.edit.text == "Housing");
    }
    SUBCASE("click outside applies valid text") {
        f.type("Cover"); f.clickOutside(); CHECK(f.result == SceneNameInputResult::apply); CHECK(f.edit.text == "Cover");
    }
    SUBCASE("empty Enter keeps the editor open and recovery succeeds") {
        f.key(ImGuiKey_Backspace); f.key(ImGuiKey_Enter);
        CHECK(f.result == SceneNameInputResult::editing); CHECK(f.edit.invalid); CHECK(f.edit.active);
        f.type("Recovered"); f.key(ImGuiKey_Enter);
        CHECK(f.result == SceneNameInputResult::apply); CHECK(f.edit.text == "Recovered"); CHECK_FALSE(f.edit.invalid);
    }
    SUBCASE("empty click outside cancels") {
        f.key(ImGuiKey_Backspace); f.clickOutside(); CHECK(f.result == SceneNameInputResult::cancel); CHECK(f.edit.text == "Housing");
    }
}

TEST_CASE("source name edit follows click-away but discards removed replaced and externally edited targets") {
    NameFixture f;
    SceneNameEdit edit;
    selectSceneObject(f.state, f.part());
    beginSceneNameEdit(edit, *findSceneObject(f.state, f.part()), f.state.sceneGeneration);
    edit.text = "Pending"; edit.active = true;
    SUBCASE("selection changes commit to the previous target") {
        selectSceneObject(f.state, f.model()); synchronizeSceneNameEdit(f.state, edit);
        CHECK(f.name(f.part()) == "Pending"); CHECK(f.name(f.model()) == "pump.obj");
    }
    SUBCASE("hidden editor finishes valid text") {
        synchronizeSceneNameEdit(f.state, edit, true); CHECK(f.name(f.part()) == "Pending");
    }
    SUBCASE("empty click-away keeps the previous name") {
        edit.text = "  "; selectSceneObject(f.state, f.model()); synchronizeSceneNameEdit(f.state, edit);
        CHECK(f.name(f.part()) == "solid_007");
    }
    SUBCASE("new scene cancels") {
        ++f.state.sceneGeneration; synchronizeSceneNameEdit(f.state, edit);
        CHECK(f.name(f.part()) == "solid_007");
    }
    SUBCASE("external rename cancels") {
        renameSceneObject(f.state, f.part(), "External"); synchronizeSceneNameEdit(f.state, edit);
        CHECK(f.name(f.part()) == "External");
    }
    SUBCASE("removed object cancels") {
        f.state.files.clear(); f.state.sceneNodes.clear(); synchronizeSceneNameEdit(f.state, edit);
    }
    CHECK(edit.objectId == 0);
}
