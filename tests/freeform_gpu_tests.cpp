#include "freeform.h"
#include "scene_renderer.h"
#include "freeform_trim_fixture.h"
#include <doctest/doctest.h>
#include <cmath>

namespace {
struct FreeformDevice {
    bool initialized=false;
    woby::GpuMesh mesh;
    ~FreeformDevice() { if (initialized) { woby::destroyGpuMesh(mesh); woby::graphics::shutdown(); } }
};
}

TEST_CASE("Native freeform upload preserves CPU rational surfaces curves and trim topology exactly") {
    namespace g=woby::graphics;
    FreeformDevice device;
    device.initialized=g::init({}); REQUIRE(device.initialized);
    for (bool distant : {false,true}) for (bool trimmed : {false,true}) for (bool authored : {false,true}) {
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
        if (distant) {
            mesh.origin={1e12,-1e12,1e12};
            for (auto* patch : {&surface,&curve}) for (auto& control : patch->controls) {
                for (size_t k=0;k<3;++k) { control[k]+=mesh.origin[k]; }
            }
        }
        // Mixed polygon and line ranges retain their original offsets through upload.
        editMeshGeometry(mesh).vertices={{{9,8,7},{0,0,1},{0,0}},{{8,7,6},{0,0,1},{0,0}},{{7,6,5},{0,0,1},{0,0}}};
        editMeshGeometry(mesh).precisePositions={{9,8,7},{8,7,6},{7,6,5}};
        editMeshGeometry(mesh).indices={0,1,2}; editMeshGeometry(mesh).lineIndices={0,1};
        woby::appendFreeformGeometry(mesh,{surface,curve});
        woby::rebaseMesh(mesh,{mesh.origin[0]+.25,mesh.origin[1]-.75,mesh.origin[2]+1.5});
        device.mesh=woby::createGpuMesh(mesh,woby::meshVertexLayout(),woby::gpuMeshEdges|woby::gpuMeshPoints);
        std::vector<woby::Vertex> output(meshGeometry(mesh).vertices.size());
        std::vector<uint32_t> triangles(meshGeometry(mesh).indices.size()), lines(meshGeometry(mesh).lineIndices.size());
        g::readBuffer(device.mesh.vertexBuffer,output.data());
        g::readBuffer(device.mesh.triangleIndexBuffer,triangles.data());
        const auto ready=g::readBuffer(device.mesh.importedLineBuffer,lines.data());
        while (g::frame()<ready) {}
        CHECK(triangles==meshGeometry(mesh).indices); CHECK(lines==meshGeometry(mesh).lineIndices);
        for (size_t i=0;i<output.size();++i) {
            CAPTURE(i); CAPTURE(authored); CAPTURE(trimmed); CAPTURE(distant);
            for (size_t k=0;k<3;++k) {
                CHECK(output[i].position[k]==meshGeometry(mesh).vertices[i].position[k]);
                CHECK(output[i].normal[k]==meshGeometry(mesh).vertices[i].normal[k]);
            }
            for (size_t k=0;k<2;++k) { CHECK(output[i].texcoord[k]==meshGeometry(mesh).vertices[i].texcoord[k]); }
        }
        woby::destroyGpuMesh(device.mesh);
    }
}
