#include "model_mesh.h"
#include "simd_bounds.h"
#include "parallel_work.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace woby {
Mesh::Mesh(const Mesh& other)
    : storage(other.storage), draft(other.draft ? std::make_unique<MeshGeometry>(*other.draft) : nullptr),
      preparationCache(other.draft ? nullptr : other.preparationCache),
      bounds(other.bounds), originalBounds(other.originalBounds), origin(other.origin),
      sourceData(other.sourceData), duplicateInput(other.duplicateInput),
      annotationCache(other.draft ? nullptr : other.annotationCache), uvQuality(other.uvQuality),
      freeform(other.freeform), contentRevision(other.contentRevision), preparationDeferred(other.preparationDeferred)
{
}
Mesh& Mesh::operator=(const Mesh& other)
{
    if (this != &other) { Mesh copy(other); *this = std::move(copy); }
    return *this;
}
MeshGeometry& editMeshGeometry(Mesh& mesh)
{
    mesh.preparationCache.reset();
    if (!mesh.draft) {
        auto next = std::make_unique<MeshGeometry>(meshGeometry(mesh));
        mesh.draft = std::move(next);
        if (mesh.storage) { mesh.annotationCache.reset(); mesh.uvQuality.reset(); }
        // Retain the previous owner until publication so existing const borrows
        // remain valid while construction starts (including argument evaluation).
        renewMeshContentRevision(mesh);
    }
    return *mesh.draft;
}
void publishMeshGeometry(Mesh& mesh)
{
    if (!mesh.draft) { return; }
    const auto& g = *mesh.draft;
    if (g.sourcePositions) {
        if (!g.precisePositions.empty()) { throw std::invalid_argument("Mixed direct and source positions."); }
        if (g.positionIndexing == PositionIndexing::identity) {
            if (!g.positionIndices.empty() || g.vertices.size() > g.sourcePositions->size()) {
                throw std::invalid_argument("Invalid identity position storage.");
            }
        } else {
            if (g.positionIndices.size() != g.vertices.size()) { throw std::invalid_argument("Incomplete position mapping."); }
            for (const auto index : g.positionIndices) {
                if (index >= g.sourcePositions->size()) { throw std::invalid_argument("Invalid source position index."); }
            }
        }
    } else if (!g.positionIndices.empty() || g.positionIndexing != PositionIndexing::identity
        || (!g.precisePositions.empty() && g.precisePositions.size() != g.vertices.size())) {
        throw std::invalid_argument("Invalid direct position storage.");
    }
    if (!mesh.preparationCache || mesh.preparationCache->geometry != mesh.draft.get()) {
        auto cache = std::make_shared<MeshPreparationCache>();
        cache->geometry = mesh.draft.get();
        mesh.preparationCache = std::move(cache);
    }
    mesh.storage = std::shared_ptr<const MeshGeometry>(std::move(mesh.draft));
}

const Bounds* validatedMeshBounds(const Mesh& mesh) noexcept
{
    const auto& cache = mesh.preparationCache;
    return !mesh.draft && cache && cache->geometry == mesh.storage.get() && cache->bounds
        ? &*cache->bounds : nullptr;
}

void appendMeshPositionIndex(MeshGeometry& geometry, uint32_t sourceIndex)
{
    // Called before the corresponding render vertex is appended.
    if (geometry.positionIndexing == PositionIndexing::identity && sourceIndex != geometry.vertices.size()) {
        geometry.positionIndices.reserve(geometry.vertices.capacity());
        for (size_t i = 0; i < geometry.vertices.size(); ++i) { geometry.positionIndices.push_back(static_cast<uint32_t>(i)); }
        geometry.positionIndexing = PositionIndexing::mapped;
    }
    if (geometry.positionIndexing == PositionIndexing::mapped) { geometry.positionIndices.push_back(sourceIndex); }
}
std::vector<Coordinate>& editMeshPositions(Mesh& mesh)
{
    std::vector<Coordinate> positions;
    positions.reserve(meshGeometry(mesh).vertices.size());
    for (size_t i = 0; i < meshGeometry(mesh).vertices.size(); ++i) { positions.push_back(meshPosition(mesh, i)); }
    auto& g = editMeshGeometry(mesh);
    g.precisePositions = std::move(positions);
    g.sourcePositions.reset();
    g.positionIndexing = PositionIndexing::identity;
    std::vector<uint32_t>().swap(g.positionIndices);
    mesh.sourceData.reset();
    mesh.duplicateInput.reset();
    mesh.originalBounds.reset();
    mesh.annotationCache.reset();
    mesh.uvQuality.reset();
    renewMeshContentRevision(mesh);
    return g.precisePositions;
}

