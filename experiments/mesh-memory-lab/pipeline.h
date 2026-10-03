#pragma once
#include "trace.h"
#include <array>

namespace mesh_lab {
enum class Node { source, parse, attributes, triangulate, corners, pack, mesh, upload, gpu, count };
struct PipelineNode {
    Node id;
    const char* key;
    const char* title;
    const char* caption;
    bool transformer;
};
inline constexpr std::array<PipelineNode, 9> pipeline = {{
    {Node::source, "source", "OBJ text", "char[] / embedded source", false},
    {Node::parse, "parse", "Parse", "text -> typed values", true},
    {Node::attributes, "attributes", "Attribute pools", "independent index domains", false},
    {Node::triangulate, "triangulate", "Triangulate", "rebase / split polygons", true},
    {Node::corners, "corners", "Corner stream", "triangle list / p,n,uv", false},
    {Node::pack, "pack", "Build mesh", "intern / convert / pack", true},
    {Node::mesh, "mesh", "CPU mesh", "AoS vertices + indices", false},
    {Node::upload, "upload", "Upload", "copy / submit / retain", true},
    {Node::gpu, "gpu", "GPU buffers", "indexed vertex fetch", false},
}};
[[nodiscard]] size_t payloadBytes(Node node, const Trace& trace);
[[nodiscard]] std::array<Node, 2> transformationEndpoints(Node node);
} // namespace mesh_lab
