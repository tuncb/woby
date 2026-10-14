// Reference resolver from application revision 7ee6133. Keep its loop unchanged.
#pragma once
#include "obj_corner_mapping.h"
namespace probe {
using namespace woby;
using namespace woby::obj_mapping;
inline constexpr auto emptyEntry = woby::obj_mapping::empty;
template<bool Precise>
inline void resolveOriginalFaces(const rapidobj::Result& result, Table& table,
    MeshGeometry& geometry, SourceMeshData& source, const rapidobj::GeometryBuffers* buffers,
    const ModelLoadProgressCallback& progress, size_t corners, bool deferred) {
    table.primary.resize(sourcePositions(source).size());
    table.seams.reserve(std::min({sourcePositions(source).size(),corners,size_t{1024}}));
    geometry.indices.resize(corners);
    if (!deferred) { source.indices.resize(corners); }
    auto* indices = geometry.indices.data(); auto* sourceIndices = source.indices.data();
    auto* primary = table.primary.data();
    const auto* preciseUvs = Precise ? buffers->texcoords.data() : nullptr;
    const auto* floatUvs = result.attributes.texcoords.data();
    size_t completed = 0;
    for (size_t shapeIndex = 0; shapeIndex < result.shapes.size(); ++shapeIndex) {
        const auto& shape = result.shapes[shapeIndex];
        const auto offset = static_cast<uint32_t>(completed);
        bool hasTexcoords = true;
        for (const auto& index : shape.mesh.indices) {
            if ((completed & 16383) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,completed,corners); }
            if (index.position_index < 0 || static_cast<size_t>(index.position_index) >= table.primary.size()) {
                throw std::runtime_error("OBJ contains an invalid source position index.");
            }
            const Key key{index.position_index,index.normal_index,index.texcoord_index};
            auto& entry = primary[static_cast<size_t>(key.position)];
            uint32_t id;
            if (entry.id == emptyEntry) {
                (void)sceneBufferBytes(size_t(table.next)+1,sizeof(Vertex));
                id = tagId<Precise>(key,table.next,preciseUvs,floatUvs);
                entry = {id,key.normal,key.uv}; ++table.next;
            } else if (entry.normal == key.normal && entry.uv == key.uv) { id = entry.id; }
            else { id = seamId<Precise>(table,key,preciseUvs,floatUvs); }
            hasTexcoords = hasTexcoords && !(id & invalidUv);
            indices[completed] = id & idMask;
            if (!deferred) { sourceIndices[completed] = static_cast<uint32_t>(key.position); }
            ++completed;
        }
        const auto count = static_cast<uint32_t>(completed)-offset;
        if (count) {
            MeshNode node; node.name = shape.name.empty() ? "shape "+std::to_string(shapeIndex+1) : shape.name;
            node.indexOffset = offset; node.indexCount = count; node.hasTexcoords = hasTexcoords;
            geometry.nodes.push_back(std::move(node));
        }
    }
}
}