uint64_t nextMeshContentRevision()
{
    static std::atomic<uint64_t> revision{0};
    return revision.fetch_add(1, std::memory_order_relaxed) + 1;
}
void renewMeshContentRevision(Mesh& mesh) { mesh.contentRevision = nextMeshContentRevision(); }

namespace {

std::array<float, 3> subtract(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2]};
}

std::array<float, 3> cross(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    return {
        lhs[1] * rhs[2] - lhs[2] * rhs[1],
        lhs[2] * rhs[0] - lhs[0] * rhs[2],
        lhs[0] * rhs[1] - lhs[1] * rhs[0],
    };
}

void add(std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    lhs[0] += rhs[0];
    lhs[1] += rhs[1];
    lhs[2] += rhs[2];
}

void normalize(std::array<float, 3>& value)
{
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (length <= 0.000001f || !std::isfinite(length)) {
        value = {0.0f, 1.0f, 0.0f};
        return;
    }

    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
}

void calculateFaceNormals(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
    size_t first, size_t count, std::array<float, 3>* output)
{
    size_t face = 0;
#if defined(__AVX2__)
    // Four independent faces share the square root and division instructions.
    // Keep the scalar expression order and exact division, including its fallback.
    for (; face + 4 <= count; face += 4) {
        const size_t index = (first + face) * 3;
        const auto position = [&](size_t corner, size_t axis) {
            return _mm_setr_ps(vertices[indices[index + corner]].position[axis],
                vertices[indices[index + 3 + corner]].position[axis],
                vertices[indices[index + 6 + corner]].position[axis],
                vertices[indices[index + 9 + corner]].position[axis]);
        };
        const auto ax = position(0, 0), ay = position(0, 1), az = position(0, 2);
        const auto bx = _mm_sub_ps(position(1, 0), ax), by = _mm_sub_ps(position(1, 1), ay), bz = _mm_sub_ps(position(1, 2), az);
        const auto cx = _mm_sub_ps(position(2, 0), ax), cy = _mm_sub_ps(position(2, 1), ay), cz = _mm_sub_ps(position(2, 2), az);
        auto x = _mm_sub_ps(_mm_mul_ps(by, cz), _mm_mul_ps(bz, cy));
        auto y = _mm_sub_ps(_mm_mul_ps(bz, cx), _mm_mul_ps(bx, cz));
        auto z = _mm_sub_ps(_mm_mul_ps(bx, cy), _mm_mul_ps(by, cx));
        const auto length = _mm_sqrt_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(x, x), _mm_mul_ps(y, y)), _mm_mul_ps(z, z)));
        const auto valid = _mm_and_ps(_mm_cmpgt_ps(length, _mm_set1_ps(0.000001f)),
            _mm_cmple_ps(length, _mm_set1_ps(std::numeric_limits<float>::max())));
        const auto divisor = _mm_blendv_ps(_mm_set1_ps(1), length, valid);
        x = _mm_div_ps(_mm_and_ps(x, valid), divisor);
        y = _mm_div_ps(_mm_blendv_ps(_mm_set1_ps(1), y, valid), divisor);
        z = _mm_div_ps(_mm_and_ps(z, valid), divisor);
        float xs[4], ys[4], zs[4];
        _mm_storeu_ps(xs, x); _mm_storeu_ps(ys, y); _mm_storeu_ps(zs, z);
        for (size_t lane = 0; lane < 4; ++lane) { output[face + lane] = {xs[lane], ys[lane], zs[lane]}; }
    }
#endif
    for (; face < count; ++face) {
        const size_t index = (first + face) * 3;
        output[face] = calculateFaceNormal(vertices[indices[index]].position,
            vertices[indices[index + 1]].position, vertices[indices[index + 2]].position);
    }
}

