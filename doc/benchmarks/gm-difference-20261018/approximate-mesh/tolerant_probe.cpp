#include "background_load.h"
#include "comparison_scene.h"
#include "importer_host.h"
#include "ui_operations.h"
#include "difference_mesh.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <iostream>
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
int main() {
    try {
        spdlog::set_level(spdlog::level::off);
        const auto failures=woby::loadImporterFolder("D:/temp/GM-data/importers");
        if(!failures.empty())throw std::runtime_error(woby::importerLoadFailureSummary(failures));
        auto loaded=woby::loadSceneCpu("D:/temp/GM-data/test.woby",{},{});
        auto state=woby::prepareSceneReplacement({},std::move(loaded.files),loaded.document);
        const auto comparison=state.comparisons.at(0);const auto id=comparison.objectId;
        auto settings=comparison.settings;settings.lockPlacement=false;settings.alignOrigins=false;
        woby::setComparisonSettings(state,settings,id);
        const std::array<size_t,9> matches{7,6,8,1,2,4,3,5,0};
        for(size_t pair=0;pair<9;++pair) {
            woby::setComparisonObjectsEnabled(state,{},woby::ComparisonSide::a,false,id);
            woby::setComparisonObjectsEnabled(state,{},woby::ComparisonSide::b,false,id);
            woby::setComparisonObjectsEnabled(state,{comparison.a.at(pair).objectId},woby::ComparisonSide::a,true,id);
            woby::setComparisonObjectsEnabled(state,{comparison.b.at(matches[pair]).objectId},woby::ComparisonSide::b,true,id);
            const auto snapshot=woby::snapshotComparisonInputs(state,id);
            const auto a=woby::comparisonWorldMesh(snapshot,woby::ComparisonSide::a),b=woby::comparisonWorldMesh(snapshot,woby::ComparisonSide::b);
            const auto start=Clock::now();
            Json record={{"pair",pair},{"input",comparison.a.at(pair).name},{"plaxis",comparison.b.at(matches[pair]).name},
                {"triangles",{woby::meshGeometry(a).indices.size()/3,woby::meshGeometry(b).indices.size()/3}}};
            std::cout<<Json({{"event","start"},{"pair",pair}}).dump()<<std::endl;
            try {
                woby::DifferenceSettings construction;construction.method=woby::DifferenceMethod::approximateMesh;
                const auto result=woby::buildDifferenceMesh(a,b,construction);
                record["volumes"]=result.volumes;
                record["output_triangles"]={woby::meshGeometry(result.parts[0]).indices.size()/3,woby::meshGeometry(result.parts[1]).indices.size()/3};
                record["removed_collapsed"]=result.removedCollapsedTriangles;
                record["message"]=result.message;
                record["method"]=woby::differenceMethodName(result.construction.method);
                record["status"]="success";
            }catch(const std::exception&e){record["status"]="error";record["error"]=e.what();}
            record["seconds"]=std::chrono::duration<double>(Clock::now()-start).count();
            std::cout<<record.dump()<<std::endl;
        }
        return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}
}
