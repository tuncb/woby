#include "freeform.h"
#include "freeform_trim.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace woby {
namespace {
std::array<double, maxFreeformDegree + 1> basisValues(const std::vector<double>& knots,
    size_t span, uint32_t degree, double t)
{
    std::array<double, maxFreeformDegree + 1> result{}, left{}, right{};
    result[0] = 1;
    for (uint32_t j = 1; j <= degree; ++j) {
        left[j] = t - knots[span + 1 - j]; right[j] = knots[span + j] - t;
        double saved = 0;
        for (uint32_t r = 0; r < j; ++r) {
            const double divisor = right[r + 1] + left[j - r];
            const double value = divisor > 0 ? result[r] / divisor : 0;
            result[r] = saved + right[r + 1] * value;
            saved = left[j - r] * value;
        }
        result[j] = saved;
    }
    return result;
}

FreeformBasis basis(const std::vector<double>& knots, uint32_t count, uint32_t degree, double t)
{
    const auto upper = std::upper_bound(knots.begin() + degree, knots.begin() + count + 1, t);
    const size_t span = std::clamp(static_cast<size_t>(upper - knots.begin() - 1), size_t(degree), size_t(count - 1));
    FreeformBasis result; result.first = span - degree;
    result.values = basisValues(knots, span, degree, t);
    const auto lower = basisValues(knots, span, degree - 1, t);
    for (uint32_t j = 0; j <= degree; ++j) {
        const size_t i = result.first + j;
        const double a = knots[i + degree] - knots[i];
        const double b = knots[i + degree + 1] - knots[i + 1];
        result.derivatives[j] = degree * ((j && a > 0 ? lower[j - 1] / a : 0)
            - (j < degree && b > 0 ? lower[j] / b : 0));
    }
    return result;
}

void validateDirection(const std::vector<double>& knots, uint32_t count, uint32_t degree,
    const std::array<double, 2>& domain)
{
    if (degree < 1 || degree > maxFreeformDegree) { throw std::runtime_error("Freeform degree must be between 1 and 8."); }
    if (count <= degree || knots.size() != size_t(count) + degree + 1) {
        throw std::runtime_error("Freeform knot count does not match degree and control points.");
    }
    size_t repeats = 0;
    double previous = -std::numeric_limits<double>::infinity();
    for (const double knot : knots) {
        if (!std::isfinite(knot) || knot < previous) { throw std::runtime_error("Freeform knots must be finite and nondecreasing."); }
        repeats = knot == previous ? repeats + 1 : 1;
        const bool interior = knot > knots[degree] && knot < knots[count];
        if (repeats > size_t(degree) + (interior ? 0 : 1)) {
            throw std::runtime_error("Discontinuous freeform knot vectors are not supported.");
        }
        previous = knot;
    }
    if (!std::isfinite(domain[0]) || !std::isfinite(domain[1]) || domain[0] >= domain[1]
        || domain[0] < knots[degree] || domain[1] > knots[count]) {
        throw std::runtime_error("Freeform parameter domain is outside the active knot range.");
    }
}

std::vector<double> gridParameters(const std::vector<double>& knots, uint32_t degree,
    const std::array<double, 2>& domain, bool surface = false)
{
    std::vector<double> result{domain[0]};
    const auto segments = degree == 1 && !surface ? 1u : freeformSegmentsPerSpan;
    double start = domain[0];
    for (const double knot : knots) {
        const double end = std::min(knot, domain[1]);
        if (end <= start) { continue; }
        if (result.size() + segments > maxFreeformVertices) { throw std::runtime_error("Freeform tessellation exceeds the vertex limit."); }
        for (uint32_t i = 1; i <= segments; ++i) {
            result.push_back(i == segments ? end : std::lerp(start, end, double(i) / segments));
        }
        start = end;
        if (start == domain[1]) { break; }
    }
    return result;
}

std::array<float, 3> normal(const FreeformSample& sample)
{
    const auto& a = sample.du; const auto& b = sample.dv;
    Coordinate cross{a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
    const double length = std::hypot(cross[0], cross[1], cross[2]);
    if (!(length > 0) || !std::isfinite(length)) { return {}; }
    return {static_cast<float>(cross[0]/length), static_cast<float>(cross[1]/length), static_cast<float>(cross[2]/length)};
}
} // namespace

FreeformBasis freeformBasis(const std::vector<double>& knots, uint32_t count, uint32_t degree, double t)
{
    return basis(knots, count, degree, t);
}

void validateFreeformPatch(const FreeformPatch& patch)
{
    validateDirection(patch.knotsU, patch.countU, patch.degreeU, patch.domainU);
    if (patch.surface) { validateDirection(patch.knotsV, patch.countV, patch.degreeV, patch.domainV); }
    else if (patch.countV != 1 || patch.degreeV != 0) { throw std::runtime_error("Invalid freeform curve dimensions."); }
    if (patch.controls.size() != size_t(patch.countU) * patch.countV || patch.controls.size() > maxFreeformVertices) {
        throw std::runtime_error("Freeform control point count does not match its dimensions.");
    }
    for (const auto& point : patch.controls) {
        if (!std::all_of(point.begin(), point.end(), [](double v) { return std::isfinite(v); }) || point[3] <= 0) {
            throw std::runtime_error("Freeform control points must be finite with positive weights.");
        }
    }
    if (!patch.texcoords.empty() && patch.texcoords.size() != patch.controls.size()) {
        throw std::runtime_error("Freeform surfaces require UVs on all control points or none.");
    }
    if (!patch.normals.empty() && patch.normals.size() != patch.controls.size()) {
        throw std::runtime_error("Freeform surfaces require normals on all control points or none.");
    }
    for (const auto& n : patch.normals) {
        if (!finiteCoordinate(n)) { throw std::runtime_error("Non-finite freeform normal."); }
    }
    for (const auto& uv : patch.texcoords) {
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])
            || std::abs(uv[0]) > std::numeric_limits<float>::max() || std::abs(uv[1]) > std::numeric_limits<float>::max()) {
            throw std::runtime_error("Freeform texture coordinate exceeds the supported display range.");
        }
    }
}