bool hasCompleteNormals(const std::vector<Vertex>& vertices)
{
    return std::all_of(vertices.begin(), vertices.end(), [](const Vertex& vertex) {
        return validNormal(vertex.normal);
    });
}

} // namespace

void coordinateIdentity(double* result)
{
    std::fill_n(result, 16, 0.0);
    for (size_t i = 0; i < 4; ++i) { result[i*5] = 1.0; }
}
void coordinateMultiply(double* result, const double* a, const double* b)
{
    CoordinateMatrix product{};
    for (size_t col = 0; col < 4; ++col) {
        for (size_t row = 0; row < 4; ++row) {
            for (size_t k = 0; k < 4; ++k) { product[col*4+row] += a[col*4+k] * b[k*4+row]; }
        }
    }
    std::copy(product.begin(), product.end(), result);
}
Coordinate transformCoordinate(const double* m, const Coordinate& p)
{
    Coordinate result{};
    for (size_t k = 0; k < 3; ++k) { result[k] = m[k]*p[0] + m[k+4]*p[1] + m[k+8]*p[2] + m[k+12]; }
    return result;
}

bool finiteCoordinate(const Coordinate& point) noexcept
{
    return std::all_of(point.begin(), point.end(), [](double v) { return std::isfinite(v); });
}
Coordinate meshPosition(const Mesh& mesh, size_t index)
{
    const auto& g = meshGeometry(mesh);
    if (index >= g.vertices.size()) { throw std::out_of_range("Invalid render vertex index."); }
    if (g.sourcePositions) {
        return g.sourcePositions->at(g.positionIndexing == PositionIndexing::identity ? index : g.positionIndices.at(index));
    }
    if (!g.precisePositions.empty()) { return g.precisePositions.at(index); }
    const auto& p = g.vertices[index].position;
    return {p[0], p[1], p[2]};
}
namespace {
std::optional<std::array<Coordinate, 2>> reduceOriginalMeshBounds(
    const Mesh& mesh, const MeshNode* node, const std::function<bool()>& canceled)
{
    if (canceled && canceled()) { return {}; }
    const auto& geometry = meshGeometry(mesh);
    const auto indices = node ? meshNodeIndices(mesh, *node) : std::span<const uint32_t>{};
    const auto count = node ? indices.size() : geometry.vertices.size();
    if (!count) { return std::array<Coordinate, 2>{}; }
    // Select storage once. The checked accessor can then stay inline, so each
    // corner does not spill the SIMD accumulators around a meshPosition call.
    const auto reduce = [&](const auto& position) -> std::optional<std::array<Coordinate, 2>> {
        const auto original = [&](size_t i) {
            const size_t index = node ? indices[i] : i;
            if (index >= geometry.vertices.size()) { throw std::out_of_range("Invalid render vertex index."); }
            const auto point = originalPosition(position(index), mesh.origin);
            if (!finiteCoordinate(point)) { throw std::runtime_error("Non-finite original coordinate."); }
            return point;
        };
        auto bounds = doubleBoundsReduction(original(0));
        for (size_t begin = 1; begin < count;) {
            if (canceled && canceled()) { return {}; }
            const auto end = begin + std::min(size_t{65536}, count - begin);
            for (size_t i = begin; i < end; ++i) { expand(bounds, original(i)); }
            begin = end;
        }
        if (canceled && canceled()) { return {}; }
        return boundsResult(bounds);
    };
    if (geometry.sourcePositions) {
        const auto& positions = *geometry.sourcePositions;
        if (geometry.positionIndexing == PositionIndexing::identity) {
            return reduce([&](size_t i) { return positions.at(i); });
        }
        return reduce([&](size_t i) { return positions.at(geometry.positionIndices.at(i)); });
    }
    if (!geometry.precisePositions.empty()) {
        return reduce([&](size_t i) { return geometry.precisePositions.at(i); });
    }
    return reduce([&](size_t i) {
        const auto& p = geometry.vertices[i].position;
        return Coordinate{p[0], p[1], p[2]};
    });
}
void recordMeshBounds(Mesh& mesh)
{
    auto cache = std::make_shared<MeshPreparationCache>();
    cache->geometry = &meshGeometry(mesh);
    cache->bounds = mesh.bounds;
    mesh.preparationCache = std::move(cache);
}
struct MappingCanceled {};
} // namespace

