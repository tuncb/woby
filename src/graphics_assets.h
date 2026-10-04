#pragma once
#include <array>
namespace woby
{
inline constexpr std::array nativeShaderNames{
    "vs_mesh",
    "fs_mesh",
    "vs_mesh_edges",
    "vs_triangle_lines",
    "fs_mesh_edges",
    "fs_mesh_edges_pulled",
    "fs_marker_mesh_edges",
    "fs_marker_mesh_edges_pulled",
    "vs_color",
    "fs_color",
    "vs_annotation",
    "vs_point_sprite",
    "vs_line_sprite",
    "fs_point_sprite",
    "vs_comparison",
    "fs_comparison",
    "vs_imgui",
    "fs_imgui",
    "vs_marker_point",
    "fs_marker_point",
    "fs_marker_mesh",
    "fs_marker_line",
    "fs_marker_comparison",
    "vs_marker_screen",
    "fs_marker_composite",
    "vs_marker_highlight",
    "fs_marker_highlight_single",
    "fs_marker_highlight_msaa",
    "cs_marker_lookup_single",
    "cs_marker_lookup_msaa",
    "cs_freeform",
};
}
