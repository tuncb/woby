#include "comparison_scene.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <iostream>

namespace {
using Clock = std::chrono::steady_clock;

// No files are loaded; all synthetic paths share an explicit temporary root.
struct BenchmarkDirectory {
    std::filesystem::path root;
    BenchmarkDirectory()
    {
        const auto prefix = "woby-scene-benchmark-" + std::to_string(Clock::now().time_since_epoch().count());
        for (size_t attempt = 0;; ++attempt) {
            root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / (prefix + std::to_string(attempt));
            if (std::filesystem::create_directory(root)) { break; }
        }
    }
    ~BenchmarkDirectory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

woby::UiState scene(size_t parts, const std::filesystem::path& root)
{
    woby::UiState state;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    for (size_t i = 0; i < parts; ++i) {
        mesh.nodes.push_back({"part" + std::to_string(i), static_cast<uint32_t>(mesh.indices.size()), 3});
        mesh.indices.insert(mesh.indices.end(), {0, 1, 2});
    }
    state.files.push_back(woby::createUiFileState(root / "model.obj", std::move(mesh), 0));
    woby::appendFolderTreeSceneNode(state, root, 0, 1);
    const auto comparison = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, comparison);
    (void)woby::createView(state);
    (void)woby::createView(state);
    state.selectedSceneObjects = {state.files[0].objectId};
    return state;
}

template <typename Work>
void measure(const char* workload, size_t size, size_t batch, Work work)
{
    const auto expected = work();
    std::vector<double> samples;
    for (size_t run = 0; run < 7; ++run) {
        const auto start = Clock::now();
        for (size_t i = 0; i < batch; ++i) {
            if (work() != expected) { throw std::runtime_error("Scene benchmark result changed."); }
        }
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count()
            / static_cast<double>(batch));
    }
    std::sort(samples.begin(), samples.end());
    std::cout << workload << ',' << size << ',' << samples.front() << ',' << samples[3] << ','
        << samples.back() << ',' << expected << std::endl;
}
}

// Opt-in only: shares the real production functions already linked into tests.
// Invoke with --test-case="scene query benchmark" --no-skip=true in Release.
TEST_CASE("scene query benchmark" * doctest::skip())
{
    const BenchmarkDirectory directory;
    std::cout << "workload,parts,min_ms,median_ms,max_ms,checksum\n";
    for (const size_t count : {16u, 256u, 4096u}) {
        const auto state = scene(count, directory.root);
        const size_t batch = count < 4096 ? 30 : 3;
        measure("inspector", count, batch, [&] {
            double checksum = 0;
            for (int property = 0; property <= static_cast<int>(woby::UiObjectProperty::blue); ++property) {
                const auto value = woby::selectedObjectProperty(state, static_cast<woby::UiObjectProperty>(property));
                checksum += value.value + static_cast<double>(value.available) + static_cast<double>(value.mixed);
            }
            return checksum;
        });
        measure("document", count, batch, [&] {
            const auto document = woby::createSceneDocument(state);
            return document.files[0].groups.size() + document.comparisons[0].a.size()
                + document.views[0].objects.size() + document.views[1].parts.size();
        });
        measure("signature", count, batch * 10, [&] {
            return woby::comparisonGeometrySignature(state, state.comparisons[0].objectId);
        });
    }
}