std::array<Coordinate, 2> originalMeshBounds(const Mesh& mesh, const MeshNode* node)
{
    if (!node && mesh.originalBounds) { return *mesh.originalBounds; }
    return *reduceOriginalMeshBounds(mesh, node, {});
}

std::optional<std::array<Coordinate, 2>> prepareOriginalMeshBounds(
    const Mesh& mesh, const std::function<bool()>& canceled)
{
    return reduceOriginalMeshBounds(mesh, nullptr, canceled);
}

std::optional<std::vector<uint32_t>> mapSourceTriangleIndices(
    const Mesh& mesh, const std::function<bool()>& canceled, bool parallel)
{
    if (canceled && canceled()) { return {}; }
    const auto& geometry = meshGeometry(mesh);
    if (!mesh.sourceData) { throw std::invalid_argument("Missing source triangle positions."); }
    const auto positionCount = sourcePositions(*mesh.sourceData).size();
    std::vector<uint32_t> indices(geometry.indices.size());
    // Existing callbacks need not be thread-safe. Poll them under one lock,
    // outside the tight mapping loop, and join all writers before returning.
    std::mutex cancelMutex;
    std::atomic<bool> stopped{false};
    const auto checkCanceled = [&] {
        if (stopped.load(std::memory_order_relaxed)) { throw MappingCanceled{}; }
        if (canceled) {
            std::lock_guard lock(cancelMutex);
            if (stopped.load(std::memory_order_relaxed) || canceled()) {
                stopped.store(true, std::memory_order_relaxed);
                throw MappingCanceled{};
            }
        }
    };
    const auto map = [&](const auto& sourceIndex) {
        parallelAnalysisBatches(indices.size(), 65536, {}, [&](size_t begin, size_t end) {
            checkCanceled();
            for (size_t i = begin; i < end; ++i) {
                const auto vertex = geometry.indices[i];
                if (vertex >= geometry.vertices.size()) { throw std::runtime_error("Invalid source triangle vertex."); }
                const auto position = sourceIndex(vertex);
                if (position >= positionCount) { throw std::runtime_error("Invalid source triangle position."); }
                indices[i] = position;
            }
        }, parallel ? size_t{262144} : std::numeric_limits<size_t>::max());
    };
    try {
        if (geometry.positionIndexing == PositionIndexing::identity) { map([](uint32_t vertex) { return vertex; }); }
        else { map([&](uint32_t vertex) { return geometry.positionIndices.at(vertex); }); }
        checkCanceled();
    } catch (const MappingCanceled&) { return {}; }
    return indices;
}
std::array<float, 3> renderPosition(const Coordinate& point)
{
    std::array<float, 3> result{};
    for (size_t k = 0; k < 3; ++k) {
        if (!std::isfinite(point[k]) || std::abs(point[k]) > std::numeric_limits<float>::max()) {
            throw std::invalid_argument("Position exceeds the supported display range.");
        }
        result[k] = static_cast<float>(point[k]);
    }
    return result;
}
Coordinate relativePosition(const Coordinate& point, const Coordinate& origin)
{
    return {point[0] - origin[0], point[1] - origin[1], point[2] - origin[2]};
}
Coordinate originalPosition(const Coordinate& point, const Coordinate& origin)
{
    return {point[0] + origin[0], point[1] + origin[1], point[2] + origin[2]};
}
Coordinate coordinateOrigin(const std::vector<Coordinate>& points)
{
    if (points.empty()) { return {}; }
    auto bounds = doubleBoundsReduction(points.front());
    for (const auto& p : points) {
        if (!finiteCoordinate(p)) { throw std::invalid_argument("Non-finite source coordinate."); }
        expand(bounds, p);
    }
    const auto [low, high] = boundsResult(bounds);
    Coordinate origin{};
    // Small coordinates already have a useful working frame. Keep their existing
    // origin, including models straddling zero. Rebase large absolute offsets.
    for (size_t k = 0; k < 3; ++k) {
        const double center = low[k] * .5 + high[k] * .5;
        if (std::abs(center) >= 65536.0) { origin[k] = center; }
    }
    return origin;
}
void localizeMesh(Mesh& mesh)
{
    renewMeshContentRevision(mesh);
    if (meshGeometry(mesh).precisePositions.size() != meshGeometry(mesh).vertices.size()) {
        throw std::invalid_argument("Source and render position counts differ.");
    }
    mesh.origin = coordinateOrigin(meshGeometry(mesh).precisePositions);
    for (size_t i = 0; i < meshGeometry(mesh).vertices.size(); ++i) {
        auto& p = editMeshGeometry(mesh).precisePositions[i];
        p = relativePosition(p, mesh.origin);
        editMeshGeometry(mesh).vertices[i].position = renderPosition(p);
    }
}

