#include "mesh_duplicates.h"
#include "analysis_index.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace woby {
namespace {
void canceled(const std::stop_token& stop)
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
constexpr size_t noOccurrence = std::numeric_limits<size_t>::max();
struct Occurrence { size_t part, next; };
struct SourceOccurrences {
    std::vector<size_t> heads;
    std::vector<Occurrence> entries;
};
void addOccurrence(SourceOccurrences& occurrences, size_t id, size_t part)
{
    occurrences.entries.push_back({part, occurrences.heads[id]});
    occurrences.heads[id] = occurrences.entries.size() - 1;
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
        SourceOccurrences pointParts, faceParts;
        std::vector<size_t> seen;
        if (result.points.enabled) {
            pointParts.heads.assign(data.points.size(), noOccurrence);
            pointParts.entries.reserve(data.points.size());
            seen.assign(data.points.size(), noOccurrence);
        }
        if (result.triangles.enabled && !stl) {
            faceParts.heads.assign(data.indices.size() / 3, noOccurrence);
            faceParts.entries.reserve(data.indices.size() / 3);
        }
        for (size_t partIndex = 0; partIndex < source.parts.size(); ++partIndex) {
            const auto& part = source.parts[partIndex];
            if (part.firstIndex % 3 || part.indexCount % 3 || part.firstIndex > data.indices.size()
                || part.indexCount > data.indices.size() - part.firstIndex) {
                throw std::invalid_argument("Invalid source part range.");
            }
            for (size_t i = part.firstIndex; i < part.firstIndex + part.indexCount; ++i) {
                canceled(stop);
                const auto point = data.indices[i];
                if (result.points.enabled && seen[point] != partIndex) {
                    seen[point] = partIndex;
                    addOccurrence(pointParts, point, partIndex);
                }
                if (!faceParts.heads.empty() && i % 3 == 0) { addOccurrence(faceParts, i/3, partIndex); }
            }
        }
        if (result.points.enabled && source.wholeFile && !source.parts.empty()) {
            for (size_t i = 0; i < data.points.size(); ++i) {
                canceled(stop);
                // Sentinel instance: unused points follow the file transform.
                if (pointParts.heads[i] == noOccurrence) { addOccurrence(pointParts, i, source.parts.size()); }
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
            AnalysisIndex<double, 3> groups;
            reserveAnalysisIndex(groups, data.points.size());
            std::vector<size_t> heads, next(data.points.size(), noOccurrence);
            for (size_t id = 0; id < pointParts.heads.size(); ++id) {
                canceled(stop);
                if (pointParts.heads[id] == noOccurrence) { continue; }
                const auto group = analysisIndex(groups, data.points[id]);
                if (group == heads.size()) { heads.push_back(noOccurrence); }
                next[id] = heads[group]; heads[group] = id;
            }
            std::vector<DuplicateFinding> found;
            for (size_t group = 0; group < heads.size(); ++group) {
                canceled(stop);
                if (next[heads[group]] == noOccurrence) { continue; }
                const auto& position = groups.keys[group];
                std::vector<size_t> members;
                for (size_t id = heads[group]; id != noOccurrence; id = next[id]) { canceled(stop); members.push_back(id); }
                std::reverse(members.begin(), members.end());
                auto value = finding();
                std::set<std::array<float, 3>> positions;
                for (const auto id : members) {
                    canceled(stop);
                    value.members.push_back({id, false});
                    for (size_t occurrence = pointParts.heads[id]; occurrence != noOccurrence; occurrence = pointParts.entries[occurrence].next) {
                        canceled(stop);
                        const auto p = pointParts.entries[occurrence].part;
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
            std::vector<std::array<uint32_t, 3>> keys(faceParts.heads.size());
            std::vector<size_t> starts(data.points.size() + 1);
            size_t selected = 0;
            for (size_t id = 0; id < faceParts.heads.size(); ++id) {
                canceled(stop);
                if (faceParts.heads[id] == noOccurrence) { continue; }
                keys[id] = triple(id);
                std::sort(keys[id].begin(), keys[id].end());
                ++starts[keys[id][0] + 1]; ++selected;
            }
            for (size_t i = 1; i < starts.size(); ++i) { starts[i] += starts[i-1]; }
            auto cursor = starts;
            std::vector<size_t> order(selected);
            for (size_t id = 0; id < faceParts.heads.size(); ++id) {
                canceled(stop);
                if (faceParts.heads[id] != noOccurrence) { order[cursor[keys[id][0]]++] = id; }
            }
            std::vector<size_t> heads, next(faceParts.heads.size(), noOccurrence);
            for (size_t point = 0; point < data.points.size(); ++point) {
                canceled(stop);
                std::sort(order.begin() + static_cast<ptrdiff_t>(starts[point]),
                    order.begin() + static_cast<ptrdiff_t>(starts[point+1]), [&](size_t a, size_t b) {
                        canceled(stop);
                        return keys[a] != keys[b] ? keys[a] < keys[b] : a < b;
                    });
                for (size_t i = starts[point]; i < starts[point+1];) {
                    size_t end = i + 1;
                    while (end < starts[point+1] && keys[order[end]] == keys[order[i]]) { ++end; }
                    if (end-i > 1) {
                        heads.push_back(order[end-1]);
                        for (size_t j = i+1; j < end; ++j) { next[order[j]] = order[j-1]; }
                    }
                    i = end;
                }
            }
            std::vector<DuplicateFinding> found;
            for (const auto head : heads) {
                canceled(stop);
                if (next[head] == noOccurrence) { continue; }
                std::vector<size_t> members;
                for (size_t id = head; id != noOccurrence; id = next[id]) { canceled(stop); members.push_back(id); }
                std::reverse(members.begin(), members.end());
                auto value = finding();
                std::set<std::array<std::array<float, 3>, 3>> geometries;
                for (const auto id : members) {
                    canceled(stop);
                    const auto ids = triple(id);
                    value.members.push_back({id, reversed(triple(members.front()), ids)});
                    for (size_t occurrence = faceParts.heads[id]; occurrence != noOccurrence; occurrence = faceParts.entries[occurrence].next) {
                        canceled(stop);
                        const auto part = faceParts.entries[occurrence].part;
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
