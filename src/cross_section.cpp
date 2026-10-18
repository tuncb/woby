#include "cross_section.h"
#include "surface_annotation.h"
#include <boost/unordered/unordered_flat_map.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>

namespace woby {
std::vector<bool> crossSectionSourceVisibility(const UiState& state, std::span<const SceneObjectId> sources)
{
    std::vector<bool> visible(sources.size(), false);
    boost::unordered_flat_map<SceneObjectId, size_t> indices;
    for (size_t i = 0; i < sources.size(); ++i) { indices.emplace(sources[i], i); }
    const auto group = [&](size_t fileIndex, size_t groupIndex, bool parentVisible) {
        if (fileIndex >= state.files.size()) { return; }
        const auto& file = state.files[fileIndex];
        if (groupIndex >= file.groupSettings.size()) { return; }
        const auto& settings = file.groupSettings[groupIndex];
        if (const auto it = indices.find(settings.objectId); it != indices.end()) {
            visible[it->second] = parentVisible && file.fileSettings.visible && settings.visible;
        }
    };
    const auto visit = [&](auto&& self, const UiSceneNode& node, bool parentVisible) -> void {
        if (node.kind == UiSceneNodeKind::group) {
            group(node.fileIndex, node.groupIndex, parentVisible);
            return;
        }
        if (node.kind == UiSceneNodeKind::folder) { parentVisible = parentVisible && node.settings.visible; }
        else {
            if (node.fileIndex >= state.files.size()) { return; }
            parentVisible = parentVisible && state.files[node.fileIndex].fileSettings.visible;
            if (node.children.empty()) {
                for (size_t g = 0; g < state.files[node.fileIndex].groupSettings.size(); ++g) { group(node.fileIndex, g, parentVisible); }
                return;
            }
        }
        for (const auto& child : node.children) { self(self, child, parentVisible); }
    };
    if (state.sceneNodes.empty()) {
        for (size_t f = 0; f < state.files.size(); ++f) {
            for (size_t g = 0; g < state.files[f].groupSettings.size(); ++g) { group(f, g, true); }
        }
    } else {
        for (const auto& node : state.sceneNodes) { visit(visit, node, true); }
    }
    return visible;
}
std::array<float, 3> snapSectionEndpoint(const std::array<float, 3>& anchor,
    const std::array<float, 3>& point, SceneUpAxis upAxis)
{
    const size_t horizontal = upAxis == SceneUpAxis::z ? 1 : 2;
    const double x = double(point[0]) - anchor[0], y = double(point[horizontal]) - anchor[horizontal];
    constexpr std::array<std::array<int, 2>, 8> directions{{{1,0}, {1,1}, {0,1}, {-1,1},
        {-1,0}, {-1,-1}, {0,-1}, {1,-1}}};
    const int octant = static_cast<int>(std::round(std::atan2(y, x) / (std::numbers::pi / 4)));
    const auto direction = directions[static_cast<size_t>((octant + 8) % 8)];
    const double length = (x * direction[0] + y * direction[1])
        / (direction[0] * direction[0] + direction[1] * direction[1]);
    auto result = anchor;
    result[0] += static_cast<float>(length * direction[0]);
    result[horizontal] += static_cast<float>(length * direction[1]);
    return result;
}
CrossSectionSettings normalizedCrossSectionSettings(CrossSectionSettings s, size_t sources)
{
    if (s.upAxis != SceneUpAxis::z && s.upAxis != SceneUpAxis::y) {
        throw std::runtime_error("Invalid section up axis.");
    }
    for (const auto p : {s.a, s.b}) {
        for (float v : p) {
            if (!std::isfinite(v)) {
                throw std::runtime_error("Section coordinates must be finite.");
            }
        }
    }
    const size_t up = s.upAxis == SceneUpAxis::z ? 2 : 1;
    s.b[up] = s.a[up];
    double length = 0;
    for (size_t k = 0; k < 3; ++k) {
        length += std::pow(double(s.b[k]) - s.a[k], 2);
    }
    if (length < 1e-16 || !std::isfinite(length)) {
        throw std::runtime_error("Draw a section line with two different endpoints.");
    }
    if (!std::isfinite(s.lift)) {
        throw std::runtime_error("Section lift must be finite.");
    }
    s.lift = std::max(0.0f, s.lift);
    s.sourceVisible.resize(sources, true);
    const int last = sources ? static_cast<int>(sources - 1) : 0;
    s.reference = std::clamp(s.reference, 0, last);
    s.compared = std::clamp(s.compared, 0, last);
    return s;
}
std::array<float, 3> sectionDirection(const CrossSectionSettings& s)
{
    std::array<float, 3> d{};
    double length = 0;
    for (size_t k = 0; k < 3; ++k) {
        d[k] = s.b[k] - s.a[k];
        length += double(d[k]) * d[k];
    }
    for (auto& v : d) {
        v = static_cast<float>(v / std::sqrt(length));
    }
    return d;
}
std::array<float, 3> sectionNormal(const CrossSectionSettings& s)
{
    const auto d = sectionDirection(s);
    return s.upAxis == SceneUpAxis::z ? std::array<float, 3>{-d[1], d[0], 0} : std::array<float, 3>{d[2], 0, -d[0]};
}
std::array<double, 2> sectionCoordinates(const CrossSectionSettings& s, const Coordinate& p)
{
    const auto d = sectionDirection(s);
    double x = 0;
    for (size_t k = 0; k < 3; ++k) {
        x += (p[k] - s.a[k]) * d[k];
    }
    return {x, p[s.upAxis == SceneUpAxis::z ? 2 : 1]};
}
std::array<float, 4> sectionSourceColor(size_t index)
{
    constexpr std::array<std::array<float, 4>, 6> colors{{{.2f, .8f, 1, 1},
                                                          {1, .6f, .2f, 1},
                                                          {.75f, .55f, 1, 1},
                                                          {.4f, .9f, .55f, 1},
                                                          {1, .4f, .65f, 1},
                                                          {.95f, .9f, .4f, 1}}};
    return colors[index % colors.size()];
}
std::vector<DiagnosticEdge> sectionGuides(const CrossSectionSettings& s)
{
    auto a = s.a, b = s.b;
    const size_t up = s.upAxis == SceneUpAxis::z ? 2 : 1;
    a[up] += s.lift;
    b[up] += s.lift;
    std::vector<DiagnosticEdge> lines{{s.a, s.b}};
    if (s.showPlane) {
        lines.push_back({a, b});
    }
    if (s.showConnections) {
        for (const auto& p : {s.a, s.b}) {
            for (int i = 0; i < 16; i += 2) {
                auto c = p, d = p;
                c[up] += s.lift * static_cast<float>(i) / 16;
                d[up] += s.lift * static_cast<float>(i + 1) / 16;
                lines.push_back({c, d});
            }
        }
    }
    return lines;
}
std::optional<std::array<float, 3>> sectionWorkplanePoint(const ScenePickView& view, PickPoint point, SceneUpAxis up,
                                                          float height)
{
    const auto vp = annotationCompose(view.view, view.projection);
    PickMatrix inverse{};
    inverseMatrix(inverse.data(), vp.data());
    const auto ndc = annotationNdc(view, point);
    auto a = annotationTransform(inverse, {ndc[0], ndc[1], view.homogeneousDepth ? -1.0f : 0.0f, 1});
    auto b = annotationTransform(inverse, {ndc[0], ndc[1], .5f, 1});
    if (std::abs(a[3]) < 1e-20f || std::abs(b[3]) < 1e-20f) {
        return {};
    }
    for (size_t k = 0; k < 3; ++k) {
        a[k] /= a[3];
        b[k] /= b[3];
    }
    const size_t axis = up == SceneUpAxis::z ? 2 : 1;
    const double delta = double(b[axis]) - a[axis];
    if (std::abs(delta) < 1e-10) {
        return {};
    }
    const double t = (height - double(a[axis])) / delta;
    if (t < 0) {
        return {};
    }
    std::array<float, 3> p{};
    for (size_t k = 0; k < 3; ++k) {
        p[k] = static_cast<float>(a[k] + t * (double(b[k]) - a[k]));
        if (!std::isfinite(p[k])) {
            return {};
        }
    }
    p[axis] = height;
    return p;
}
CrossSectionResult cutCrossSection(const CrossSectionSettings& input, std::span<const SceneObjectId> sources,
                                   std::span<const ScenePickPart> parts, std::stop_token stop)
{
    const auto s = normalizedCrossSectionSettings(input, sources.size());
    const auto n = sectionNormal(s);
    CrossSectionResult result;
    using Key = std::array<double, 6>;
    for (auto id : sources) {
        CrossSectionContour contour;
        contour.source = id;
        const auto part =
            std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == id && p.mesh; });
        if (part == parts.end()) {
            contour.missing = true;
            result.contours.push_back(std::move(contour));
            continue;
        }
        const auto& mesh = meshGeometry(*part->mesh);
        CoordinateMatrix model;
        if (part->coordinateModel) { model = *part->coordinateModel; }
        else { std::copy(part->model.begin(), part->model.end(), model.begin()); }
        std::set<Key> edges;
        std::map<Key, size_t> coplanar;
        const auto key = [](Coordinate a, Coordinate b) {
            if (b < a) {
                std::swap(a, b);
            }
            return Key{a[0], a[1], a[2], b[0], b[1], b[2]};
        };
        const size_t end = std::min(mesh.indices.size(), part->indexOffset + part->indexCount);
        for (size_t i = part->indexOffset; i + 2 < end; i += 3) {
            if ((i & 1023u) == 0 && stop.stop_requested()) {
                return {};
            }
            std::array<Coordinate, 3> p{};
            std::array<double, 3> d{};
            bool valid = true;
            for (size_t j = 0; j < 3; ++j) {
                const auto index = mesh.indices[i + j];
                if (index >= mesh.vertices.size()) {
                    valid = false;
                    break;
                }
                const auto w = transformCoordinate(model.data(), meshPosition(*part->mesh,index));
                for (size_t k = 0; k < 3; ++k) {
                    p[j][k] = w[k];
                    valid = valid && std::isfinite(w[k]);
                    d[j] += (double(w[k]) - s.a[k]) * n[k];
                }
                if (std::abs(d[j]) < 1e-9) {
                    d[j] = 0;
                }
            }
            if (!valid) {
                continue;
            }
            // A collapsed face has no surface contour, even when it lies in the plane.
            std::array<double, 3> firstEdge{}, secondEdge{};
            for (size_t k = 0; k < 3; ++k) {
                firstEdge[k] = double(p[1][k]) - p[0][k];
                secondEdge[k] = double(p[2][k]) - p[0][k];
            }
            if (firstEdge[1] * secondEdge[2] == firstEdge[2] * secondEdge[1] &&
                firstEdge[2] * secondEdge[0] == firstEdge[0] * secondEdge[2] &&
                firstEdge[0] * secondEdge[1] == firstEdge[1] * secondEdge[0]) {
                continue;
            }
            if (d[0] == 0 && d[1] == 0 && d[2] == 0) {
                for (size_t j = 0; j < 3; ++j) {
                    ++coplanar[key(p[j], p[(j + 1) % 3])];
                }
                continue;
            }
            std::vector<Coordinate> hits;
            for (size_t j = 0; j < 3; ++j) {
                const size_t k = (j + 1) % 3;
                if (d[j] == 0) {
                    hits.push_back(p[j]);
                }
                if ((d[j] < 0 && d[k] > 0) || (d[j] > 0 && d[k] < 0)) {
                    const double t = d[j] / (d[j] - d[k]);
                    Coordinate v{};
                    for (size_t c = 0; c < 3; ++c) {
                        v[c] = p[j][c] + t * (p[k][c] - p[j][c]);
                    }
                    hits.push_back(v);
                }
            }
            if (hits.size() == 2 && hits[0] != hits[1]) {
                edges.insert(key(hits[0], hits[1]));
            }
        }
        for (const auto& [edge, count] : coplanar) {
            if (count % 2) {
                edges.insert(edge);
            }
        }
        for (const auto& e : edges) {
            contour.lines.push_back({{e[0], e[1], e[2]}, {e[3], e[4], e[5]}});
        }
        contour.minimum.fill(std::numeric_limits<double>::max());
        contour.maximum.fill(-std::numeric_limits<double>::max());
        for (const auto& line : contour.lines) {
            for (const auto p : {line.a, line.b}) {
                const auto xy = sectionCoordinates(s, p);
                for (size_t k = 0; k < 2; ++k) {
                    contour.minimum[k] = std::min(contour.minimum[k], xy[k]);
                    contour.maximum[k] = std::max(contour.maximum[k], xy[k]);
                }
            }
        }
        result.contours.push_back(std::move(contour));
    }
    if (s.showDifferences && s.reference != s.compared && !sources.empty()) {
        const auto& a = result.contours[static_cast<size_t>(s.reference)].lines;
        const auto& b = result.contours[static_cast<size_t>(s.compared)].lines;
        if (!a.empty() && !b.empty()) {
            for (const auto pair : {std::pair{&a, &b}, std::pair{&b, &a}}) {
                const size_t stride = std::max(size_t{1}, (pair.first->size() + 255) / 256);
                for (size_t i = 0; i < pair.first->size(); i += stride) {
                    for (const auto p : {(*pair.first)[i].a, (*pair.first)[i].b}) {
                        if (stop.stop_requested()) {
                            return {};
                        }
                        double nearest = std::numeric_limits<double>::infinity();
                        Coordinate hit{};
                        for (const auto& line : *pair.second) {
                            double dot = 0, length = 0;
                            for (size_t k = 0; k < 3; ++k) {
                                const double v = double(line.b[k]) - line.a[k];
                                dot += (double(p[k]) - line.a[k]) * v;
                                length += v * v;
                            }
                            const double t = length > 0 ? std::clamp(dot / length, 0.0, 1.0) : 0;
                            double distance = 0;
                            Coordinate q{};
                            for (size_t k = 0; k < 3; ++k) {
                                q[k] = line.a[k] + t * (line.b[k] - line.a[k]);
                                distance += std::pow(double(p[k]) - q[k], 2);
                            }
                            if (distance < nearest) {
                                nearest = distance;
                                hit = q;
                            }
                        }
                        if (!result.largestGap || nearest > result.gap * result.gap) {
                            result.gap = std::sqrt(nearest);
                            result.largestGap = CrossSectionEdge{p, hit};
                        }
                    }
                }
            }
        }
    }
    return result;
}
} // namespace woby