void rebaseMesh(Mesh& mesh, const Coordinate& origin)
{
    renewMeshContentRevision(mesh);
    if (!finiteCoordinate(origin)) { throw std::invalid_argument("Non-finite mesh origin."); }
    if (mesh.origin == origin) { return; }
    const auto delta = relativePosition(mesh.origin, origin);
    auto& g = editMeshGeometry(mesh);
    if (g.sourcePositions) {
        const auto previous = g.sourcePositions;
        auto positions = std::make_shared<std::vector<Coordinate>>(*previous);
        for (auto& p : *positions) { p = originalPosition(p, delta); }
        g.sourcePositions = std::move(positions);
        if (mesh.sourceData && mesh.sourceData->positions == previous) {
            auto source = std::make_shared<SourceMeshData>(*mesh.sourceData);
            source->positions = g.sourcePositions;
            mesh.sourceData = std::move(source);
        } else if (mesh.sourceData) {
            auto source = std::make_shared<SourceMeshData>(*mesh.sourceData);
            for (auto& p : editSourcePositions(*source)) { p = originalPosition(p, delta); }
            publishSourcePositions(*source);
            mesh.sourceData = std::move(source);
        }
    } else {
        if (g.precisePositions.empty()) {
            g.precisePositions.reserve(g.vertices.size());
            for (const auto& vertex : g.vertices) { g.precisePositions.push_back({vertex.position[0], vertex.position[1], vertex.position[2]}); }
        }
        for (auto& p : g.precisePositions) { p = originalPosition(p, delta); }
        if (mesh.sourceData) {
            auto source = std::make_shared<SourceMeshData>(*mesh.sourceData);
            for (auto& p : editSourcePositions(*source)) { p = originalPosition(p, delta); }
            publishSourcePositions(*source);
            mesh.sourceData = std::move(source);
        }
    }
    for (size_t i = 0; i < g.vertices.size(); ++i) { g.vertices[i].position = renderPosition(meshPosition(mesh, i)); }
    if (mesh.duplicateInput) {
        auto input = std::make_shared<DuplicateInput>(*mesh.duplicateInput);
        for (auto& source : input->sources) {
            for (size_t k = 0; k < 3; ++k) { source.unusedPointTransform[12+k] += delta[k]; }
            for (auto& part : source.parts) { for (size_t k = 0; k < 3; ++k) { part.transform[12+k] += delta[k]; } }
        }
        mesh.duplicateInput = std::move(input);
    }
    mesh.origin = origin;
    mesh.annotationCache.reset();
    if (!meshGeometry(mesh).vertices.empty()) {
        mesh.bounds = calculateBounds(meshGeometry(mesh).vertices);
        recordMeshBounds(mesh);
    }
}

bool empty(const Mesh& mesh) noexcept
{
    return meshGeometry(mesh).vertices.empty() || (meshGeometry(mesh).indices.empty() && meshGeometry(mesh).lineIndices.empty() && meshGeometry(mesh).pointIndices.empty());
}

std::span<const uint32_t> meshNodeIndices(const Mesh& mesh, const MeshNode& node)
{
    const auto& indices = node.pointIndexCount ? meshGeometry(mesh).pointIndices : node.lineIndexCount ? meshGeometry(mesh).lineIndices : meshGeometry(mesh).indices;
    const size_t offset = node.pointIndexCount ? node.pointIndexOffset : node.lineIndexCount ? node.lineIndexOffset : node.indexOffset;
    const size_t count = node.pointIndexCount ? node.pointIndexCount : node.lineIndexCount ? node.lineIndexCount : node.indexCount;
    if (offset > indices.size() || count > indices.size() - offset) {
        throw std::runtime_error("Invalid model group index range.");
    }
    return std::span<const uint32_t>(indices).subspan(offset, count);
}

