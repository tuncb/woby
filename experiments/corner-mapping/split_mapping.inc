// Attribute gathering has a fixed function boundary. All worker counts use
// the same compiled serial resolver and gather code.
template <bool Prototype>
__declspec(noinline) void gatherResolvedVertices(MeshGeometry& geometry, const std::vector<IndexKey>& keys,
    const std::vector<Coordinate>& positions, const rapidobj::GeometryBuffers* buffers,
    const rapidobj::Attributes& attrib, const ModelLoadProgressCallback& progress, size_t totalIndices)
{
    auto phase = corner_mapping::begin("mapping_vertex_allocate");
    geometry.vertices.resize(keys.size());
    obj_profile::end(phase, {{"vertices", keys.size()}, {"initialized_bytes", keys.size() * sizeof(Vertex)}});

    phase = corner_mapping::begin("mapping_position_indices");
    bool identity = true;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i % 65536 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices); }
        if (static_cast<size_t>(keys[i].vertex) != i) { identity = false; break; }
    }
    if (!identity) {
        geometry.positionIndexing = PositionIndexing::mapped;
        geometry.positionIndices.resize(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i % 65536 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices); }
            geometry.positionIndices[i] = static_cast<uint32_t>(keys[i].vertex);
        }
    }
    obj_profile::end(phase, {{"identity", identity},
        {"initialized_bytes", geometry.positionIndices.size() * sizeof(uint32_t)},
        {"key_scan_bytes_upper_bound", keys.size() * sizeof(IndexKey)},
        {"written_bytes", geometry.positionIndices.size() * sizeof(uint32_t)}});

    phase = corner_mapping::begin("mapping_attribute_gather");
    constexpr size_t chunkSize = 65536;
    const auto chunks = (keys.size() + chunkSize - 1) / chunkSize;
    const auto workers = std::max(size_t{1}, std::min(corner_mapping::gatherWorkers, chunks));
    std::atomic<size_t> nextChunk = 0, completed = 0;
    std::atomic<bool> canceled = false;
    std::vector<std::exception_ptr> errors(workers);
    auto* output = geometry.vertices.data();
    const auto* inputKeys = keys.data();
    const auto* points = positions.data();
    const auto* preciseNormals = Prototype ? buffers->normals.data() : nullptr;
    const auto* preciseUvs = Prototype ? buffers->texcoords.data() : nullptr;
    const auto* floatNormals = attrib.normals.data();
    const auto* floatUvs = attrib.texcoords.data();
    const auto work = [&](size_t worker) {
        try {
            while (!canceled.load(std::memory_order_relaxed)) {
                const auto chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
                if (chunk >= chunks) { break; }
                const auto begin = chunk * chunkSize, end = std::min(keys.size(), begin + chunkSize);
                if (worker == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices); }
                for (size_t i = begin; i < end; ++i) {
                    const auto key = inputKeys[i];
                    Vertex vertex{};
                    vertex.position = renderPosition(points[static_cast<size_t>(key.vertex)]);
                    if (key.normal >= 0) {
                        const auto normal = static_cast<size_t>(key.normal);
                        for (size_t axis = 0; axis < 3; ++axis) {
                            if constexpr (Prototype) { vertex.normal[axis] = static_cast<float>(preciseNormals[normal][axis]); }
                            else { vertex.normal[axis] = floatNormals[normal * 3 + axis]; }
                        }
                    }
                    if (key.texcoord >= 0) {
                        const auto uv = static_cast<size_t>(key.texcoord);
                        if constexpr (Prototype) {
                            vertex.texcoord = {static_cast<float>(preciseUvs[uv][0]), 1.0f - static_cast<float>(preciseUvs[uv][1])};
                        } else { vertex.texcoord = {floatUvs[uv * 2], 1.0f - floatUvs[uv * 2 + 1]}; }
                    }
                    output[i] = vertex;
                }
                completed.fetch_add(end - begin, std::memory_order_relaxed);
            }
        } catch (...) {
            errors[worker] = std::current_exception();
            canceled.store(true, std::memory_order_relaxed);
        }
    };
    std::vector<std::jthread> threads;
    threads.reserve(workers - 1);
    try {
        for (size_t worker = 1; worker < workers; ++worker) { threads.emplace_back(work, worker); }
    } catch (...) {
        canceled.store(true, std::memory_order_relaxed);
        throw;
    }
    work(0);
    for (auto& thread : threads) { thread.join(); }
    for (const auto& error : errors) { if (error) { std::rethrow_exception(error); } }
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices);
    obj_profile::end(phase, {{"workers", workers}, {"vertices", completed.load()},
        {"key_read_bytes", keys.size() * sizeof(IndexKey)}, {"vertex_write_bytes", keys.size() * sizeof(Vertex)},
        {"extra_worker_slots_bytes", errors.capacity() * sizeof(std::exception_ptr) + threads.capacity() * sizeof(std::jthread)}});
    corner_mapping::currentPass = {};
}

