#pragma once
#include "control.h"
#include "obj_mesh.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace position_mapping {
inline void require(bool value, const char* field) {
    if (!value) { throw std::runtime_error(field); }
}
template<class T> inline void sameBuffer(const std::vector<T>& a, const std::vector<T>& b, const char* field) {
    require(a.size() == b.size(), field);
    require(a.empty() || std::memcmp(a.data(), b.data(), a.size()*sizeof(T)) == 0, field);
}
inline void sameMesh(const woby::Mesh& a, const woby::Mesh& b) {
    const auto& x = woby::meshGeometry(a); const auto& y = woby::meshGeometry(b);
    require(a.origin == b.origin && a.preparationDeferred == b.preparationDeferred, "origin/preparation");
    require(a.bounds.min == b.bounds.min && a.bounds.max == b.bounds.max
        && a.bounds.center == b.bounds.center && a.bounds.radius == b.bounds.radius, "bounds");
    require(a.originalBounds == b.originalBounds, "original bounds");
    sameBuffer(x.vertices,y.vertices,"vertex bytes, including normals and UVs");
    sameBuffer(x.precisePositions,y.precisePositions,"precise positions");
    sameBuffer(x.positionIndices,y.positionIndices,"position map");
    require(x.positionIndexing == y.positionIndexing,"position indexing");
    sameBuffer(x.indices,y.indices,"ordered triangle IDs");
    sameBuffer(x.lineIndices,y.lineIndices,"line IDs");
    sameBuffer(x.pointIndices,y.pointIndices,"point IDs");
    require(x.sourcePositions && y.sourcePositions && a.sourceData && b.sourceData,"source buffers");
    sameBuffer(*x.sourcePositions,*y.sourcePositions,"source positions");
    sameBuffer(a.sourceData->indices,b.sourceData->indices,"source corner IDs");
    sameBuffer(a.sourceData->originalPointIds,b.sourceData->originalPointIds,"original IDs");
    require(a.sourceData->indicesPending == b.sourceData->indicesPending
        && a.sourceData->provenance == b.sourceData->provenance,"source state");
    require(x.nodes.size() == y.nodes.size(),"group count");
    for (size_t i = 0; i < x.nodes.size(); ++i) {
        const auto& n = x.nodes[i]; const auto& m = y.nodes[i];
        require(n.name == m.name && n.hasTexcoords == m.hasTexcoords
            && n.indexOffset == m.indexOffset && n.indexCount == m.indexCount
            && n.lineIndexOffset == m.lineIndexOffset && n.lineIndexCount == m.lineIndexCount
            && n.pointIndexOffset == m.pointIndexOffset && n.pointIndexCount == m.pointIndexCount,"group fields");
    }
}
inline int verifyFile(const std::filesystem::path& path) {
    try {
        for (const bool deferred : {true,false}) {
            woby::ObjPrototypeOptions options; options.deferPreparation = deferred;
            woby::ObjPrototypeMetrics a, b;
            forcePositionFaces = false;
            const auto normal = woby::loadObjMeshPrototype(path,{},options,&a);
            forcePositionFaces = true;
            const auto staged = woby::loadObjMeshPrototype(path,{},options,&b);
            require(!a.mapping.staged && b.mapping.staged,"expected fallback and staged paths");
            sameMesh(normal,staged);
            std::cout << "All mesh buffers and group fields match; deferred=" << deferred << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
}