bool finitePosition(const std::array<float, 3>& position) noexcept
{
    return std::isfinite(position[0])
        && std::isfinite(position[1])
        && std::isfinite(position[2]);
}

bool validNormal(const std::array<float, 3>& normal) noexcept
{
    if (!std::isfinite(normal[0]) || !std::isfinite(normal[1]) || !std::isfinite(normal[2])) {
        return false;
    }

    return std::abs(normal[0]) > 0.000001f
        || std::abs(normal[1]) > 0.000001f
        || std::abs(normal[2]) > 0.000001f;
}

std::array<float, 3> calculateFaceNormal(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const std::array<float, 3>& c)
{
    auto normal = cross(subtract(b, a), subtract(c, a));
    normalize(normal);
    return normal;
}

void generateSmoothNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
    const ModelLoadProgressCallback& progress)
{
    const auto total = vertices.size() * 2 + indices.size();
    const auto caller = std::this_thread::get_id();
    const auto vertexPass = [&](size_t offset, const auto& operation) {
        std::atomic<size_t> finished{0};
        parallelWorkBatches(vertices.size(), 16384, [&] {
            // Load callbacks belong to the caller. A thrown cancellation joins
            // all helpers before the caller can release the mesh.
            if (progress && std::this_thread::get_id() == caller) {
                reportModelLoadProgress(progress, ModelLoadStage::normals,
                    offset + finished.load(std::memory_order_relaxed), total);
            }
        }, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) { operation(vertices[i].normal); }
            finished.fetch_add(end - begin, std::memory_order_relaxed);
        }, 131072);
    };
    vertexPass(0, [](auto& normal) { normal = {0.0f, 0.0f, 0.0f}; });
    size_t completed = vertices.size();

    const size_t faceCount = indices.size() / 3;
    constexpr size_t faceBatchSize = 16384;
    if (faceCount < faceBatchSize) {
        reportModelLoadProgress(progress, ModelLoadStage::normals, completed, total);
        for (size_t face = 0; face < faceCount; ++face) {
            const size_t index = face * 3;
            Vertex& a = vertices[indices[index]], &b = vertices[indices[index + 1]], &c = vertices[indices[index + 2]];
            const auto normal = calculateFaceNormal(a.position, b.position, c.position);
            add(a.normal, normal); add(b.normal, normal); add(c.normal, normal);
        }
    } else {
        // Each live batch uses 192 KiB. Three helpers plus the caller overlap face
        // calculation with ordered accumulation, with no full-mesh scratch buffer.
        std::atomic<size_t> nextFace{0};
        constexpr size_t canceled = std::numeric_limits<size_t>::max();
        const auto fail = [&] {
            nextFace.store(canceled, std::memory_order_release);
            nextFace.notify_all();
        };
        parallelWorkBatches(faceCount, faceBatchSize, [&] {
            try {
                const size_t finished = nextFace.load(std::memory_order_relaxed);
                if (finished != canceled && progress && std::this_thread::get_id() == caller) {
                    reportModelLoadProgress(progress, ModelLoadStage::normals, completed + finished * 3, total);
                }
            } catch (...) { fail(); throw; }
        }, [&](size_t begin, size_t end) {
            try {
                std::array<std::array<float, 3>, faceBatchSize> faceNormals;
                calculateFaceNormals(vertices, indices, begin, end - begin, faceNormals.data());
                // Acquire the preceding batch's writes before adding to shared
                // vertices. Face and corner order match the serial algorithm.
                for (size_t ready = nextFace.load(std::memory_order_acquire); ready != begin;
                     ready = nextFace.load(std::memory_order_acquire)) {
                    if (ready == canceled) { return; }
                    nextFace.wait(ready, std::memory_order_acquire);
                }
                for (size_t face = begin; face < end; ++face) {
                    const size_t index = face * 3;
                    const auto& normal = faceNormals[face - begin];
                    add(vertices[indices[index]].normal, normal);
                    add(vertices[indices[index + 1]].normal, normal);
                    add(vertices[indices[index + 2]].normal, normal);
                }
                // A cancellation can arrive during accumulation. Never overwrite
                // its sentinel: all waiting batches must wake and leave.
                size_t expected = begin;
                nextFace.compare_exchange_strong(expected, end, std::memory_order_release, std::memory_order_relaxed);
                nextFace.notify_all();
            } catch (...) { fail(); throw; }
        }, 131072, 3);
    }

    completed += indices.size();
    vertexPass(completed, [](auto& normal) { normalize(normal); });
    reportModelLoadProgress(progress, ModelLoadStage::normals, total, total);
}