template <bool Prototype>
__declspec(noinline) void resolveSeparateFaces(const std::vector<rapidobj::Shape>& shapes,
    VertexIndexTable& table, MeshGeometry& geometry, SourceMeshData& source,
    const rapidobj::GeometryBuffers* buffers, const rapidobj::Attributes& attrib,
    const ModelLoadProgressCallback& progress, size_t totalIndices, bool deferPreparation)
{
    const auto phase = corner_mapping::begin("mapping_resolve");
    const auto positionCount = sourcePositions(source).size();
    const auto* preciseUvs = Prototype ? buffers->texcoords.data() : nullptr;
    const auto* floatUvs = attrib.texcoords.data();
    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto& shape = shapes[shapeIndex];
        const auto offset = static_cast<uint32_t>(geometry.indices.size());
        bool hasTexcoords = true;
        for (const auto& index : shape.mesh.indices) {
            if (geometry.indices.size() % 16384 == 0) {
                reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, geometry.indices.size(), totalIndices);
            }
            if (index.position_index < 0 || static_cast<size_t>(index.position_index) >= positionCount) {
                throw std::runtime_error("OBJ contains an invalid source position index.");
            }
            if (!deferPreparation) { source.indices.push_back(static_cast<uint32_t>(index.position_index)); }
            const auto tagged = vertexIndex(table, {index.position_index, index.normal_index, index.texcoord_index});
            if (corner_mapping::validateUniqueUvs) { hasTexcoords = hasTexcoords && !(tagged & invalidUvBit); }
            else {
                hasTexcoords = hasTexcoords && index.texcoord_index >= 0;
                if (hasTexcoords) {
                    const auto uv = static_cast<size_t>(index.texcoord_index);
                    const auto u = Prototype ? static_cast<float>(preciseUvs[uv][0]) : floatUvs[uv * 2];
                    const auto v = Prototype ? static_cast<float>(preciseUvs[uv][1]) : floatUvs[uv * 2 + 1];
                    hasTexcoords = std::isfinite(u) && std::isfinite(v);
                }
            }
            geometry.indices.push_back(tagged & renderIdMask);
        }
        const auto count = static_cast<uint32_t>(geometry.indices.size()) - offset;
        if (count) {
            MeshNode node;
            node.hasTexcoords = hasTexcoords;
            node.name = shape.name.empty() ? "shape " + std::to_string(shapeIndex + 1) : shape.name;
            node.indexOffset = offset;
            node.indexCount = count;
            geometry.nodes.push_back(std::move(node));
        }
    }
    obj_profile::end(phase, {{"corners", geometry.indices.size()}, {"vertices", table.keys.size()},
        {"render_index_write_bytes", geometry.indices.size() * sizeof(uint32_t)},
        {"source_index_write_bytes", source.indices.size() * sizeof(uint32_t)},
        {"unique_uv_validation", corner_mapping::validateUniqueUvs}});
}
