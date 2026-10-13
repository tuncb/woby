template <bool Prototype, bool UniqueUvs>
__declspec(noinline) void resolveFastFaces(const std::vector<rapidobj::Shape>& shapes,
    VertexIndexTable& table, MeshGeometry& geometry, SourceMeshData& source,
    const rapidobj::GeometryBuffers* buffers, const rapidobj::Attributes& attrib,
    const ModelLoadProgressCallback& progress, size_t totalIndices, bool deferPreparation)
{
    auto phase = corner_mapping::begin("mapping_render_index_allocate");
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, 0, totalIndices);
    size_t corners = 0;
    for (const auto& shape : shapes) { corners += shape.mesh.indices.size(); }
    geometry.indices.resize(corners);
    if (!deferPreparation) { source.indices.resize(corners); }
    obj_profile::end(phase, {{"initialized_bytes", (geometry.indices.size() + source.indices.size()) * sizeof(uint32_t)}});
    phase = corner_mapping::begin("mapping_resolve");
    const auto positionCount = sourcePositions(source).size();
    auto* renderIndices = geometry.indices.data();
    auto* sourceIndices = source.indices.data();
    const auto* preciseUvs = Prototype ? buffers->texcoords.data() : nullptr;
    const auto* floatUvs = attrib.texcoords.data();
    auto* entries = table.entries.data();
    size_t completed = 0;
    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto& shape = shapes[shapeIndex];
        const auto offset = static_cast<uint32_t>(completed);
        bool hasTexcoords = true;
        for (const auto& index : shape.mesh.indices) {
            if ((completed & 16383) == 0) {
                reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, completed, totalIndices);
            }
            if (index.position_index < 0 || static_cast<size_t>(index.position_index) >= positionCount) {
                throw std::runtime_error("OBJ contains an invalid source position index.");
            }
            const IndexKey key{index.position_index, index.normal_index, index.texcoord_index};
            auto& entry = entries[static_cast<size_t>(index.position_index)];
            uint32_t tagged;
            if (entry.id == emptyBucket) {
                (void)sceneBufferBytes(size_t(table.next) + 1, sizeof(Vertex));
                tagged = table.next++;
                if constexpr (UniqueUvs) {
                    bool valid = false;
                    if (key.texcoord >= 0) {
                        const auto uv = static_cast<size_t>(key.texcoord);
                        const auto u = Prototype ? static_cast<float>(preciseUvs[uv][0]) : floatUvs[uv * 2];
                        const auto v = Prototype ? static_cast<float>(preciseUvs[uv][1]) : floatUvs[uv * 2 + 1];
                        valid = std::isfinite(u) && std::isfinite(v);
                    }
                    if (!valid) { tagged |= invalidUvBit; }
                }
                entry.id = tagged;
                entry.normal = key.normal;
                entry.texcoord = key.texcoord;
            } else if (entry.normal == key.normal && entry.texcoord == key.texcoord) {
                tagged = entry.id;
            } else { tagged = vertexIndex(table, key); }
            if constexpr (UniqueUvs) { hasTexcoords = hasTexcoords && !(tagged & invalidUvBit); }
            else {
                hasTexcoords = hasTexcoords && index.texcoord_index >= 0;
                if (hasTexcoords) {
                    const auto uv = static_cast<size_t>(index.texcoord_index);
                    const auto u = Prototype ? static_cast<float>(preciseUvs[uv][0]) : floatUvs[uv * 2];
                    const auto v = Prototype ? static_cast<float>(preciseUvs[uv][1]) : floatUvs[uv * 2 + 1];
                    hasTexcoords = std::isfinite(u) && std::isfinite(v);
                }
            }
            renderIndices[completed] = tagged & renderIdMask;
            if (!deferPreparation) { sourceIndices[completed] = static_cast<uint32_t>(index.position_index); }
            ++completed;
        }
        const auto count = static_cast<uint32_t>(completed) - offset;
        if (count) {
            MeshNode node;
            node.hasTexcoords = hasTexcoords;
            node.name = shape.name.empty() ? "shape " + std::to_string(shapeIndex + 1) : shape.name;
            node.indexOffset = offset;
            node.indexCount = count;
            geometry.nodes.push_back(std::move(node));
        }
    }
    obj_profile::end(phase, {{"corners", completed}, {"vertices", table.next},
        {"render_index_write_bytes", completed * sizeof(uint32_t)},
        {"source_index_write_bytes", source.indices.size() * sizeof(uint32_t)},
        {"unique_uv_validation", UniqueUvs}, {"primary_hit_inline", true}});
}
template <bool Prototype>
__declspec(noinline) void resolveSeparateFaces(const std::vector<rapidobj::Shape>& shapes,
    VertexIndexTable& table, MeshGeometry& geometry, SourceMeshData& source,
    const rapidobj::GeometryBuffers* buffers, const rapidobj::Attributes& attrib,
    const ModelLoadProgressCallback& progress, size_t totalIndices, bool deferPreparation)
{
    if (corner_mapping::validateUniqueUvs) {
        resolveFastFaces<Prototype, true>(shapes, table, geometry, source, buffers, attrib, progress, totalIndices, deferPreparation);
    } else {
        resolveFastFaces<Prototype, false>(shapes, table, geometry, source, buffers, attrib, progress, totalIndices, deferPreparation);
    }
}
