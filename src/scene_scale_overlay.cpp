#include "scene_scale_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace woby {
namespace {

constexpr ImU32 ink = IM_COL32(250, 221, 135, 255);
constexpr ImU32 backdrop = IM_COL32(24, 28, 34, 235);

struct LabelRect { ImVec2 low, high; };
bool overlap(const LabelRect& a, const LabelRect& b)
{
    return a.low.x < b.high.x && a.high.x > b.low.x && a.low.y < b.high.y && a.high.y > b.low.y;
}

} // namespace

void drawSceneScaleOverlay(ImDrawList& draw, const UiState& state,
    const std::optional<SceneDimensions>& dimensions, const ScenePickView& view,
    ImVec2 origin, float pixelScale, float fontSize)
{
    const float width = static_cast<float>(view.width) * pixelScale;
    const float height = static_cast<float>(view.height) * pixelScale;
    const float pad = fontSize * .4f;
    if (width < fontSize * 6 || height < fontSize * 5) { return; }
    const ImVec2 low{origin.x + pad, origin.y + pad};
    const ImVec2 high{origin.x + width - pad, origin.y + height - pad};
    draw.PushClipRect(origin, {origin.x + width, origin.y + height}, true);
    std::vector<LabelRect> labels;
    const auto textSize = [&](const char* text) {
        return ImGui::GetFont()->CalcTextSizeA(fontSize, 100000, 0, text);
    };
    const auto label = [&](const char* text, ImVec2 center, ImU32 color) {
        const auto size = textSize(text);
        if (size.x + pad * 2 > high.x - low.x || size.y + pad * 2 > high.y - low.y) { return false; }
        const float x = std::clamp(center.x - size.x * .5f, low.x + pad, high.x - size.x - pad);
        const float y = std::clamp(center.y - size.y * .5f, low.y + pad, high.y - size.y - pad);
        const LabelRect rect{{x - pad, y - pad}, {x + size.x + pad, y + size.y + pad}};
        if (std::any_of(labels.begin(), labels.end(), [&](const auto& other) { return overlap(rect, other); })) { return false; }
        labels.push_back(rect);
        draw.AddRectFilled(rect.low, rect.high, backdrop, pad * .5f);
        draw.AddText(ImGui::GetFont(), fontSize, {x, y}, color, text);
        return true;
    };
    const auto project = [&](const DimensionPoint& point) -> std::optional<ImVec2> {
        const auto p = projectDimensionPoint(point, view);
        if (!p) { return {}; }
        return ImVec2{origin.x + (*p)[0] * pixelScale, origin.y + (*p)[1] * pixelScale};
    };

    // A compact readout keeps edge-on, zero-length, and off-screen dimensions
    // available when their projected dimension lines cannot be shown.
    float baseline = high.y - fontSize;
    if (state.showGrid) {
        char text[96];
        const double spacing = sceneGrid(state.sceneBounds, state.upAxis).spacing;
        std::snprintf(text, sizeof(text), "Grid spacing: %.6g", spacing);
        if (textSize(text).x + pad * 4 > width) {
            std::snprintf(text, sizeof(text), "Grid: %.6g", spacing);
        }
        const auto size = textSize(text);
        label(text, {low.x + pad + size.x * .5f, baseline}, IM_COL32(221, 226, 234, 255));
        baseline -= fontSize + pad * 3;
    }
    if (state.showDimensions && dimensions) {
        char text[192];
        const auto& d = *dimensions;
        std::snprintf(text, sizeof(text), "X: %.6g   Y: %.6g   Z: %.6g",
            d.lengths[0], d.lengths[1], d.lengths[2]);
        if (textSize(text).x + pad * 4 <= width) {
            label(text, {low.x + pad + textSize(text).x * .5f, baseline}, ink);
        } else {
            // Stack at narrow viewport widths and large UI scales.
            for (int axis = 2; axis >= 0; --axis) {
                std::snprintf(text, sizeof(text), "%c: %.6g", 'X' + axis, d.lengths[static_cast<size_t>(axis)]);
                label(text, {low.x + pad + textSize(text).x * .5f, baseline}, ink);
                baseline -= fontSize + pad * 3;
            }
        }

        DimensionPoint center{};
        for (size_t axis = 0; axis < 3; ++axis) { center[axis] = (d.corners[0][axis] + d.corners[7][axis]) * .5; }
        const auto projectedCenter = project(center);
        if (projectedCenter) {
            for (size_t axis = 0; axis < 3; ++axis) {
                float bestScore = -1;
                ImVec2 a{}, b{}, offset{};
                for (size_t corner = 0; corner < 8; ++corner) {
                    if ((corner & (size_t{1} << axis)) != 0) { continue; }
                    const auto start = project(d.corners[corner]);
                    const auto end = project(d.corners[corner | (size_t{1} << axis)]);
                    if (!start || !end) { continue; }
                    const float dx = end->x - start->x, dy = end->y - start->y;
                    const float length = std::hypot(dx, dy);
                    if (length < fontSize * 2) { continue; }
                    ImVec2 normal{-dy / length, dx / length};
                    const ImVec2 midpoint{(start->x + end->x) * .5f, (start->y + end->y) * .5f};
                    float outward = (midpoint.x - projectedCenter->x) * normal.x + (midpoint.y - projectedCenter->y) * normal.y;
                    if (outward < 0) { normal.x = -normal.x; normal.y = -normal.y; outward = -outward; }
                    // Choose the outer silhouette edge, preferring usable length.
                    const float score = outward + length * .01f;
                    if (score > bestScore) { bestScore = score; a = *start; b = *end; offset = normal; }
                }
                if (bestScore < 0) { continue; }
                const float gap = fontSize * 1.5f;
                const ImVec2 da{a.x + offset.x * gap, a.y + offset.y * gap};
                const ImVec2 db{b.x + offset.x * gap, b.y + offset.y * gap};
                std::snprintf(text, sizeof(text), "%c: %.6g", static_cast<int>('X' + axis), d.lengths[axis]);
                if (!label(text, {(da.x + db.x) * .5f + offset.x * fontSize,
                        (da.y + db.y) * .5f + offset.y * fontSize}, ink)) { continue; }
                const float tick = fontSize * .25f;
                draw.AddLine(a, da, IM_COL32(250, 221, 135, 130));
                draw.AddLine(b, db, IM_COL32(250, 221, 135, 130));
                draw.AddLine(da, db, ink);
                for (const auto& p : {da, db}) {
                    draw.AddLine({p.x - offset.x * tick, p.y - offset.y * tick},
                        {p.x + offset.x * tick, p.y + offset.y * tick}, ink);
                }
            }
        }
    }
    draw.PopClipRect();
}

} // namespace woby
