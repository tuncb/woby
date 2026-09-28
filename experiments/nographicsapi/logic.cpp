#include "logic.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace woby::ng {
const char* scenarioName(Scenario scenario)
{
    switch (scenario) {
    case Scenario::overlapping: return "Overlapping markers / exact 32-bit IDs";
    case Scenario::translucent: return "Translucent front marker / independent blending";
    case Scenario::hidden: return "Hidden front marker / rear selection";
    case Scenario::zeroOpacity: return "Zero opacity / no selectable fragments";
    case Scenario::occluded: return "Translucent surface / marker occlusion";
    default: return "Unknown";
    }
}
uint32_t expectedPick(Scenario scenario)
{
    switch (scenario) {
    case Scenario::overlapping: case Scenario::translucent: return frontId;
    case Scenario::hidden: return rearId;
    case Scenario::zeroOpacity: case Scenario::occluded: return 0;
    default: throw std::invalid_argument("Unknown prototype scenario");
    }
}
Fixture makeFixture(Scenario scenario)
{
    Fixture result;
    result.scenario = scenario;
    result.state.masterVertexPointSize = 27.0f;
    // Fixed test scenes are built at a load boundary; UI only selects scenarios.
    // Geometry uses clip-space coordinates to make GPU expectations exact.
    for (size_t file = 0; file < 3; ++file) {
        UiFileState item;
        item.objectId = file + 1;
        item.path = file == 0 ? "rear.obj" : file == 1 ? "front.obj" : "occluder.obj";
        const float depth = file == 0 ? 0.7f : file == 1 ? 0.4f : 0.2f;
        item.mesh.vertices = {
            {{0.0f, 0.0f, depth}, {0,0,1}, {}},
            {{-0.63f, -0.55f, depth}, {0,0,1}, {}},
            {{0.67f, -0.48f, depth}, {0,0,1}, {}},
            {{0.13f, 0.68f, depth}, {0,0,1}, {}},
        };
        item.mesh.indices = {1,2,3};
        item.mesh.nodes = {{"fixture", 0, 3}};
        UiGroupState group;
        group.objectId = file + 10;
        group.showSolidMesh = file == 2;
        group.showVertices = file < 2;
        group.showTriangles = file == 0;
        group.color = file == 0 ? std::array<float,4>{0.12f,0.65f,0.94f,1} : std::array<float,4>{0.95f,0.35f,0.22f,1};
        if (file == 1) {
            group.visible = scenario != Scenario::hidden && scenario != Scenario::zeroOpacity;
            group.opacity = scenario == Scenario::translucent ? 0.35f : 1.0f;
        }
        if (file == 0 && scenario == Scenario::zeroOpacity) group.opacity = 0;
        if (file == 2) { group.visible = scenario == Scenario::occluded; group.opacity = 0.45f; }
        item.groupSettings.push_back(group);
        result.state.files.push_back(std::move(item));
        if (file < 2) {
            result.markers.nextId = file == 0 ? rearId : frontId;
            MarkerDraw draw;
            draw.model = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            draw.count = 4;
            draw.fileIndex = file;
            draw.fileId = file + 1;
            if (!appendMarkerDraw(result.markers, draw, result.state.masterVertexPointSize))
                throw std::runtime_error("Fixture ID allocation failed");
        }
    }
    return result;
}
std::optional<Pixel> cursorPixel(float x, float y, float scaleX, float scaleY, const SceneViewport& viewport)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(scaleX) || !std::isfinite(scaleY)
        || scaleX <= 0 || scaleY <= 0) return {};
    const double px = std::floor(double(x) * scaleX), py = std::floor(double(y) * scaleY);
    if (px < viewport.x || py < viewport.y || px >= double(viewport.x) + viewport.width
        || py >= double(viewport.y) + viewport.height) return {};
    return Pixel{static_cast<int>(px - viewport.x), static_cast<int>(py - viewport.y)};
}
Clip clipRect(float x1, float y1, float x2, float y2, float originX, float originY,
    float scaleX, float scaleY, uint32_t width, uint32_t height)
{
    const float values[] = {x1,y1,x2,y2,originX,originY,scaleX,scaleY};
    for (const float value : values) if (!std::isfinite(value)) return {};
    if (scaleX <= 0 || scaleY <= 0) return {};
    const int left = static_cast<int>(std::clamp(std::floor((x1-originX)*scaleX), 0.0f, float(width)));
    const int top = static_cast<int>(std::clamp(std::floor((y1-originY)*scaleY), 0.0f, float(height)));
    const int right = static_cast<int>(std::clamp(std::ceil((x2-originX)*scaleX), 0.0f, float(width)));
    const int bottom = static_cast<int>(std::clamp(std::ceil((y2-originY)*scaleY), 0.0f, float(height)));
    return {left,top,static_cast<uint32_t>(std::max(0,right-left)),static_cast<uint32_t>(std::max(0,bottom-top))};
}
bool completePick(Selection& selection, uint64_t epoch, uint64_t sequence, uint32_t id)
{
    if (!acceptMarkerCompletion(epoch, selection.epoch, sequence, selection.sequence)) {
        ++selection.rejected;
        return false;
    }
    selection.sequence = sequence;
    selection.id = id;
    ++selection.accepted;
    return true;
}
void invalidateSelection(Selection& selection) { ++selection.epoch; selection.id = 0; }
}
