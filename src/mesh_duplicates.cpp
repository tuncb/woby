#include "mesh_duplicates.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace woby {
namespace {
void canceled(std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
}
std::array<float, 3> transformed(const std::array<double, 3>& point, const std::array<float, 16>& m)
{
    std::array<float, 3> result{};
    for (size_t k = 0; k < 3; ++k) {
        const double value = m[k]*point[0] + m[k+4]*point[1] + m[k+8]*point[2] + m[k+12];
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
            throw std::invalid_argument("Duplicate inspection requires finite transformed coordinates.");
        }
        result[k] = static_cast<float>(value);
    }
    return result;
}
bool reversed(const std::array<uint32_t, 3>& a, const std::array<uint32_t, 3>& b)
{
    for (size_t k = 0; k < 3; ++k) {
        if (a[0] == b[k] && a[1] == b[(k+1)%3] && a[2] == b[(k+2)%3]) { return false; }
    }
    return true;
}
}

const char* sourceProvenanceName(SourceProvenance provenance)
{
    switch (provenance) {
    case SourceProvenance::objPositions: return "OBJ position records / generated triangles";
    case SourceProvenance::stlCorners: return "STL facet corners (informational)";
    case SourceProvenance::importerVertices: return "Importer vertex table / generated triangles";
    }
    return "Unknown";
}
const char* duplicateStatus(const DuplicateResult& result)
{
    if (!result.enabled) { return "disabled"; }
    if (result.unavailableSources) { return result.availableSources ? "partial" : "unavailable"; }
    return "complete";
}