FreeformSample evaluateFreeform(const FreeformPatch& patch, double u, double v)
{
    // Patches are validated once at the import/operation boundary.
    if (!std::isfinite(u) || u < patch.domainU[0] || u > patch.domainU[1]
        || (patch.surface && (!std::isfinite(v) || v < patch.domainV[0] || v > patch.domainV[1]))) {
        throw std::invalid_argument("Freeform sample is outside its parameter domain.");
    }
    const auto bu = basis(patch.knotsU, patch.countU, patch.degreeU, u);
    FreeformBasis bv; bv.values[0] = 1;
    if (patch.surface) { bv = basis(patch.knotsV, patch.countV, patch.degreeV, v); }
    double weight = 0, weightU = 0, weightV = 0, weightScale = 0;
    // Scale weights and subtract a local reference to avoid overflow and loss
    // of small features at large file coordinates.
    const auto& reference = patch.controls[bv.first * patch.countU + bu.first];
    for (uint32_t j = 0; j <= patch.degreeV; ++j) {
        for (uint32_t i = 0; i <= patch.degreeU; ++i) {
            weightScale = std::max(weightScale, patch.controls[(bv.first+j)*patch.countU + bu.first+i][3]);
        }
    }
    FreeformSample result;
    for (uint32_t j = 0; j <= patch.degreeV; ++j) {
        for (uint32_t i = 0; i <= patch.degreeU; ++i) {
            const size_t index = (bv.first+j)*patch.countU + bu.first+i;
            const auto& p = patch.controls[index];
            const double w = p[3] / weightScale;
            const double b = bu.values[i] * bv.values[j] * w;
            const double du = bu.derivatives[i] * bv.values[j] * w;
            const double dv = bu.values[i] * bv.derivatives[j] * w;
            weight += b; weightU += du; weightV += dv;
            for (size_t k = 0; k < 3; ++k) {
                const double delta = p[k] - reference[k];
                result.position[k] += b * delta; result.du[k] += du * delta; result.dv[k] += dv * delta;
                if (!patch.normals.empty()) { result.normal[k] += b * patch.normals[index][k]; }
            }
            if (!patch.texcoords.empty()) {
                for (size_t k = 0; k < 2; ++k) { result.texcoord[k] += b * patch.texcoords[index][k]; }
            }
        }
    }
    if (!(weight > 0) || !std::isfinite(weight)) { throw std::runtime_error("Freeform evaluation has an invalid rational denominator."); }
    for (size_t k = 0; k < 3; ++k) {
        result.position[k] /= weight;
        result.du[k] = (result.du[k] - result.position[k]*weightU) / weight;
        result.dv[k] = (result.dv[k] - result.position[k]*weightV) / weight;
        result.position[k] += reference[k];
    }
    for (auto& value : result.texcoord) { value /= weight; }
    if (!finiteCoordinate(result.position) || !finiteCoordinate(result.du) || !finiteCoordinate(result.dv)) {
        throw std::runtime_error("Freeform evaluation exceeded the numeric range.");
    }
    return result;
}

