#include "analysis_display.h"
#include "graphics_helpers.h"
#include "math_utils.h"
#include "obj_mesh.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <numeric>

namespace {
namespace g = woby::graphics;
using Json = nlohmann::json;
struct Session { ~Session() { g::shutdown(); } };
template<class T> g::VertexBufferHandle upload(const std::vector<T>& data) {
    return g::createVertexBuffer(g::copy(data.data(),static_cast<uint32_t>(data.size()*sizeof(T))),{sizeof(T)});
}
g::IndexBufferHandle uploadIndices(const std::vector<uint32_t>& data) {
    return g::createIndexBuffer(g::copy(data.data(),static_cast<uint32_t>(data.size()*sizeof(uint32_t))),WOBY_GPU_BUFFER_INDEX32);
}
Json measure(g::VertexBufferHandle vertices, g::IndexBufferHandle indices, g::VertexBufferHandle faces,
    g::ProgramHandle program, g::FaceDisplayKind kind, bool wire, g::UniformHandle uniform, std::array<float,4> parameters) {
    std::vector<double> samples;
    // The renderer reports timestamps three frames late. Twelve warmup frames
    // remove the preceding variant and pipeline creation from the sample set.
    for (size_t frame=0;frame<43;++frame) {
        g::touch(0); g::setUniform(uniform,parameters.data()); g::setVertexBuffer(0,vertices);
        if (wire) {
            if (g::isValid(faces)) { g::setTriangleWireframe(indices); }
            else { g::setIndexBuffer(indices); }
        } else if (g::isValid(faces)) { g::setFaceDisplay(faces,indices,kind); }
        g::setState(WOBY_GPU_STATE_WRITE_RGB|WOBY_GPU_STATE_WRITE_A|WOBY_GPU_STATE_WRITE_Z|WOBY_GPU_STATE_DEPTH_TEST_LESS
            | (wire ? WOBY_GPU_STATE_PT_LINES : uint64_t{0}));
        g::submit(0,program); g::frame();
        const auto stats = g::getStats();
        if (frame>=12) { samples.push_back(double(stats->gpuTimeEnd-stats->gpuTimeBegin)/1e6); }
    }
    auto sorted=samples; std::sort(sorted.begin(),sorted.end());
    return Json{{"median_ms",sorted[sorted.size()/2]},{"samples_ms",samples}};
}
}
int main(int argc,char** argv) {
    try {
        if (argc != 3) { throw std::runtime_error("Usage: woby_face_display_gpu_probe assets model.obj"); }
        Session session;
        if (!g::init({})) { throw std::runtime_error(g::initializationError()); }
        const auto comparison = woby::loadProgram(argv[1],"vs_comparison.bin","fs_comparison.bin");
        const auto color = woby::loadProgram(argv[1],"vs_color.bin","fs_color.bin");
        const auto comparisonUniform = g::createUniform("u_comparison",g::UniformType::Vec4);
        const auto colorUniform = g::createUniform("u_color",g::UniformType::Vec4);
        const auto output = g::createTexture2D(1024,768,false,1,g::TextureFormat::RGBA8,WOBY_GPU_TEXTURE_RT);
        const auto depth = g::createTexture2D(1024,768,false,1,g::TextureFormat::D24S8,WOBY_GPU_TEXTURE_RT);
        const std::array attachments{output,depth};
        const auto target = g::createFrameBuffer(2,attachments.data());
        g::setViewFrameBuffer(0,target); g::setViewRect(0,0,0,1024,768);
        g::setViewClear(0,WOBY_GPU_CLEAR_COLOR|WOBY_GPU_CLEAR_DEPTH,0x20242aff,1);
        std::array<float,16> identity{}; woby::identityMatrix(identity.data()); g::setViewTransform(0,identity.data(),identity.data());
        auto mesh = woby::loadObjMesh(argv[2]);
        auto& geometry=woby::editMeshGeometry(mesh);
        const auto bounds=mesh.bounds;
        float extent=0; for (size_t k=0;k<3;++k) { extent=std::max(extent,bounds.max[k]-bounds.min[k]); }
        for (auto& vertex:geometry.vertices) for (size_t k=0;k<3;++k) {
            vertex.position[k]=(vertex.position[k]-(bounds.max[k]+bounds.min[k])*.5f)/extent*(k==2?.8f:1.8f)+(k==2?.5f:0.f);
        }
        Json result{{"model",argv[2]},{"faces",geometry.indices.size()/3},{"renderer",g::getRendererName(g::getRendererType())},
            {"notes","GPU submission timestamps, 1024x768, no MSAA, identity camera after bounds normalization. Twelve warmups and 31 samples per block. Four alternating blocks. Fixed display values; analysis and upload excluded."}};
        for (const bool unshared : {false,true}) {
            auto vertices=geometry.vertices; auto indices=geometry.indices;
            if (unshared) {
                vertices.clear(); vertices.reserve(indices.size());
                for (auto i:indices) { vertices.push_back(geometry.vertices[i]); }
                std::iota(indices.begin(),indices.end(),0u);
            }
            const auto source=upload(vertices); const auto triangles=uploadIndices(indices);
            Json fixture{{"vertices",vertices.size()}};
            const size_t count=indices.size()/3;
            for (uint32_t mode=1;mode<=4;++mode) {
                std::vector<woby::Vertex> expanded;
                std::vector<uint32_t> edges;
                std::vector<woby::SurfaceQualityFace> quality;
                std::vector<woby::UvQualityFace> uv;
                std::vector<woby::DistanceFace> distance;
                if (mode==1) { quality.resize(count); }
                if (mode==2) { uv.resize(count); }
                if (mode==3) { distance.resize(count); }
                for (size_t face=0;face<count;++face) {
                    std::array<woby::Vertex,3> corners{vertices[indices[face*3]],vertices[indices[face*3+1]],vertices[indices[face*3+2]]};
                    if (mode==4) { for (size_t k=0;k<3;++k) { edges.insert(edges.end(),{indices[face*3+k],indices[face*3+(k+1)%3]}); } continue; }
                    const auto normal=woby::calculateFaceNormal(corners[0].position,corners[1].position,corners[2].position);
                    if (mode==1) { quality[face]={{.2f,.4f,.6f,.8f},normal,0}; }
                    if (mode==2) { uv[face].value={.4f,0}; for (size_t k=0;k<3;++k) { uv[face].normals[k]=corners[k].normal; } }
                    if (mode==3) {
                        auto& record=distance[face]; record.distances={.2f,.4f,.6f,.8f}; record.normals.fill(normal);
                        for (size_t k=0;k<3;++k) for (size_t axis=0;axis<3;++axis) { record.midpoints[k][axis]=(corners[k].position[axis]+corners[(k+1)%3].position[axis])*.5f; }
                        for (size_t k=0;k<12;++k) { const auto point=woby::distanceCorners[k]; expanded.push_back({point<3?corners[point].position:record.midpoints[point-3],normal,{record.distances[k/3],0}}); }
                    } else for (size_t k=0;k<3;++k) {
                        auto vertex=corners[k];
                        if (mode==1) { vertex.normal=normal; vertex.texcoord=k<2?std::array<float,2>{quality[face].values[k*2],quality[face].values[k*2+1]}:std::array<float,2>{}; }
                        else { vertex.texcoord=uv[face].value; }
                        expanded.push_back(vertex);
                    }
                }
                const auto faces=mode==1?upload(quality):mode==2?upload(uv):mode==3?upload(distance):source;
                const auto reference=mode==4?source:upload(expanded);
                const auto referenceIndices=mode==4?uploadIndices(edges):g::IndexBufferHandle{};
                Json times=Json::array();
                const auto kind=static_cast<g::FaceDisplayKind>(mode);
                const auto program=mode==4?color:comparison;
                const auto uniform=mode==4?colorUniform:comparisonUniform;
                const std::array<float,4> parameters=mode==1?std::array<float,4>{0,0,2,0}:mode==2?std::array<float,4>{0,1,6,0}:std::array<float,4>{.2f,1,1,0};
                for (size_t block=0;block<4;++block) {
                    Json pair;
                    for (size_t pass=0;pass<2;++pass) {
                        const bool compact=(block+pass)%2!=0;
                        pair[compact?"compact":"expanded"]=measure(compact?source:reference,compact?triangles:referenceIndices,compact?faces:g::VertexBufferHandle{},program,kind,mode==4,uniform,parameters);
                    }
                    times.push_back(pair);
                }
                fixture[mode==1?"quality":mode==2?"uv":mode==3?"distance":"wireframe"]=times;
                if (mode==4) { g::destroy(referenceIndices); }
                else { g::destroy(reference); g::destroy(faces); }
            }
            result[unshared?"unshared":"indexed"]=fixture;
            g::destroy(source); g::destroy(triangles);
        }
        std::cout<<result.dump(2)<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