Bounds calculateBounds(const std::vector<Vertex>& vertices, const ModelLoadProgressCallback& progress)
{
    if (vertices.empty()) {
        throw std::runtime_error("Cannot calculate bounds for an empty mesh.");
    }

    Bounds bounds{};
    bounds.min = vertices.front().position;
    bounds.max = vertices.front().position;
    auto reduction = floatBoundsReduction(bounds.min, bounds.max);

    size_t completed = 0;
    const auto total = vertices.size() * 2;
    for (const auto& vertex : vertices) {
        if (completed % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::bounds, completed, total); }
        ++completed;
        if (!finitePosition(vertex.position)) { throw std::runtime_error("Non-finite render coordinate."); }
        expand(reduction, vertex.position);
    }
    const auto box = boundsResult(reduction);
    bounds.min = box[0]; bounds.max = box[1];

    for (size_t axis = 0; axis < 3u; ++axis) {
        bounds.center[axis] = (bounds.min[axis] + bounds.max[axis]) * 0.5f;
    }

    float radiusSquared = 0.0f;
    for (const auto& vertex : vertices) {
        if (completed % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::bounds, completed, total); }
        ++completed;
        const auto offset = subtract(vertex.position, bounds.center);
        radiusSquared = std::max(radiusSquared, offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
    }

    bounds.radius = std::max(std::sqrt(radiusSquared), 0.001f);
    reportModelLoadProgress(progress, ModelLoadStage::bounds, total, total);
    return bounds;
}

void captureSourceMesh(Mesh& mesh, SourceProvenance provenance, std::span<const uint64_t> originalPointIds)
{
    auto data = std::make_shared<SourceMeshData>();
    data->provenance = provenance;
    auto& g = editMeshGeometry(mesh);
    if (!g.sourcePositions && g.precisePositions.size() == g.vertices.size()) {
        data->draftPositions = std::move(g.precisePositions);
    } else {
        data->draftPositions.reserve(g.vertices.size());
        for (size_t i = 0; i < g.vertices.size(); ++i) { data->draftPositions.push_back(meshPosition(mesh, i)); }
    }
    publishSourcePositions(*data);
    g.sourcePositions = data->positions;
    std::vector<Coordinate>().swap(g.precisePositions);
    std::vector<uint32_t>().swap(g.positionIndices);
    g.positionIndexing = PositionIndexing::identity;
    data->indicesPending = mesh.preparationDeferred;
    if (!data->indicesPending) { data->indices = meshGeometry(mesh).indices; }
    data->originalPointIds.assign(originalPointIds.begin(), originalPointIds.end());
    mesh.sourceData = std::move(data);
}

void finalizeMesh(Mesh& mesh, bool generateMissingSmoothNormals, const ModelLoadProgressCallback& progress)
{
    renewMeshContentRevision(mesh);
    if (empty(mesh)) {
        throw std::runtime_error("Mesh did not contain renderable triangles, lines or points.");
    }

    reportModelLoadProgress(progress, ModelLoadStage::normals);
    if (generateMissingSmoothNormals && !hasCompleteNormals(meshGeometry(mesh).vertices)) {
        generateSmoothNormals(editMeshGeometry(mesh).vertices, meshGeometry(mesh).indices, progress);
    }

    reportModelLoadProgress(progress, ModelLoadStage::bounds);
    mesh.bounds = calculateBounds(meshGeometry(mesh).vertices, progress);
    mesh.originalBounds.reset();
    if (!mesh.preparationDeferred) { mesh.originalBounds = originalMeshBounds(mesh); }
    recordMeshBounds(mesh);
}

} // namespace woby
