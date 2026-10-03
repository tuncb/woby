#pragma once
#include "trace.h"
#include "ui_state.h"
#include "graphics.h"
#include <filesystem>

namespace mesh_lab {
struct GpuCapture {
    woby::graphics::VertexBufferHandle vertices;
    woby::graphics::IndexBufferHandle indices;
    std::vector<uint8_t> vertexUpload, indexUpload, vertexReadback, indexReadback;
    uint32_t readyFrame = 0;
    bool complete = false, matches = false;
};
struct Viewport {
    woby::graphics::TextureHandle color, depth;
    woby::graphics::FrameBufferHandle target;
    woby::graphics::ProgramHandle program, presentation;
    woby::graphics::UniformHandle colorUniform;
    uint16_t width = 0, height = 0;
};
struct ViewMatrices {
    std::array<float, 16> view{}, projection{};
};
[[nodiscard]] ViewMatrices viewMatrices(const Trace& trace, const UiState& state, float aspect, bool homogeneousDepth);
[[nodiscard]] std::array<float, 2> projectVertex(const ViewMatrices& matrices, const woby::Vertex& vertex);
void upload(GpuCapture& gpu, const Trace& trace);
void pollReadback(GpuCapture& gpu, uint32_t frame);
void destroy(GpuCapture& gpu);
void initViewport(Viewport& view, const std::filesystem::path& assets);
void resizeViewport(Viewport& view, uint16_t width, uint16_t height);
void renderViewport(const Viewport& view, const GpuCapture& gpu, const Trace& trace, const UiState& state);
void destroy(Viewport& view);
} // namespace mesh_lab
