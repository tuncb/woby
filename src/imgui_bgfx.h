#pragma once

#include <bgfx/bgfx.h>
#include <imgui.h>

#include <filesystem>

namespace woby::imgui_bgfx {

void init(const std::filesystem::path& assetRoot, bgfx::ViewId viewId);
void shutdown();
void render(ImDrawData* drawData);

} // namespace woby::imgui_bgfx
