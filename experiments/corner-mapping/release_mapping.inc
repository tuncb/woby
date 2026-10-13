template <typename Table>
void releaseResolvedTriangles(rapidobj::Result& result, Table& table,
    size_t triangleIndices, const ModelLoadProgressCallback& progress, size_t totalIndices)
{
    (void)table;
    const auto phase = corner_mapping::begin("mapping_input_release");
    const bool triangleOnly = triangleIndices != 0 && std::all_of(result.shapes.begin(), result.shapes.end(), [](const auto& shape) {
        return shape.lines.indices.empty() && shape.points.indices.empty();
    });
    size_t faceBytes = 0, primaryBytes = 0, bucketBytes = 0;
    if (triangleOnly) {
        for (auto& shape : result.shapes) {
            reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices);
            faceBytes += shape.mesh.indices.size() * sizeof(rapidobj::Index)
                + shape.mesh.num_face_vertices.size() * sizeof(uint8_t)
                + shape.mesh.material_ids.size() * sizeof(int32_t)
                + shape.mesh.smoothing_group_ids.size() * sizeof(uint32_t);
            shape.mesh = {};
        }
#ifndef CORNER_MAPPING_COMPACT
        primaryBytes = table.primary.capacity() * sizeof(uint32_t) + table.entries.capacity() * sizeof(PrimaryEntry);
        bucketBytes = table.buckets.capacity() * sizeof(uint32_t);
        decltype(table.primary)().swap(table.primary);
        decltype(table.entries)().swap(table.entries);
        decltype(table.buckets)().swap(table.buckets);
#endif
    }
    obj_profile::end(phase, {{"triangle_only", triangleOnly}, {"released_face_bytes", faceBytes},
        {"released_primary_bytes", primaryBytes}, {"released_bucket_bytes", bucketBytes}});
}
