#pragma once

#include "graphics.h"
#include <imgui.h>

#include <filesystem>

namespace woby::imgui_graphics {

void init(const std::filesystem::path& assetRoot, woby::graphics::ViewId viewId);
void shutdown();
void render(ImDrawData* drawData);
// Render export annotations into a caller-owned view/framebuffer; fail if incomplete.
void renderToView(ImDrawData* drawData, woby::graphics::ViewId viewId);

} // namespace woby::imgui_graphics
