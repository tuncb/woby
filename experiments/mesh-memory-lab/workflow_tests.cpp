#include "workflow.h"
#include "ui_state.h"
#include "utf8_path.h"
#include <doctest/doctest.h>
#include <chrono>
#include <fstream>
#include <limits>

namespace {
struct WorkflowFixture {
    std::filesystem::path root;
    WorkflowFixture()
    {
        const auto prefix = "meshflow_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = 0;; ++i) {
            root = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(i));
            if (std::filesystem::create_directory(root)) { break; }
        }
    }
    ~WorkflowFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void write(const std::string& filename, const std::string& content) const
    {
        std::ofstream file(root / woby::pathFromUtf8(filename), std::ios::binary); file << content;
    }
};
std::string diagram()
{
    return R"({"format":"mesh-memory-lab.workflow","version":1,"title":"Branching example",
        "description":"An independently stored diagram.","initial_node":"check",
        "nodes":[
            {"id":"input","title":"Input","kind":"data","position":[0,0]},
            {"id":"check","title":"Check","kind":"transform","summary":"Choose a path","description":"Validate the snapshot.","position":[240,0]},
            {"id":"output","title":"Output","kind":"data","position":[480,0]}],
        "edges":[{"from":"input","to":"check"},{"from":"check","to":"output","label":"current"},
            {"from":"check","to":"input","label":"retry"}]})";
}
std::string replace(std::string value, const std::string& from, const std::string& to)
{
    const auto offset = value.find(from);
    REQUIRE(offset != std::string::npos);
    value.replace(offset, from.size(), to); return value;
}
} // namespace

