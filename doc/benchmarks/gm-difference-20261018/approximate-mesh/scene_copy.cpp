#include "background_load.h"
#include "comparison_scene.h"
#include "importer_host.h"
#include "ui_operations.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <iostream>

int main(int argc, char** argv)
{
    try {
        if (argc != 4) { throw std::runtime_error("scene importers destination"); }
        spdlog::set_level(spdlog::level::off);
        const auto failures = woby::loadImporterFolder(argv[2]);
        if (!failures.empty()) { throw std::runtime_error(woby::importerLoadFailureSummary(failures)); }
        auto loaded = woby::loadSceneCpu(argv[1], {}, {});
        auto state = woby::prepareSceneReplacement({}, std::move(loaded.files), loaded.document);
        const auto original = state.comparisons.at(0);
        if (original.a.size() != 10 || original.b.size() != 9) { throw std::runtime_error("Unexpected input membership"); }
        while (!state.comparisons.empty()) { woby::removeComparison(state, state.comparisons.back().objectId); }
        const std::array<size_t, 9> matches{7, 6, 8, 1, 2, 4, 3, 5, 0};
        nlohmann::json record;
        woby::SceneObjectId focus = 0;
        for (size_t i = 0; i < matches.size(); ++i) {
            const auto &a = original.a.at(i), &b = original.b.at(matches[i]);
            const auto id = woby::createAnalysisFromObjects(state, woby::AnalysisTask::surfaceComparison, {a.objectId, b.objectId});
            if (!id) { throw std::runtime_error("Could not create unit comparison"); }
            woby::renameComparison(state, id, a.name + " / " + b.name + " [geometric match]");
            woby::setComparisonTranslation(state, id, {0, 0, 0});
            auto settings = woby::comparisonSettings(state, id);
            settings.lockPlacement = false;
            settings.alignOrigins = false;
            settings.enabled = i == 6;
            settings.mode = woby::ComparisonMode::overlay;
            settings.showBoundaries = false;
            settings.showNonManifold = false;
            settings.showWinding = false;
            settings.difference.method = woby::DifferenceMethod::approximateMesh;
            settings.difference.created = i == 6;
            settings.difference.visible = i == 6;
            for (size_t k = 0; k < woby::diagnosticCategoryCount; ++k) {
                woby::setDiagnosticAutoUpdate(settings, static_cast<woby::DiagnosticCategory>(k), false);
            }
            woby::setComparisonSettings(state, settings, id);
            settings.lockPlacement = true;
            woby::setComparisonSettings(state, settings, id);
            if (i == 6) { focus = id; }
            const auto snapshot = woby::snapshotComparisonInputs(state, id);
            const auto wa = woby::comparisonWorldMesh(snapshot, woby::ComparisonSide::a);
            const auto wb = woby::comparisonWorldMesh(snapshot, woby::ComparisonSide::b);
            record["pairs"].push_back({{"input", a.name}, {"plaxis", b.name}, {"id",id},
                {"a_triangles",woby::meshGeometry(wa).indices.size()/3}, {"b_triangles",woby::meshGeometry(wb).indices.size()/3}});
        }
        for (const auto &file : state.files) {
            woby::selectSceneObject(state, file.objectId);
            (void)woby::setSceneSelectionVisible(state, false);
        }
        woby::selectSceneObject(state, focus);
        woby::frameComparison(state, focus);
        woby::writeSceneDocument(argv[3], woby::createSceneDocument(state), false);
        record["path"] = argv[3];
        record["mapping_basis"] = "Inferred from geometry; user authorized use without geological-name confirmation";
        std::cout << record.dump(2) << std::endl;
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << std::endl; return 1; }
}
