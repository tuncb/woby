// Source-order primary reads scatter to the unchanged global first-use IDs.
// No primary tuple is copied into a separate ordered-key array.
template <bool Prototype>
Vertex gatherCompactVertex(const IndexKey& key, const Coordinate* points,
    const Coordinate* preciseNormals, const std::array<double, 2>* preciseUvs,
    const float* floatNormals, const float* floatUvs)
{
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
    return vertex;
}

template <bool Prototype, bool Primary>
__declspec(noinline) void gatherCompactPass(MeshGeometry& geometry, const VertexIndexTable& table,
    const std::vector<Coordinate>& positions, const rapidobj::GeometryBuffers* buffers,
    const rapidobj::Attributes& attrib, const ModelLoadProgressCallback& progress, size_t totalIndices)
{
    auto phase = corner_mapping::begin(Primary ? "mapping_primary_gather" : "mapping_seam_gather");
    const auto count = Primary ? table.entries.size() : table.keys.size();
    constexpr size_t chunkSize = 65536;
    const auto chunks = (count + chunkSize - 1) / chunkSize;
    const auto workers = std::max(size_t{1}, std::min(corner_mapping::gatherWorkers, chunks));
    std::atomic<size_t> nextChunk = 0, completed = 0, normalsRead = 0, uvsRead = 0;
    std::atomic<bool> canceled = false;
    std::vector<std::exception_ptr> errors(workers);
    auto* output = geometry.vertices.data();
    const auto* entries = table.entries.data();
    const auto* seams = table.keys.data();
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
                const auto begin = chunk * chunkSize, end = std::min(count, begin + chunkSize);
                // Worker zero runs on the caller. Other workers never call UI code.
                if (worker == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices); }
                size_t written = 0, normalCount = 0, uvCount = 0;
                for (size_t i = begin; i < end; ++i) {
                    IndexKey key;
                    uint32_t tagged;
                    if constexpr (Primary) {
                        const auto& entry = entries[i];
                        tagged = entry.id;
                        if (tagged == emptyBucket) { continue; }
                        key = {static_cast<int>(i), entry.normal, entry.texcoord};
                    } else {
                        const auto& seam = seams[i];
                        tagged = seam.taggedId;
                        key = seam.key;
                    }
                    output[tagged & renderIdMask] = gatherCompactVertex<Prototype>(key, points,
                        preciseNormals, preciseUvs, floatNormals, floatUvs);
                    ++written;
                    normalCount += key.normal >= 0 ? size_t{1} : size_t{0};
                    uvCount += key.texcoord >= 0 ? size_t{1} : size_t{0};
                }
                completed.fetch_add(written, std::memory_order_relaxed);
                normalsRead.fetch_add(normalCount, std::memory_order_relaxed);
                uvsRead.fetch_add(uvCount, std::memory_order_relaxed);
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
    const auto written = completed.load();
    const auto metadataBytes = Primary ? count * sizeof(uint32_t) + written * 2 * sizeof(int)
        : count * sizeof(SecondaryEntry);
    obj_profile::end(phase, {{"workers", workers}, {"scanned_records", count}, {"vertices", written},
        {"metadata_read_bytes", metadataBytes}, {"source_position_read_bytes", written * sizeof(Coordinate)},
        {"normal_read_bytes", normalsRead.load() * (Prototype ? sizeof(Coordinate) : 3 * sizeof(float))},
        {"uv_read_bytes", uvsRead.load() * (Prototype ? sizeof(std::array<double, 2>) : 2 * sizeof(float))},
        {"vertex_write_bytes", written * sizeof(Vertex)},
        {"extra_worker_slots_bytes", errors.capacity() * sizeof(std::exception_ptr) + threads.capacity() * sizeof(std::jthread)}});
}

template <bool Prototype>
__declspec(noinline) void gatherResolvedVertices(MeshGeometry& geometry, const VertexIndexTable& table,
    const std::vector<Coordinate>& positions, const rapidobj::GeometryBuffers* buffers,
    const rapidobj::Attributes& attrib, const ModelLoadProgressCallback& progress, size_t totalIndices)
{
    auto phase = corner_mapping::begin("mapping_vertex_allocate");
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices);
    geometry.vertices.resize(table.next);
    obj_profile::end(phase, {{"vertices", table.next}, {"initialized_bytes", size_t(table.next) * sizeof(Vertex)}});

    phase = corner_mapping::begin("mapping_position_identity");
    bool identity = table.keys.empty();
    size_t scanned = 0;
    if (identity) {
        for (size_t i = 0; i < table.entries.size(); ++i) {
            if (i % 65536 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices); }
            ++scanned;
            const auto tagged = table.entries[i].id;
            if (tagged != emptyBucket && static_cast<size_t>(tagged & renderIdMask) != i) {
                identity = false;
                break;
            }
        }
    }
    obj_profile::end(phase, {{"identity", identity}, {"primary_id_read_bytes", scanned * sizeof(uint32_t)}});

    phase = corner_mapping::begin("mapping_position_indices");
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices);
    if (!identity) {
        geometry.positionIndexing = PositionIndexing::mapped;
        geometry.positionIndices.resize(table.next);
        for (size_t i = 0; i < table.entries.size(); ++i) {
            if (i % 65536 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices); }
            const auto tagged = table.entries[i].id;
            if (tagged != emptyBucket) { geometry.positionIndices[tagged & renderIdMask] = static_cast<uint32_t>(i); }
        }
        for (size_t i = 0; i < table.keys.size(); ++i) {
            if (i % 65536 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices); }
            const auto& seam = table.keys[i];
            geometry.positionIndices[seam.taggedId & renderIdMask] = static_cast<uint32_t>(seam.key.vertex);
        }
    }
    obj_profile::end(phase, {{"identity", identity},
        {"initialized_bytes", geometry.positionIndices.size() * sizeof(uint32_t)},
        {"primary_id_read_bytes", identity ? size_t{0} : table.entries.size() * sizeof(uint32_t)},
        {"seam_position_read_bytes", identity ? size_t{0} : table.keys.size() * (sizeof(uint32_t) + sizeof(int))},
        {"written_bytes", geometry.positionIndices.size() * sizeof(uint32_t)}});

    gatherCompactPass<Prototype, true>(geometry, table, positions, buffers, attrib, progress, totalIndices);
    gatherCompactPass<Prototype, false>(geometry, table, positions, buffers, attrib, progress, totalIndices);
    corner_mapping::currentPass = {};
}
