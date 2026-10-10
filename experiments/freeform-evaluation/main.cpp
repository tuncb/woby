// Compare the production sample reuse path with the parameter-based wrapper.
#include "freeform.h"
#include "obj_mesh.h"
#include "utf8_path.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
using namespace woby;

// The production normal rules, with the initial sample supplied by the caller.
std::array<float, 3> differentialNormal(const FreeformSample& sample) {
    const auto& a = sample.du; const auto& b = sample.dv;
    const Coordinate cross{a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
    const double length = std::hypot(cross[0], cross[1], cross[2]);
    if (!(length > 0) || !std::isfinite(length)) { return {}; }
    return {static_cast<float>(cross[0]/length), static_cast<float>(cross[1]/length), static_cast<float>(cross[2]/length)};
}
std::array<float, 3> reusedNormal(const FreeformPatch& patch, double u, double v,
    const FreeformSample& sample, size_t& fallbacks) {
    if (!patch.surface) { return {0,0,1}; }
    if (!patch.normals.empty()) {
        const double length = std::hypot(sample.normal[0], sample.normal[1], sample.normal[2]);
        if (length > 0 && std::isfinite(length)) {
            return {static_cast<float>(sample.normal[0]/length), static_cast<float>(sample.normal[1]/length), static_cast<float>(sample.normal[2]/length)};
        }
    }
    auto result = differentialNormal(sample);
    if (validNormal(result)) { return result; }
    ++fallbacks;
    const double a = std::lerp(patch.domainU[0], patch.domainU[1], 1e-4);
    const double b = std::lerp(patch.domainU[0], patch.domainU[1], 1-1e-4);
    const double c = std::lerp(patch.domainV[0], patch.domainV[1], 1e-4);
    const double d = std::lerp(patch.domainV[0], patch.domainV[1], 1-1e-4);
    result = differentialNormal(evaluateFreeform(patch, std::clamp(u,a,b), std::clamp(v,c,d)));
    return validNormal(result) ? result : std::array<float,3>{0,0,1};
}
template<class F> void eachSample(const Mesh& mesh, F&& fn) {
    for (size_t i = 0; i < mesh.freeform->patches.size(); ++i) {
        const auto& p = mesh.freeform->patches[i]; const auto& g = mesh.freeform->grids[i];
        if (g.samples.empty()) {
            for (double v : g.v) for (double u : g.u) { fn(p,u,v); }
        } else { for (const auto& uv : g.samples) { fn(p,uv[0],uv[1]); } }
    }
}
void require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
FreeformPatch panel(bool singular) {
    FreeformPatch p;
    p.surface=true; p.degreeU=1; p.degreeV=1; p.countU=2; p.countV=2;
    p.knotsU={0,0,1,1}; p.knotsV=p.knotsU; p.domainU={0,1}; p.domainV={0,1};
    p.controls={{{0,0,0,1}},{{singular ? 0. : 1.,0,0,1}},{{0,1,0,1}},{{1,1,.3,1}}};
    return p;
}
void selfTest() {
    size_t fallbackCount = 0;
    for (bool singular : {false,true}) for (int authored : {0,1,2}) {
        auto p = panel(singular);
        if (authored) { p.normals.assign(4, authored==1 ? Coordinate{0,0,-1} : Coordinate{}); }
        validateFreeformPatch(p);
        for (double v : {0.,.5,1.}) for (double u : {0.,.5,1.}) {
            const auto s=evaluateFreeform(p,u,v);
            require(reusedNormal(p,u,v,s,fallbackCount)==freeformNormal(p,u,v), "Normal mismatch");
        }
    }
    require(fallbackCount>0, "Singular fallback was not exercised");
    auto curve=panel(false); curve.surface=false; curve.degreeV=0; curve.countV=1; curve.controls.resize(2);
    require(reusedNormal(curve,.5,0,evaluateFreeform(curve,.5),fallbackCount)==freeformNormal(curve,.5,0), "Curve mismatch");
}
double milliseconds(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
}

int main(int argc, char** argv) {
    try {
        selfTest();
        if (argc==2 && std::string(argv[1])=="--self-test") { std::cout << "Normal reuse checks passed\n"; return 0; }
        require(argc==2, "Usage: woby_freeform_evaluation_probe model.obj | --synthetic | --self-test");
        Mesh mesh;
        const auto start=Clock::now();
        if (std::string(argv[1])=="--synthetic") {
            std::vector<FreeformPatch> patches(1000,panel(false));
            appendFreeformGeometry(mesh,std::move(patches));
        } else { mesh=loadObjMesh(pathFromUtf8(argv[1])); }
        const double loadMs=milliseconds(start);
        require(mesh.freeform!=nullptr, "Model has no native freeform geometry");
        size_t samples=0, surfaces=0, fallbacks=0;
        eachSample(mesh,[&](const auto& p,double u,double v) {
            ++samples; surfaces+=p.surface ? 1u : 0u;
            const auto s=evaluateFreeform(p,u,v);
            require(reusedNormal(p,u,v,s,fallbacks)==freeformNormal(p,u,v), "Model normal mismatch");
        });
        nlohmann::json out={{"input",argv[1]}, {"load_ms_context_only",loadMs},
            {"patches",mesh.freeform->patches.size()}, {"samples",samples}, {"surface_samples",surfaces},
            {"singular_fallback_samples",fallbacks}, {"all_normals_exactly_equal",true},
            {"mesh_vertices",meshGeometry(mesh).vertices.size()}, {"mesh_triangles",meshGeometry(mesh).indices.size()/3}};
        auto evaluate=[&](bool reuse) {
            double checksum=0;
            const auto begin=Clock::now();
            eachSample(mesh,[&](const auto& p,double u,double v) {
                const auto s=evaluateFreeform(p,u,v);
                const auto n=reuse ? freeformNormal(p,u,v,s) : freeformNormal(p,u,v);
                checksum+=s.position[0]+s.position[1]+s.position[2]+n[0]+n[1]+n[2]+s.texcoord[0]+s.texcoord[1];
            });
            return std::pair{milliseconds(begin),checksum};
        };
        for (int repeat=0; repeat<5; ++repeat) {
            std::pair<double,double> before,after;
            if (repeat%2==0) { before=evaluate(false); after=evaluate(true); }
            else { after=evaluate(true); before=evaluate(false); }
            require(before.second==after.second, "Evaluation checksum mismatch");
            out["evaluation_pairs"].push_back({{"before_ms",before.first},{"reuse_ms",after.first},{"checksum",before.second}});
        }
        out["gpu_evaluation_removed"] = true;
        std::cout << out.dump(2) << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
