#include "freeform_gpu.h"
#include "freeform_trim_fixture.h"
#include "graphics_helpers.h"
#include <doctest/doctest.h>
#include <cmath>

namespace {
struct FreeformDevice {
    bool initialized=false;
    ~FreeformDevice() { if (initialized) { woby::graphics::shutdown(); } }
};
}

TEST_CASE("Native freeform compute matches CPU rational surfaces curves and topology") {
    namespace g=woby::graphics;
    FreeformDevice device;
    device.initialized=g::init({}); REQUIRE(device.initialized);
    const auto program=g::createProgram(woby::loadShader(std::filesystem::path(WOBY_TEST_ASSET_DIRECTORY)
        / "shaders" / woby::rendererShaderFolder(g::getRendererType()) / "cs_freeform.bin"),true);
    for (bool trimmed : {false,true}) for (bool authored : {false,true}) {
        woby::FreeformPatch surface;
        surface.surface=true; surface.degreeU=2; surface.degreeV=1; surface.countU=3; surface.countV=2;
        surface.knotsU={10,10,10,14,14,14}; surface.knotsV={-2,-2,2,2};
        surface.domainU={10,14}; surface.domainV={-1,2};
        for (int y=0;y<2;++y) {
            surface.controls.push_back({1,0,double(y),1});
            surface.controls.push_back({1,1,double(y),std::sqrt(.5)});
            surface.controls.push_back({0,1,double(y),1});
            for (int x=0;x<3;++x) {
                surface.texcoords.push_back({double(x)/2,double(y)});
                if (authored) { surface.normals.push_back({0,0,-1}); }
            }
        }
        if (trimmed) {
            auto trim=std::make_shared<woby::FreeformTrimming>();
            trim->regions.push_back({{}, {trimPolygon({{11,-.5},{13,-.5},{13,1.5},{11,1.5}})}});
            surface.trimming=trim;
        }
        auto curve=surface; curve.trimming.reset(); curve.surface=false; curve.degreeV=0; curve.countV=1;
        curve.controls.resize(3); curve.normals.clear(); curve.texcoords.clear(); curve.knotsV.clear();
        woby::Mesh mesh;
        // Existing polygon data must survive compute writes at nonzero offsets.
        mesh.vertices={{{9,8,7},{0,0,1},{0,0}},{{8,7,6},{0,0,1},{0,0}},{{7,6,5},{0,0,1},{0,0}}};
        mesh.precisePositions={{9,8,7},{8,7,6},{7,6,5}};
        mesh.indices={0,1,2}; mesh.lineIndices={0,1};
        woby::appendFreeformGeometry(mesh,{surface,curve});
        woby::rebaseMesh(mesh,{.25,-.75,1.5});
        auto initial=mesh.vertices;
        for (size_t i=3;i<initial.size();++i) { initial[i].position={-99,-99,-99}; initial[i].texcoord={0,0}; }
        for (size_t i=3;i<mesh.freeform->grids[1].vertexOffset;++i) { initial[i].normal={0,0,0}; }
        const auto vb=g::createVertexBuffer(g::copy(initial.data(),static_cast<uint32_t>(initial.size()*sizeof(woby::Vertex))),{32});
        auto triangleInput=mesh.indices, lineInput=mesh.lineIndices;
        if (!trimmed) { std::fill(triangleInput.begin()+3,triangleInput.end(),0); }
        std::fill(lineInput.begin()+2,lineInput.end(),0);
        const auto ib=g::createIndexBuffer(g::copy(triangleInput.data(),static_cast<uint32_t>(triangleInput.size()*4)),WOBY_GPU_BUFFER_INDEX32);
        const auto lb=g::createIndexBuffer(g::copy(lineInput.data(),static_cast<uint32_t>(lineInput.size()*4)),WOBY_GPU_BUFFER_INDEX32);
        woby::dispatchFreeformGpu(mesh,vb,ib,lb,program,0);
        std::vector<woby::Vertex> output(initial.size());
        std::vector<uint32_t> triangles(triangleInput.size()), lines(lineInput.size());
        g::readBuffer(vb,output.data()); g::readBuffer(ib,triangles.data());
        const auto ready=g::readBuffer(lb,lines.data());
        while (g::frame()<ready) {}
        CHECK(triangles==mesh.indices); CHECK(lines==mesh.lineIndices);
        for (size_t i=0;i<output.size();++i) {
            CAPTURE(i); CAPTURE(authored); CAPTURE(trimmed);
            for (size_t k=0;k<3;++k) {
                CHECK(output[i].position[k]==doctest::Approx(mesh.vertices[i].position[k]).epsilon(2e-5));
                CHECK(output[i].normal[k]==doctest::Approx(mesh.vertices[i].normal[k]).epsilon(2e-5));
            }
            for (size_t k=0;k<2;++k) { CHECK(output[i].texcoord[k]==doctest::Approx(mesh.vertices[i].texcoord[k]).epsilon(2e-5)); }
        }
        g::destroy(vb); g::destroy(ib); g::destroy(lb);
    }
    g::destroy(program);
}