MeshDuplicates inspectDuplicates(const DuplicateInput& input, std::stop_token stop)
{
    MeshDuplicates result;
    result.points.enabled = input.settings.points;
    result.triangles.enabled = input.settings.triangles;
    canceled(stop);
    if (!result.points.enabled && !result.triangles.enabled) { return result; }
    for (const auto& source : input.sources) {
        canceled(stop);
        if (!source.data) {
            if (result.points.enabled) { ++result.points.unavailableSources; }
            if (result.triangles.enabled) { ++result.triangles.unavailableSources; }
            continue;
        }
        const auto& data = *source.data;
        const bool stl = data.provenance == SourceProvenance::stlCorners;
        if (result.points.enabled) { ++result.points.availableSources; }
        if (result.triangles.enabled) {
            if (stl) { ++result.triangles.unavailableSources; }
            else { ++result.triangles.availableSources; }
        }
        if (data.indices.size() % 3 != 0) { throw std::invalid_argument("Invalid source triangle indices."); }
        for (const auto& point : data.points) {
            canceled(stop);
            for (const auto value : point) {
                if (!std::isfinite(value)) { throw std::invalid_argument("Non-finite source coordinate."); }
            }
        }
        for (const auto index : data.indices) {
            canceled(stop);
            if (index >= data.points.size()) { throw std::invalid_argument("Invalid source point index."); }
        }
        // Track every transformed occurrence without counting a shared source ID twice.
        std::map<size_t, std::vector<size_t>> pointParts, faceParts;
        for (size_t partIndex = 0; partIndex < source.parts.size(); ++partIndex) {
            const auto& part = source.parts[partIndex];
            if (part.firstIndex % 3 || part.indexCount % 3 || part.firstIndex > data.indices.size()
                || part.indexCount > data.indices.size() - part.firstIndex) {
                throw std::invalid_argument("Invalid source part range.");
            }
            std::set<size_t> points;
            for (size_t i = part.firstIndex; i < part.firstIndex + part.indexCount; ++i) {
                canceled(stop);
                if (data.indices[i] >= data.points.size()) { throw std::invalid_argument("Invalid source point index."); }
                points.insert(data.indices[i]);
                if (i % 3 == 0) { faceParts[i/3].push_back(partIndex); }
            }
            for (const auto p : points) { pointParts[p].push_back(partIndex); }
        }
        if (source.wholeFile && !source.parts.empty()) {
            for (size_t i = 0; i < data.points.size(); ++i) {
                canceled(stop);
                // Sentinel instance: unused points follow the file transform.
                if (!pointParts.contains(i)) { pointParts[i].push_back(source.parts.size()); }
            }
        }
        const auto finding = [&] {
            DuplicateFinding value;
            value.fileId = source.fileId;
            value.source = source.name;
            value.provenance = data.provenance;
            return value;
        };
        if (result.points.enabled) {
            // Ordered finite-coordinate keys compare +0 and -0 equally, with no epsilon.
            std::map<std::array<double, 3>, std::vector<size_t>> groups;
            for (const auto& [id, parts] : pointParts) { canceled(stop); (void)parts; groups[data.points[id]].push_back(id); }
            std::vector<DuplicateFinding> found;
            for (const auto& [position, members] : groups) {
                canceled(stop);
                if (members.size() < 2) { continue; }
                auto value = finding();
                std::set<std::array<float, 3>> positions;
                for (const auto id : members) {
                    canceled(stop);
                    value.members.push_back({id, false});
                    for (const auto p : pointParts.at(id)) {
                        canceled(stop);
                        positions.insert(transformed(position, p == source.parts.size() ? source.unusedPointTransform : source.parts[p].transform));
                    }
                }
                value.geometry.assign(positions.begin(), positions.end());
                if (stl) { result.points.informationalCount += members.size() - 1; }
                else { result.points.duplicateCount += members.size() - 1; }
                found.push_back(std::move(value));
            }
            std::sort(found.begin(), found.end(), [&](const auto& a, const auto& b) { canceled(stop); return a.members.front().id < b.members.front().id; });
            for (auto& value : found) { result.points.findings.push_back(std::move(value)); }
        }
        if (result.triangles.enabled && !stl) {
            const auto triple = [&](size_t id) { return std::array<uint32_t, 3>{data.indices[id*3], data.indices[id*3+1], data.indices[id*3+2]}; };
            std::map<std::array<uint32_t, 3>, std::vector<size_t>> groups;
            for (const auto& [id, parts] : faceParts) {
                canceled(stop); (void)parts;
                auto key = triple(id);
                std::sort(key.begin(), key.end());
                groups[key].push_back(id);
            }
            std::vector<DuplicateFinding> found;
            for (const auto& [key, members] : groups) {
                canceled(stop); (void)key;
                if (members.size() < 2) { continue; }
                auto value = finding();
                std::set<std::array<std::array<float, 3>, 3>> geometries;
                for (const auto id : members) {
                    canceled(stop);
                    const auto ids = triple(id);
                    value.members.push_back({id, reversed(triple(members.front()), ids)});
                    for (const auto part : faceParts.at(id)) {
                        std::array<std::array<float, 3>, 3> geometry;
                        for (size_t k = 0; k < 3; ++k) { geometry[k] = transformed(data.points[ids[k]], source.parts[part].transform); }
                        std::sort(geometry.begin(), geometry.end());
                        geometries.insert(geometry);
                    }
                }
                for (const auto& geometry : geometries) { value.geometry.insert(value.geometry.end(), geometry.begin(), geometry.end()); }
                result.triangles.duplicateCount += members.size() - 1;
                found.push_back(std::move(value));
            }
            std::sort(found.begin(), found.end(), [&](const auto& a, const auto& b) { canceled(stop); return a.members.front().id < b.members.front().id; });
            for (auto& value : found) { result.triangles.findings.push_back(std::move(value)); }
        }
    }
    for (auto* detector : {&result.points, &result.triangles}) {
        std::sort(detector->findings.begin(), detector->findings.end(), [&](const auto& a, const auto& b) {
            canceled(stop);
            return a.fileId != b.fileId ? a.fileId < b.fileId : a.members.front().id < b.members.front().id;
        });
    }
    return result;
}
} // namespace woby