TEST_CASE("Workflow diagrams preserve branches cycles layout and selected node through serialization")
{
    const auto workflow = mesh_lab::parseWorkflow(diagram());
    REQUIRE(workflow.nodes.size() == 3);
    REQUIRE(workflow.edges.size() == 3);
    CHECK(workflow.objSource.empty());
    CHECK(workflow.initialNode == 1);
    CHECK(workflow.extent[0] == 480 + mesh_lab::workflowCardWidth);
    const auto copy = mesh_lab::parseWorkflow(mesh_lab::serializeWorkflow(workflow));
    CHECK(copy.title == workflow.title);
    CHECK(copy.description == workflow.description);
    CHECK(copy.nodes[1].description == workflow.nodes[1].description);
    CHECK(copy.nodes[2].position == workflow.nodes[2].position);
    CHECK(copy.edges[2].from == 1);
    CHECK(copy.edges[2].to == 0);
    CHECK(copy.edges[2].label == "retry");
    CHECK(copy.initialNode == 1);
    CHECK(mesh_lab::serializeWorkflow(copy) == mesh_lab::serializeWorkflow(workflow));
}
TEST_CASE("Workflow validation rejects unsupported formats and broken graph references")
{
    const auto source = diagram();
    CHECK_THROWS((void)mesh_lab::parseWorkflow("not JSON"));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "\"version\":1", "\"version\":2")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "\"version\":1", "\"version\":1.0")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "mesh-memory-lab.workflow", "another-format")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "\"id\":\"output\"", "\"id\":\"input\"")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "\"to\":\"output\"", "\"to\":\"missing\"")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "\"initial_node\":\"check\"", "\"initial_node\":\"missing\"")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "\"kind\":\"transform\"", "\"kind\":\"script\"")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "\"summary\"", "\"summmary\"")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "[240,0]", "[-1,0]")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "[240,0]", "[240,20001]")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(replace(source, "[240,0]", "[240]")));
    CHECK_THROWS((void)mesh_lab::parseWorkflow(std::string(mesh_lab::maxWorkflowBytes + 1, ' ')));
    auto invalid = mesh_lab::parseWorkflow(source);
    invalid.nodes[0].position[0] = std::numeric_limits<float>::infinity();
    CHECK_THROWS((void)mesh_lab::serializeWorkflow(invalid));
    invalid = mesh_lab::parseWorkflow(source);
    invalid.edges.push_back(invalid.edges.front());
    CHECK_THROWS((void)mesh_lab::serializeWorkflow(invalid));
    invalid.edges = {{0,0,{}}};
    CHECK_THROWS((void)mesh_lab::serializeWorkflow(invalid));
}
TEST_CASE("Live inspector bindings require a source and matching data or transform node")
{
    auto workflow = mesh_lab::parseWorkflow(diagram());
    workflow.nodes[0].inspector = mesh_lab::Node::mesh;
    CHECK_THROWS((void)mesh_lab::serializeWorkflow(workflow));
    workflow.objSource = std::string(mesh_lab::internalObjSource());
    CHECK_NOTHROW((void)mesh_lab::serializeWorkflow(workflow));
    workflow.nodes[0].inspector = mesh_lab::Node::parse;
    CHECK_THROWS((void)mesh_lab::serializeWorkflow(workflow));
    workflow.nodes[0].inspector = static_cast<mesh_lab::Node>(999);
    CHECK_THROWS((void)mesh_lab::serializeWorkflow(workflow));
}
TEST_CASE("Workflow files round trip from an independent temporary root and never overwrite")
{
    WorkflowFixture fixture;
    auto workflow = mesh_lab::parseWorkflow(diagram());
    workflow.title = "Workflow \xc3\xa9";
    workflow.objSource = std::string(mesh_lab::internalObjSource());
    const auto path = fixture.root / woby::pathFromUtf8("workflow \xc3\xa9.meshflow");
    mesh_lab::saveWorkflow(path, workflow);
    const auto copy = mesh_lab::loadWorkflow(path);
    CHECK(copy.title == workflow.title);
    CHECK(copy.objSource == workflow.objSource);
    workflow.title = "Must not replace saved file";
    CHECK_THROWS((void)mesh_lab::saveWorkflow(path, workflow));
    CHECK(mesh_lab::loadWorkflow(path).title == copy.title);
    CHECK(std::distance(std::filesystem::directory_iterator(fixture.root), std::filesystem::directory_iterator{}) == 1);
    CHECK_THROWS((void)mesh_lab::loadWorkflow(fixture.root / "missing.meshflow"));
}
TEST_CASE("Workflow discovery isolates malformed files and captures live meshes from embedded sources")
{
    WorkflowFixture fixture;
    fixture.write("01-diagram.meshflow", diagram());
    fixture.write("02-broken.meshflow", "{}");
    fixture.write("ignored.json", "{}");
    auto workflow = mesh_lab::parseWorkflow(diagram());
    workflow.objSource = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    mesh_lab::saveWorkflow(fixture.root / "03-live.MESHFLOW", workflow);
    workflow.objSource = "v 0 0 0\nf 1 2 3\n";
    mesh_lab::saveWorkflow(fixture.root / "04-bad-source.meshflow", workflow);
    const auto library = mesh_lab::loadWorkflowLibrary(fixture.root);
    REQUIRE(library.entries.size() == 4);
    CHECK(library.directory == fixture.root);
    CHECK(library.entries[0].document.has_value());
    CHECK_FALSE(library.entries[0].trace);
    CHECK_FALSE(library.entries[1].document.has_value());
    CHECK_FALSE(library.entries[1].error.empty());
    REQUIRE(library.entries[2].trace);
    CHECK(library.entries[2].trace->triangles.size() == 1);
    CHECK(library.entries[2].trace->generatedNormals);
    CHECK(library.entries[2].trace->texcoords.empty());
    CHECK_FALSE(library.entries[3].document.has_value());
    CHECK_FALSE(library.entries[3].error.empty());
}
TEST_CASE("Switching workflow resets linked selection and leaves invalid requests unchanged")
{
    WorkflowFixture fixture;
    fixture.write("diagram.meshflow", diagram());
    auto live = mesh_lab::parseWorkflow(diagram());
    live.objSource = std::string(mesh_lab::internalObjSource());
    live.nodes[1].inspector = mesh_lab::Node::pack;
    mesh_lab::saveWorkflow(fixture.root / "live.meshflow", live);
    const auto library = mesh_lab::loadWorkflowLibrary(fixture.root);
    mesh_lab::UiState state;
    mesh_lab::selectWorkflow(state,library,1);
    CHECK(state.workflow == 1);
    CHECK(state.workflowNode == 1);
    CHECK(state.node == mesh_lab::Node::pack);
    mesh_lab::selectTriangle(state,*library.entries[1].trace,3);
    mesh_lab::selectComponent(state,7);
    mesh_lab::orbit(state,1,1,2);
    mesh_lab::selectWorkflow(state,library,0);
    CHECK(state.workflow == 0);
    CHECK(state.workflowNode == 1);
    CHECK(state.triangle == 0);
    CHECK(state.component == 0);
    CHECK(state.zoom == 1);
    mesh_lab::selectWorkflowNode(state,*library.entries[0].document,2);
    mesh_lab::selectWorkflowNode(state,*library.entries[0].document,999);
    mesh_lab::selectWorkflow(state,library,999);
    CHECK(state.workflow == 0);
    CHECK(state.workflowNode == 2);
}
TEST_CASE("Packaged workflow inputs load with live data and independent diagram topology")
{
    WorkflowFixture fixture;
    for (const auto& entry : std::filesystem::directory_iterator(MESH_LAB_WORKFLOW_DIRECTORY)) {
        if (entry.path().extension() == ".meshflow") { std::filesystem::copy_file(entry.path(), fixture.root / entry.path().filename()); }
    }
    const auto library = mesh_lab::loadWorkflowLibrary(fixture.root);
    REQUIRE(library.entries.size() == 3);
    for (const auto& entry : library.entries) { INFO(entry.error); REQUIRE(entry.document.has_value()); }
    REQUIRE(library.entries[0].trace);
    REQUIRE(library.entries[2].trace);
    CHECK(library.entries[0].trace->mesh.vertices.size() == 8);
    CHECK(library.entries[2].trace->mesh.vertices.size() == 4);
    CHECK(library.entries[2].trace->mesh.indices.size() == 6);
    CHECK_FALSE(library.entries[1].trace);
    CHECK(library.entries[1].document->nodes.size() != mesh_lab::pipeline.size());
}
