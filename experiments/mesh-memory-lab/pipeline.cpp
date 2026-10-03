#include "pipeline.h"
#include <stdexcept>

namespace mesh_lab {
size_t payloadBytes(Node node, const Trace& trace)
{
    const size_t attributes = trace.positions.size()*sizeof(woby::Coordinate)
        + trace.normals.size()*sizeof(std::array<float, 3>) + trace.texcoords.size()*sizeof(std::array<float, 2>);
    switch (node) {
    case Node::source: return trace.source.size();
    case Node::attributes: return attributes + trace.originalCorners*sizeof(Corner);
    case Node::corners: return attributes + trace.mesh.indices.size()*sizeof(Corner);
    case Node::mesh: return trace.mesh.vertices.size()*(sizeof(woby::Vertex)+sizeof(woby::Coordinate))
        + trace.mesh.indices.size()*sizeof(uint32_t)
        + trace.mesh.sourceData->points.size()*sizeof(woby::Coordinate)
        + trace.mesh.sourceData->indices.size()*sizeof(uint32_t);
    case Node::gpu: return trace.mesh.vertices.size()*sizeof(woby::Vertex) + trace.mesh.indices.size()*sizeof(uint32_t);
    default: throw std::invalid_argument("Only data nodes have a payload.");
    }
}
std::array<Node, 2> transformationEndpoints(Node node)
{
    const auto index = static_cast<size_t>(node);
    if (index >= pipeline.size() || !pipeline[index].transformer) {
        throw std::invalid_argument("Expected a transformer node.");
    }
    return {static_cast<Node>(index-1), static_cast<Node>(index+1)};
}
} // namespace mesh_lab
