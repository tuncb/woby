#include "background_load.h"
#include "comparison_scene.h"
#include "difference_mesh.h"
#include "importer_host.h"
#include "ui_operations.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stop_token>
#include <thread>

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now()-start).count(); }
Json meshInfo(const woby::Mesh& mesh) {
    const auto& g = woby::meshGeometry(mesh);
    Json groups = Json::array();
    for (const auto& group : g.nodes) {
        woby::Coordinate low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
        double volume = 0;
        for (size_t i=group.indexOffset; i<size_t(group.indexOffset)+group.indexCount; i+=3) {
            std::array<woby::Coordinate,3> p;
            for (size_t j=0;j<3;++j) {
                p[j]=woby::meshPosition(mesh,g.indices[i+j]);
                for(size_t k=0;k<3;++k) {p[j][k]+=mesh.origin[k];low[k]=std::min(low[k],p[j][k]);high[k]=std::max(high[k],p[j][k]);}
            }
            volume+=(p[0][0]*(p[1][1]*p[2][2]-p[1][2]*p[2][1])+p[0][1]*(p[1][2]*p[2][0]-p[1][0]*p[2][2])+p[0][2]*(p[1][0]*p[2][1]-p[1][1]*p[2][0]))/6;
        }
        groups.push_back({{"name",group.name},{"triangles",group.indexCount/3},{"min",low},{"max",high},{"signed_triangle_volume",volume}});
    }
    return {{"triangles",g.indices.size()/3},{"vertices",g.vertices.size()},{"groups",groups}};
}
int main(int argc, char** argv) {
    try {
        if(argc<6) {std::cerr<<"scene importers cell seconds mode[full|no-external|direct|inspect]\n";return 2;}
        spdlog::set_level(spdlog::level::off);
        const auto failures=woby::loadImporterFolder(argv[2]);
        if(!failures.empty()) throw std::runtime_error(woby::importerLoadFailureSummary(failures));
        const auto started=Clock::now();
        auto loaded=woby::loadSceneCpu(argv[1],{},{});
        auto state=woby::prepareSceneReplacement({},std::move(loaded.files),loaded.document);
        auto& comparison=state.comparisons.at(0);
        const auto id=comparison.objectId;
        const std::string mode=argv[5];
        if(mode=="no-external") {
            for(const auto& part : comparison.a) if(part.name.find("External")!=std::string::npos)
                woby::setComparisonObjectsEnabled(state,{part.objectId},woby::ComparisonSide::a,false,id);
        }
        if(mode=="pair" || mode=="pair-direct") {
            if(argc!=8)throw std::runtime_error("pair modes need zero-based A and B member indexes");
            const auto partA=comparison.a.at(std::stoul(argv[6])).objectId;
            const auto partB=comparison.b.at(std::stoul(argv[7])).objectId;
            auto settings=comparison.settings;
            settings.lockPlacement=false;settings.alignOrigins=false;
            woby::setComparisonSettings(state,settings,id);
            woby::setComparisonObjectsEnabled(state,{},woby::ComparisonSide::a,false,id);
            woby::setComparisonObjectsEnabled(state,{},woby::ComparisonSide::b,false,id);
            woby::setComparisonObjectsEnabled(state,{partA},woby::ComparisonSide::a,true,id);
            woby::setComparisonObjectsEnabled(state,{partB},woby::ComparisonSide::b,true,id);
        }
        const auto snapshot=woby::snapshotComparisonInputs(state,id);
        const auto worldStarted=Clock::now();
        const auto a=woby::comparisonWorldMesh(snapshot,woby::ComparisonSide::a), b=woby::comparisonWorldMesh(snapshot,woby::ComparisonSide::b);
        const auto assemblySeconds=seconds(worldStarted);
        Json record={{"event","inputs"},{"load_and_assembly_seconds",seconds(started)},{"assembly_seconds",assemblySeconds},{"a",meshInfo(a)},{"b",meshInfo(b)},{"mode",mode}};
        std::cout<<record.dump()<<std::endl;
        if(mode=="inspect")return 0;
        auto settings=comparison.settings.difference;
        settings.method=mode=="direct"||mode=="pair-direct"?woby::DifferenceMethod::direct:woby::DifferenceMethod::approximate;
        settings.cellSize=std::stod(argv[3]);
        const auto limit=std::stod(argv[4]);
        std::stop_source stop;
        const auto runStart=Clock::now();
        std::jthread timer([&](std::stop_token done){while(!done.stop_requested()&&seconds(runStart)<limit)std::this_thread::sleep_for(std::chrono::milliseconds(20));if(!done.stop_requested())stop.request_stop();});
        Json result={{"event","result"},{"cell_size",settings.cellSize},{"mode",mode},{"limit_seconds",limit}};
        try {
            const auto output=woby::buildDifferenceMesh(a,b,settings,stop.get_token());
            result["status"]="success";
            result["volumes"]=output.volumes;
            result["triangles"]={woby::meshGeometry(output.parts[0]).indices.size()/3,woby::meshGeometry(output.parts[1]).indices.size()/3};
            result["removed_collapsed"]=output.removedCollapsedTriangles;
            result["message"]=output.message;
        } catch(const std::exception& e) {result["status"]=stop.stop_requested()?"canceled":"error";result["error"]=e.what();}
        result["seconds"]=seconds(runStart);timer.request_stop();
        std::cout<<result.dump()<<std::endl;
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}
