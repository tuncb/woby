#include "surface_mesh_quality.h"
#include "obj_mesh.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

namespace {
using namespace woby;
using Json = nlohmann::json;
using FaceDisplay = SurfaceQualityFace;
using Ranges = std::array<QualityDistribution, surfaceQualityMetricCount>;
void require(bool condition, const char* text) { if (!condition) { throw std::runtime_error(text); } }
std::vector<FaceDisplay> packFaces(const Mesh& mesh, const SurfaceMeshQuality& quality, const Ranges& ranges, std::stop_token stop = {})
{
    return surfaceQualityDisplayFaces(mesh, quality, ranges, stop);
}
void verify(const Mesh& mesh, const std::vector<Vertex>& expanded, const std::vector<FaceDisplay>& faces)
{
    const auto& g = meshGeometry(mesh);
    require(expanded.size() == faces.size()*3, "Display count differs");
    for (size_t face=0; face<faces.size(); ++face) {
        for (size_t k=0; k<3; ++k) {
            Vertex value{g.vertices.at(g.indices.at(face*3+k)).position, faces[face].normal, {}};
            if (k<2) { value.texcoord = {faces[face].values[k*2], faces[face].values[k*2+1]}; }
            require(std::memcmp(&value, &expanded[face*3+k], sizeof(Vertex)) == 0, "Reconstructed vertex bits differ");
        }
        for (size_t m=0; m<4; ++m) {
            require(std::bit_cast<uint32_t>(faces[face].values[m]) ==
                std::bit_cast<uint32_t>(expanded[face*3+m/2].texcoord[m%2]), "Selected shader metric differs");
        }
    }
}
uint64_t digest(float value, uint64_t hash) { return (hash ^ std::bit_cast<uint32_t>(value))*1099511628211ull; }
uint64_t fetchExpanded(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& order)
{
    uint64_t hash=14695981039346656037ull;
    for (const auto face : order) {
        const auto metric = face%4;
        for (size_t k=0; k<3; ++k) {
            const auto& v=vertices[size_t(face)*3+k];
            for (auto x:v.position) { hash=digest(x,hash); }
            for (auto x:v.normal) { hash=digest(x,hash); }
            hash=digest(vertices[size_t(face)*3+metric/2].texcoord[metric%2],hash);
        }
    }
    return hash;
}
uint64_t fetchFaces(const Mesh& mesh, const std::vector<FaceDisplay>& faces, const std::vector<uint32_t>& order)
{
    uint64_t hash=14695981039346656037ull;
    const auto& g=meshGeometry(mesh);
    for (const auto face : order) {
        const auto metric=face%4;
        for (size_t k=0; k<3; ++k) {
            for (auto x:g.vertices[g.indices[size_t(face)*3+k]].position) { hash=digest(x,hash); }
            for (auto x:faces[face].normal) { hash=digest(x,hash); }
            hash=digest(faces[face].values[metric],hash);
        }
    }
    return hash;
}
void selfTest()
{
    auto mesh=loadObjMeshText("v -0 0 0\nv 1 0 0\nv 0 1 0\nv 2 0 0\nf 1 2 3\nf 2 4 3\nf 1 1 1\n");
    auto quality=inspectSurfaceMeshQuality(mesh);
    quality.triangles[0].values={-1,2,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()};
    quality.triangles[1].values={-0.0,1,.5,.25};
    Ranges ranges;
    auto expanded=surfaceQualityDisplayVertices(mesh,quality,ranges);
    auto compact=packFaces(mesh,quality,ranges);
    verify(mesh,expanded,compact);
    require(compact[0].values == std::array<float,4>{0,1,-2,-2}, "Sentinels or clamping differ");
    require(compact[2].values == std::array<float,4>{-1,-1,-1,-1}, "Degenerate sentinel differs");
    require(fetchExpanded(expanded,{2,0,1})==fetchFaces(mesh,compact,{2,0,1}), "Indexed fetch differs");
    // A nonzero draw offset must select the same face as the old sliced buffer.
    require(fetchExpanded(expanded,{1})==fetchFaces(mesh,compact,{1}), "Draw offset differs");
    std::stop_source stop; stop.request_stop();
    bool rejected=false;
    try { (void)packFaces(mesh,quality,ranges,stop.get_token()); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected,"Cancellation ignored");
    quality.triangles.pop_back(); rejected=false;
    try { (void)packFaces(mesh,quality,ranges); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected,"Invalid face count accepted");
}
}
int main(int argc,char** argv)
{
    try {
        selfTest();
        if (argc==2 && std::string_view(argv[1])=="--self-test") { return 0; }
        require(argc==3,"usage: woby_face_display_probe ABSOLUTE_OBJ_PATH SAMPLE_COUNT");
        const int samples=std::stoi(argv[2]);
        require(samples>0 && samples<=15 && std::filesystem::path(argv[1]).is_absolute(),"Invalid arguments");
        auto mesh=loadObjMesh(argv[1]);
        std::cerr<<"Measuring source quality\n";
        const auto quality=inspectSurfaceMeshQuality(mesh);
        const auto ranges=surfaceQualityDistributions(quality,{});
        std::vector<uint32_t> order(quality.triangles.size());
        std::iota(order.begin(),order.end(),uint32_t{0});
        auto shuffled=order;
        std::mt19937 rng(1729); std::shuffle(shuffled.begin(),shuffled.end(),rng);
        Json out{{"model",argv[1]},{"faces",order.size()},{"render_vertices",meshGeometry(mesh).vertices.size()},
            {"expanded_bytes",order.size()*3*sizeof(Vertex)},{"compact_bytes",order.size()*sizeof(FaceDisplay)},
            {"equivalence","all reconstructed vertex bits and all four shader metrics"}};
        auto expanded=surfaceQualityDisplayVertices(mesh,quality,ranges);
        auto compact=packFaces(mesh,quality,ranges);
        verify(mesh,expanded,compact);
        require(fetchExpanded(expanded,order)==fetchFaces(mesh,compact,order),"Sequential warmup differs");
        require(fetchExpanded(expanded,shuffled)==fetchFaces(mesh,compact,shuffled),"Shuffled warmup differs");
        const auto time=[&](const char* name,auto work) {
            const auto begin=std::chrono::steady_clock::now();
            auto result=work();
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            out["samples_ms"][name].push_back(ms);
            return result;
        };
        // One untimed warmup above. Alternate order to reduce allocator/order bias.
        for (int run=0; run<samples; ++run) {
            if (run%2) {
                compact=time("compact_pack",[&]{return packFaces(mesh,quality,ranges);});
                expanded=time("expanded_pack",[&]{return surfaceQualityDisplayVertices(mesh,quality,ranges);});
            } else {
                expanded=time("expanded_pack",[&]{return surfaceQualityDisplayVertices(mesh,quality,ranges);});
                compact=time("compact_pack",[&]{return packFaces(mesh,quality,ranges);});
            }
            verify(mesh,expanded,compact); // Outside timing; both buffers remain resident.
            for (bool random : {false,true}) {
                const auto& ids=random ? shuffled : order;
                uint64_t a,b;
                if (run%2) {
                    b=time(random?"compact_shuffled_cpu_fetch":"compact_sequential_cpu_fetch",[&]{return fetchFaces(mesh,compact,ids);});
                    a=time(random?"expanded_shuffled_cpu_fetch":"expanded_sequential_cpu_fetch",[&]{return fetchExpanded(expanded,ids);});
                } else {
                    a=time(random?"expanded_shuffled_cpu_fetch":"expanded_sequential_cpu_fetch",[&]{return fetchExpanded(expanded,ids);});
                    b=time(random?"compact_shuffled_cpu_fetch":"compact_sequential_cpu_fetch",[&]{return fetchFaces(mesh,compact,ids);});
                }
                require(a==b,"Timed fetch differs");
                out[random?"shuffled_hash":"sequential_hash"]=a;
            }
        }
        for (auto it=out["samples_ms"].begin();it!=out["samples_ms"].end();++it) {
            auto values=it.value().get<std::vector<double>>(); std::sort(values.begin(),values.end());
            out["median_ms"][it.key()]=values[values.size()/2];
        }
        std::cout<<out.dump(2)<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