std::array<float, 3> freeformNormal(const FreeformPatch& patch, double u, double v)
{
    if (!patch.surface) { return {0,0,1}; }
    const auto sample = evaluateFreeform(patch, u, v);
    if (!patch.normals.empty()) {
        const double length = std::hypot(sample.normal[0], sample.normal[1], sample.normal[2]);
        if (length > 0 && std::isfinite(length)) {
            return {static_cast<float>(sample.normal[0]/length), static_cast<float>(sample.normal[1]/length), static_cast<float>(sample.normal[2]/length)};
        }
    }
    auto result = normal(sample);
    if (validNormal(result)) { return result; }
    // At a collapsed boundary, use the nearby interior differential.
    const double a = std::lerp(patch.domainU[0], patch.domainU[1], 1e-4);
    const double b = std::lerp(patch.domainU[0], patch.domainU[1], 1-1e-4);
    const double c = std::lerp(patch.domainV[0], patch.domainV[1], 1e-4);
    const double d = std::lerp(patch.domainV[0], patch.domainV[1], 1-1e-4);
    result = normal(evaluateFreeform(patch, std::clamp(u,a,b), std::clamp(v,c,d)));
    return validNormal(result) ? result : std::array<float,3>{0,0,1};
}

void appendFreeformGeometry(Mesh& mesh, std::vector<FreeformPatch> patches, const ModelLoadProgressCallback& progress)
{
    if (patches.empty()) { return; }
    auto geometry = std::make_shared<FreeformGeometry>();
    geometry->patches = std::move(patches);
    auto source = mesh.sourceData ? std::make_shared<SourceMeshData>(*mesh.sourceData) : std::make_shared<SourceMeshData>();
    if (!mesh.sourceData) { source->provenance = SourceProvenance::objPositions; }
    std::unordered_set<uint64_t> usedIds(source->originalPointIds.begin(), source->originalPointIds.end());
    uint64_t nextId = 0;
    std::unordered_set<std::string> names;
    for (const auto& node : mesh.nodes) { names.insert(node.name); }
    size_t total = 0;
    for (const auto& patch : geometry->patches) {
        validateFreeformPatch(patch);
        FreeformGrid grid;
        grid.u = gridParameters(patch.knotsU, patch.degreeU, patch.domainU, patch.surface);
        grid.v = patch.surface ? gridParameters(patch.knotsV, patch.degreeV, patch.domainV, true) : std::vector<double>{0};
        triangulateFreeformTrim(patch,grid,progress);
        const size_t count = freeformVertexCount(grid);
        if (count > maxFreeformVertices - total) { throw std::runtime_error("Freeform tessellation exceeds the vertex limit."); }
        total += count;
        geometry->grids.push_back(std::move(grid));
    }
    if (mesh.vertices.size() + total > UINT32_MAX || source->points.size() + total > UINT32_MAX
        || mesh.indices.size() + total*6 > UINT32_MAX || mesh.lineIndices.size() + total*2 > UINT32_MAX) {
        throw std::runtime_error("Freeform geometry exceeds the supported index range.");
    }
    size_t completed = 0;
    for (size_t p = 0; p < geometry->patches.size(); ++p) {
        auto& patch = geometry->patches[p]; auto& grid = geometry->grids[p];
        const auto baseName = patch.name.empty() ? (patch.surface ? "Surface" : "Curve") : patch.name;
        patch.name = baseName;
        for (size_t suffix = 2; !names.insert(patch.name).second; ++suffix) { patch.name = baseName + " (" + std::to_string(suffix) + ")"; }
        grid.vertexOffset = static_cast<uint32_t>(mesh.vertices.size());
        grid.groupIndex = static_cast<uint32_t>(mesh.nodes.size());
        grid.indexOffset = static_cast<uint32_t>(patch.surface ? mesh.indices.size() : mesh.lineIndices.size());
        const auto sourceOffset = static_cast<uint32_t>(source->points.size());
        const auto appendVertex = [&](double u,double v) {
            if (completed % 1024 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, completed, total); }
            const auto sample = evaluateFreeform(patch,u,v);
            const auto point = relativePosition(sample.position, mesh.origin);
            Vertex vertex{renderPosition(point), freeformNormal(patch,u,v), {}};
            if (!patch.texcoords.empty()) { vertex.texcoord = {static_cast<float>(sample.texcoord[0]), 1-static_cast<float>(sample.texcoord[1])}; }
            mesh.vertices.push_back(vertex); mesh.precisePositions.push_back(point); source->points.push_back(point);
            if (!usedIds.empty()) {
                while (usedIds.contains(nextId)) { ++nextId; }
                source->originalPointIds.push_back(nextId++);
            }
            ++completed;
        };
        if (!grid.samples.empty()) {
            for (const auto& uv:grid.samples) { appendVertex(uv[0],uv[1]); }
            for (auto index:grid.triangles) { mesh.indices.push_back(grid.vertexOffset+index); source->indices.push_back(sourceOffset+index); }
        } else {
            for (const double v:grid.v) for (const double u:grid.u) { appendVertex(u,v); }
        }
        const uint32_t nu = static_cast<uint32_t>(grid.u.size()), nv = static_cast<uint32_t>(grid.v.size());
        for (uint32_t y = 0; grid.samples.empty() && y < (patch.surface ? nv-1 : 1); ++y) {
            for (uint32_t x = 0; x + 1 < nu; ++x) {
                const uint32_t a = y*nu + x;
                if (patch.surface) {
                    for (const auto index : {a,a+1,a+nu,a+1,a+nu+1,a+nu}) {
                        mesh.indices.push_back(grid.vertexOffset + index); source->indices.push_back(sourceOffset + index);
                    }
                } else { mesh.lineIndices.push_back(grid.vertexOffset+a); mesh.lineIndices.push_back(grid.vertexOffset+a+1); }
            }
        }
        MeshNode node; node.name = patch.name; node.hasTexcoords = !patch.texcoords.empty();
        if (patch.surface) { node.indexOffset = grid.indexOffset; node.indexCount = static_cast<uint32_t>(mesh.indices.size()) - grid.indexOffset; }
        else { node.lineIndexOffset = grid.indexOffset; node.lineIndexCount = static_cast<uint32_t>(mesh.lineIndices.size()) - grid.indexOffset; }
        mesh.nodes.push_back(std::move(node));
    }
    mesh.sourceData = std::move(source); mesh.freeform = std::move(geometry);
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, total, total);
}
} // namespace woby
