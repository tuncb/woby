#include "importer_host.h"
#include "analysis_index.h"
#include "freeform.h"
#include "utf8_path.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <set>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace woby {
namespace {

struct Importer {
    ImporterInfo info;
    WobyImporterApi api{};
    decltype(WobyImporterApiWithHierarchy::get_hierarchy) getHierarchy = nullptr;
    decltype(WobyImporterApiWithLines::get_lines) getLines = nullptr;
    decltype(WobyImporterApiWithPointIds::get_point_ids) getPointIds = nullptr;
    decltype(WobyImporterApiWithGeometry::get_points) getPoints = nullptr;
    decltype(WobyImporterApiWithGeometry::get_freeform) getFreeform = nullptr;
    std::shared_ptr<void> library;
    std::shared_ptr<std::mutex> callMutex;
};

struct ImporterRegistry {
    std::mutex mutex;
    std::vector<Importer> entries;
};

ImporterRegistry& registry()
{
    static ImporterRegistry value;
    return value;
}

std::string utf8(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string lowercase(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

std::string boundedString(const char* value, size_t limit = 4096u)
{
    if (value == nullptr) {
        throw std::runtime_error("Importer returned a null string.");
    }
    size_t length = 0;
    while (length < limit && value[length] != '\0') {
        ++length;
    }
    if (length == limit) {
        throw std::runtime_error("Importer string exceeds the size limit.");
    }
    return {value, length};
}

bool supports(const ImporterInfo& info, const std::filesystem::path& path)
{
    const std::string extension = lowercase(utf8(path.extension()));
    return extension.size() > 1u
        && std::find(info.extensions.begin(), info.extensions.end(), extension.substr(1u)) != info.extensions.end();
}

std::filesystem::path importerManifestLibrary(const std::filesystem::path& manifest)
{
    if (!std::filesystem::is_regular_file(manifest) || std::filesystem::file_size(manifest) > 64u * 1024u) {
        throw std::runtime_error("Importer manifest must be a file of at most 64 KiB.");
    }
    std::ifstream stream(manifest, std::ios::binary);
    const auto data = nlohmann::json::parse(stream);
    if (!data.at("schema").is_number_integer() || data.at("schema") != 1) {
        throw std::runtime_error("Unsupported importer manifest schema.");
    }
    const auto library = data.at("library").get<std::string>();
    const auto relative = pathFromUtf8(library);
    if (library.empty() || relative.has_root_path()
        || library.find_first_of("\\:") != std::string::npos
        || std::any_of(library.begin(), library.end(), [](unsigned char c) { return c < 32; })
        || std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; })) {
        throw std::runtime_error("Importer library must be a relative path inside its package, using forward slashes.");
    }
    const auto root = std::filesystem::canonical(manifest.parent_path());
    const auto path = std::filesystem::canonical(root / relative);
    const auto contained = path.lexically_relative(root);
    if (contained.empty() || contained.is_absolute() || *contained.begin() == ".."
        || !std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Importer library must be a file inside its package.");
    }
    return path;
}

struct CallbackContext {
    const ImportCallbacks* callbacks;
    std::exception_ptr error;
};

uint32_t WOBY_IMPORT_CALL isCanceled(void* opaque)
{
    auto& context = *static_cast<CallbackContext*>(opaque);
    try {
        return context.error || (context.callbacks->canceled && context.callbacks->canceled()) ? 1u : 0u;
    } catch (...) {
        context.error = std::current_exception();
        return 1u;
    }
}

void WOBY_IMPORT_CALL reportProgress(void* opaque, float fraction)
{
    auto& context = *static_cast<CallbackContext*>(opaque);
    try {
        if (!context.error && context.callbacks->progress && std::isfinite(fraction)) {
            context.callbacks->progress(std::clamp(fraction, 0.0f, 1.0f));
        }
    } catch (...) {
        context.error = std::current_exception();
    }
}

MeshNode importGroup(const WobyImportGroup& group, std::set<std::string>& names, size_t& nameBytes)
{
    MeshNode node;
    node.name = boundedString(group.name);
    nameBytes += node.name.size();
    if (node.name.empty() || nameBytes > 1024u * 1024u || !names.insert(node.name).second) {
        throw std::runtime_error("Importer group names must be unique, nonempty and within size limits.");
    }
    if ((group.flags & ~(WOBY_IMPORT_GROUP_HAS_COLOR | WOBY_IMPORT_GROUP_INITIALLY_HIDDEN)) != 0u) {
        throw std::runtime_error("Invalid importer group flags.");
    }
    node.defaultVisible = (group.flags & WOBY_IMPORT_GROUP_INITIALLY_HIDDEN) == 0u;
    if ((group.flags & WOBY_IMPORT_GROUP_HAS_COLOR) != 0u) {
        std::array<float, 4> color;
        for (size_t i = 0; i < color.size(); ++i) {
            if (!std::isfinite(group.color[i]) || group.color[i] < 0 || group.color[i] > 1) {
                throw std::runtime_error("Importer group color components must be finite and within [0, 1].");
            }
            color[i] = group.color[i];
        }
        node.defaultColor = color;
    }
    return node;
}

struct FreeformImportBudget {
    size_t controls = 0, trimItems = 0;
};

FreeformPatch copySpline(const WobyImportSpline& input, FreeformImportBudget& budget,
    const ModelLoadProgressCallback& progress, bool trimCurve = false)
{
    if (input.struct_size < sizeof(WobyImportSpline) || input.kind > WOBY_IMPORT_SPLINE_SURFACE
        || (input.flags & ~(WOBY_IMPORT_HAS_NORMALS | WOBY_IMPORT_HAS_TEXCOORDS)) != 0
        || input.degree_u < 1 || input.degree_u > maxFreeformDegree || input.count_u <= input.degree_u
        || input.control_count > maxFreeformVertices - budget.controls || input.controls == nullptr
        || uint64_t(input.count_u) * input.count_v != input.control_count
        || uint64_t(input.count_u) + input.degree_u + 1 != input.knot_count_u || !input.knots_u) {
        throw std::runtime_error("Invalid importer spline buffers, dimensions or size limits.");
    }
    const bool surface = input.kind == WOBY_IMPORT_SPLINE_SURFACE;
    if (surface ? (input.degree_v < 1 || input.degree_v > maxFreeformDegree || input.count_v <= input.degree_v
            || uint64_t(input.count_v) + input.degree_v + 1 != input.knot_count_v || !input.knots_v)
        : (input.degree_v != 0 || input.count_v != 1 || input.knot_count_v != 0)) {
        throw std::runtime_error("Invalid importer spline V dimensions.");
    }
    if (trimCurve && (surface || input.flags != 0 || input.control_count > 4096u)) {
        throw std::runtime_error("Importer UV trim curves require curve geometry, no attributes and at most 4096 controls.");
    }
    budget.controls += input.control_count;
    FreeformPatch patch;
    patch.surface = surface;
    patch.degreeU = input.degree_u; patch.degreeV = input.degree_v;
    patch.countU = input.count_u; patch.countV = input.count_v;
    patch.domainU = {input.domain_u[0], input.domain_u[1]};
    patch.domainV = {input.domain_v[0], input.domain_v[1]};
    patch.knotsU.assign(input.knots_u, input.knots_u + input.knot_count_u);
    if (surface) { patch.knotsV.assign(input.knots_v, input.knots_v + input.knot_count_v); }
    patch.controls.reserve(input.control_count);
    for (uint32_t i = 0; i < input.control_count; ++i) {
        if (i % 1024 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh); }
        const auto& c = input.controls[i];
        for (double position : c.position) {
            if (!std::isfinite(position) || (!trimCurve && std::abs(position) > 1e9)) { throw std::runtime_error("Invalid importer control position."); }
        }
        if (trimCurve && c.position[2] != 0) { throw std::runtime_error("Importer trim curves must lie in UV space (z=0)."); }
        patch.controls.push_back({c.position[0], c.position[1], c.position[2], c.weight});
        if (input.flags & WOBY_IMPORT_HAS_NORMALS) { patch.normals.push_back({c.normal[0],c.normal[1],c.normal[2]}); }
        if (input.flags & WOBY_IMPORT_HAS_TEXCOORDS) { patch.texcoords.push_back({c.texcoord[0],c.texcoord[1]}); }
    }
    validateFreeformPatch(patch);
    return patch;
}

void addTrimItems(FreeformImportBudget& budget, uint32_t count, const void* data)
{
    if (count > 100000u - budget.trimItems || (count && !data)) {
        throw std::runtime_error("Invalid importer trim buffers or size limits.");
    }
    budget.trimItems += count;
}

FreeformTrimLoop copyTrimLoop(const WobyImportTrimLoop& input,
    const std::vector<std::shared_ptr<const FreeformPatch>>& curves, FreeformImportBudget& budget)
{
    addTrimItems(budget, input.segment_count, input.segments);
    FreeformTrimLoop loop;
    loop.segments.reserve(input.segment_count);
    for (uint32_t i = 0; i < input.segment_count; ++i) {
        const auto& segment = input.segments[i];
        if (segment.curve_index >= curves.size()) { throw std::runtime_error("Importer trim curve index is out of range."); }
        const auto& curve = curves[segment.curve_index];
        const auto a = segment.interval[0], b = segment.interval[1];
        if (!std::isfinite(a) || !std::isfinite(b) || a == b || std::min(a,b) < curve->domainU[0]
            || std::max(a,b) > curve->domainU[1]) { throw std::runtime_error("Invalid importer trim interval."); }
        loop.segments.push_back({curve, {a,b}});
    }
    return loop;
}

std::vector<FreeformPatch> copyFreeform(const WobyImportFreeform& input,
    std::vector<MeshNode>& groups, std::set<std::string>& names, size_t& nameBytes,
    const ModelLoadProgressCallback& progress)
{
    FreeformImportBudget budget;
    std::vector<std::shared_ptr<const FreeformPatch>> curves;
    for (uint32_t i = 0; i < input.trim_curve_count; ++i) {
        curves.push_back(std::make_shared<FreeformPatch>(copySpline(input.trim_curves[i], budget, progress, true)));
    }
    std::vector<FreeformPatch> patches;
    for (uint32_t i = 0; i < input.patch_count; ++i) {
        const auto& source = input.patches[i];
        if (source.group.index_offset || source.group.index_count) { throw std::runtime_error("Importer patch group ranges must be zero."); }
        groups.push_back(importGroup(source.group, names, nameBytes));
        auto patch = copySpline(source.spline, budget, progress);
        patch.name = groups.back().name;
        addTrimItems(budget, source.region_count, source.regions);
        if (source.region_count) {
            if (!patch.surface) { throw std::runtime_error("Importer curves cannot have trimming regions."); }
            auto trimming = std::make_shared<FreeformTrimming>();
            trimming->sourceFile = patch.name;
            for (uint32_t r = 0; r < source.region_count; ++r) {
                const auto& region = source.regions[r];
                addTrimItems(budget, region.hole_count, region.holes);
                FreeformTrimRegion target;
                target.outer = copyTrimLoop(region.outer, curves, budget);
                for (uint32_t h = 0; h < region.hole_count; ++h) {
                    if (!region.holes[h].segment_count) { throw std::runtime_error("Importer trim holes must be nonempty."); }
                    target.holes.push_back(copyTrimLoop(region.holes[h], curves, budget));
                }
                trimming->regions.push_back(std::move(target));
            }
            patch.trimming = std::move(trimming);
        }
        patches.push_back(std::move(patch));
    }
    return patches;
}

} // namespace

void loadImporter(const std::filesystem::path& path)
{
    auto& host = registry();
    std::lock_guard lock(host.mutex);
    const auto absolutePath = std::filesystem::canonical(path);
    for (const auto& entry : host.entries) {
        std::error_code error;
        if (entry.info.path == absolutePath || std::filesystem::equivalent(entry.info.path, absolutePath, error)) {
            return;
        }
    }

    Importer entry;
    entry.info.path = absolutePath;
#ifdef _WIN32
    HMODULE module = LoadLibraryExW(absolutePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        throw std::runtime_error("Cannot load importer " + utf8(path) + " (Windows error "
            + std::to_string(GetLastError()) + "). Check architecture and dependencies.");
    }
    entry.library = std::shared_ptr<void>(module, [](void* handle) { FreeLibrary(static_cast<HMODULE>(handle)); });
    const auto symbol = GetProcAddress(module, "woby_get_importer_api");
#else
    void* module = dlopen(absolutePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        throw std::runtime_error("Cannot load importer " + utf8(path) + ": " + dlerror());
    }
    entry.library = std::shared_ptr<void>(module, [](void* handle) { dlclose(handle); });
    const auto symbol = dlsym(module, "woby_get_importer_api");
#endif
    if (symbol == nullptr) {
        throw std::runtime_error("Missing woby_get_importer_api export: " + utf8(path));
    }
    WobyGetImporterApi getApi = nullptr;
    static_assert(sizeof(getApi) == sizeof(symbol));
    std::memcpy(&getApi, &symbol, sizeof(getApi));
    const WobyImporterApi* api = getApi(WOBY_IMPORTER_ABI_VERSION);
    if (api == nullptr || api->struct_size < sizeof(WobyImporterApi)
        || api->abi_version != WOBY_IMPORTER_ABI_VERSION || api->import_file == nullptr
        || api->release_result == nullptr) {
        throw std::runtime_error("Incompatible importer API: " + utf8(path));
    }
    entry.api = *api;
    if (api->struct_size >= sizeof(WobyImporterApiWithHierarchy)) {
        entry.getHierarchy = reinterpret_cast<const WobyImporterApiWithHierarchy*>(api)->get_hierarchy;
    }
    if (api->struct_size >= sizeof(WobyImporterApiWithLines)) {
        entry.getLines = reinterpret_cast<const WobyImporterApiWithLines*>(api)->get_lines;
    }
    if (api->struct_size >= sizeof(WobyImporterApiWithPointIds)) {
        entry.getPointIds = reinterpret_cast<const WobyImporterApiWithPointIds*>(api)->get_point_ids;
    }
    if (api->struct_size >= sizeof(WobyImporterApiWithGeometry)) {
        const auto* geometry = reinterpret_cast<const WobyImporterApiWithGeometry*>(api);
        entry.getPoints = geometry->get_points;
        entry.getFreeform = geometry->get_freeform;
    }
    entry.info.id = boundedString(api->id, 256u);
    entry.info.name = boundedString(api->name);
    entry.info.version = boundedString(api->version, 256u);
    if (entry.info.id.empty() || entry.info.name.empty() || entry.info.version.empty()) {
        throw std::runtime_error("Importer identity, name and version must not be empty.");
    }
    std::istringstream extensions(boundedString(api->extensions));
    std::string extension;
    while (std::getline(extensions, extension, ';')) {
        extension = lowercase(extension);
        if (extension.empty() || extension.size() > 32u
            || !std::all_of(extension.begin(), extension.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            }) || extension == "obj" || extension == "stl" || extension == "woby") {
            throw std::runtime_error("Invalid or reserved importer extension: " + extension);
        }
        if (std::find(entry.info.extensions.begin(), entry.info.extensions.end(), extension) != entry.info.extensions.end()) {
            throw std::runtime_error("Repeated importer extension: " + extension);
        }
        entry.info.extensions.push_back(extension);
    }
    if (entry.info.extensions.empty()) {
        throw std::runtime_error("Importer must advertise at least one extension.");
    }
    for (const auto& existing : host.entries) {
        if (existing.info.id == entry.info.id) {
            throw std::runtime_error("Duplicate importer ID: " + entry.info.id);
        }
        for (const auto& ext : entry.info.extensions) {
            if (std::find(existing.info.extensions.begin(), existing.info.extensions.end(), ext) != existing.info.extensions.end()) {
                throw std::runtime_error("Importer extension already registered: " + ext);
            }
        }
    }
    entry.callMutex = std::make_shared<std::mutex>();
    host.entries.push_back(std::move(entry));
}

void unloadImporters()
{
    auto& host = registry();
    std::lock_guard lock(host.mutex);
    host.entries.clear();
}

std::vector<ImporterInfo> loadedImporters()
{
    auto& host = registry();
    std::lock_guard lock(host.mutex);
    std::vector<ImporterInfo> result;
    for (const auto& entry : host.entries) {
        result.push_back(entry.info);
    }
    return result;
}

std::vector<std::filesystem::path> discoverImporterFiles(const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        const auto extension = lowercase(utf8(entry.path().extension()));
#ifdef _WIN32
        const bool library = extension == ".dll";
#elif defined(__APPLE__)
        const bool library = extension == ".dylib" || extension == ".so";
#else
        const bool library = extension == ".so";
#endif
        if (library && entry.is_regular_file()) {
            result.push_back(entry.path());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> loadPortableImporters(const std::filesystem::path& folder)
{
    std::vector<std::string> errors;
    std::vector<std::filesystem::path> packages;
    try {
        if (!std::filesystem::exists(folder)) { return errors; }
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            packages.push_back(entry.path());
        }
    } catch (const std::exception& error) {
        errors.push_back(utf8(folder) + ": " + error.what());
    }
    std::sort(packages.begin(), packages.end());
    for (const auto& package : packages) {
        const auto manifest = package / "importer.json";
        try {
            if (!std::filesystem::is_directory(package) || !std::filesystem::exists(manifest)) { continue; }
            loadImporter(importerManifestLibrary(manifest));
        } catch (const std::exception& error) {
            errors.push_back(utf8(manifest) + ": " + error.what());
        }
    }
    return errors;
}

bool hasImporterForPath(const std::filesystem::path& path)
{
    auto& host = registry();
    std::lock_guard lock(host.mutex);
    return std::any_of(host.entries.begin(), host.entries.end(), [&](const Importer& entry) { return supports(entry.info, path); });
}

Mesh copyImportedMesh(const WobyImportResult& result, const WobyImportHierarchy* hierarchy,
    const WobyImportLines* lines, const WobyImportPointIds* pointIds,
    const WobyImportPoints* points, const WobyImportFreeform* freeform, const ModelLoadProgressCallback& progress)
{
    if (lines && lines->struct_size < sizeof(WobyImportLines)) {
        throw std::runtime_error("Invalid importer line buffers or size limits.");
    }
    if (points && (points->struct_size < sizeof(WobyImportPoints)
        || (points->index_count && !points->indices) || points->group_count > 100000u
        || (points->group_count && !points->groups))) {
        throw std::runtime_error("Invalid importer point buffers or size limits.");
    }
    if (freeform && (freeform->struct_size < sizeof(WobyImportFreeform)
        || freeform->patch_count > 100000u || freeform->trim_curve_count > 100000u
        || (freeform->patch_count && !freeform->patches) || (freeform->trim_curve_count && !freeform->trim_curves))) {
        throw std::runtime_error("Invalid importer freeform buffers or size limits.");
    }
    if (pointIds && (pointIds->struct_size < sizeof(WobyImportPointIds)
        || pointIds->vertex_count != result.vertex_count || (pointIds->vertex_count && pointIds->ids == nullptr))) {
        throw std::runtime_error("Invalid importer point ID buffer or vertex count.");
    }
    constexpr uint64_t maxBytes = 4ull * 1024u * 1024u * 1024u;
    const uint64_t bytes = uint64_t(result.vertex_count) * sizeof(WobyImportVertex)
        + uint64_t(result.index_count) * sizeof(uint32_t)
        + (lines ? uint64_t(lines->index_count) * sizeof(uint32_t) : 0)
        + (points ? uint64_t(points->index_count) * sizeof(uint32_t) : 0)
        + (pointIds ? uint64_t(pointIds->vertex_count) * sizeof(uint64_t) : 0);
    if (result.struct_size < sizeof(WobyImportResult) || (result.flags & ~3u) != 0u
        || (result.index_count == 0u && (!lines || !lines->index_count) && (!points || !points->index_count)
            && (!freeform || !freeform->patch_count)) || result.index_count % 3u != 0u
        || (result.vertex_count && result.vertices == nullptr) || (result.index_count != 0u && result.indices == nullptr) || bytes > maxBytes
        || result.group_count > 100000u || (result.group_count != 0u && result.groups == nullptr)) {
        throw std::runtime_error("Invalid importer mesh buffers, flags or size limits.");
    }
    const auto effectiveGroupCount = [](uint32_t groups, uint32_t indices) { return groups ? groups : (indices ? 1u : 0u); };
    if (lines && (lines->index_count % 2u != 0u
        || (lines->index_count != 0u && lines->indices == nullptr)
        || uint64_t(effectiveGroupCount(result.group_count, result.index_count))
            + effectiveGroupCount(lines->group_count, lines->index_count) > 100000u
        || (lines->group_count != 0u && lines->groups == nullptr))) {
        throw std::runtime_error("Invalid importer line buffers or size limits.");
    }
    if (uint64_t(effectiveGroupCount(result.group_count, result.index_count))
        + (lines ? effectiveGroupCount(lines->group_count, lines->index_count) : 0u)
        + (points ? effectiveGroupCount(points->group_count, points->index_count) : 0u)
        + (freeform ? freeform->patch_count : 0u) > 100000u) {
        throw std::runtime_error("Importer exceeds combined group size limits.");
    }
    Mesh mesh;
    mesh.vertices.resize(result.vertex_count);
    mesh.precisePositions.resize(result.vertex_count);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (i % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, i, result.vertex_count); }
        const auto& source = result.vertices[i];
        auto& vertex = mesh.vertices[i];
        for (size_t axis = 0; axis < 3u; ++axis) {
            if (!std::isfinite(source.position[axis]) || std::abs(source.position[axis]) > 1.0e9f) {
                throw std::runtime_error("Invalid importer vertex position.");
            }
            mesh.precisePositions[i][axis] = source.position[axis];
            if ((result.flags & WOBY_IMPORT_HAS_NORMALS) != 0u) {
                vertex.normal[axis] = source.normal[axis];
            }
        }
        if ((result.flags & WOBY_IMPORT_HAS_NORMALS) != 0u) {
            if (!finitePosition(vertex.normal)) { throw std::runtime_error("Invalid importer vertex normal."); }
            const double length = std::hypot(double(vertex.normal[0]), double(vertex.normal[1]), double(vertex.normal[2]));
            if (length == 0.0) { throw std::runtime_error("Invalid importer vertex normal."); }
            for (float& component : vertex.normal) { component = static_cast<float>(component / length); }
        }
        for (size_t axis = 0; axis < 2u; ++axis) {
            if ((result.flags & WOBY_IMPORT_HAS_TEXCOORDS) != 0u) {
                if (!std::isfinite(source.texcoord[axis])) {
                    throw std::runtime_error("Invalid importer texture coordinate.");
                }
                vertex.texcoord[axis] = source.texcoord[axis];
            }
        }
    }
    if (pointIds) {
        AnalysisIndex<uint64_t, 1> identities;
        reserveAnalysisIndex(identities, result.vertex_count);
        std::vector<size_t> representatives;
        representatives.reserve(result.vertex_count);
        for (size_t i = 0; i < result.vertex_count; ++i) {
            const auto identity = analysisIndex(identities, std::array<uint64_t, 1>{pointIds->ids[i]});
            if (identity == representatives.size()) { representatives.push_back(i); }
            else if (mesh.precisePositions[i] != mesh.precisePositions[representatives[identity]]) {
                throw std::runtime_error("Importer vertices sharing a point ID must have identical positions.");
            }
        }
    }
    if (result.index_count) { mesh.indices.assign(result.indices, result.indices + result.index_count); }
    for (uint32_t index : mesh.indices) {
        if (index >= result.vertex_count) {
            throw std::runtime_error("Importer index is outside the vertex buffer.");
        }
    }
    size_t nameBytes = 0;
    std::set<std::string> groupNames;
    const auto appendGroups = [&](const WobyImportGroup* groups, uint32_t count, uint32_t indexCount, uint32_t primitiveSize) {
        uint32_t nextIndex = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& group = groups[i];
            if (group.index_offset != nextIndex || group.index_count == 0u || group.index_count % primitiveSize != 0u
                || group.index_count > indexCount - nextIndex) {
                throw std::runtime_error("Importer groups must partition the primitive buffer in order.");
            }
            auto node = importGroup(group, groupNames, nameBytes);
            if (primitiveSize == 3) { node.indexOffset = nextIndex; node.indexCount = group.index_count; }
            else if (primitiveSize == 2) { node.lineIndexOffset = nextIndex; node.lineIndexCount = group.index_count; }
            else { node.pointIndexOffset = nextIndex; node.pointIndexCount = group.index_count; }
            mesh.nodes.push_back(std::move(node));
            nextIndex += group.index_count;
        }
        if (count == 0u && indexCount != 0u) {
            const WobyImportGroup group{primitiveSize == 3 ? "Mesh" : (primitiveSize == 2 ? "Lines" : "Points"),0,0,0,{}};
            auto node = importGroup(group, groupNames, nameBytes);
            if (primitiveSize == 3) { node.indexCount = indexCount; }
            else if (primitiveSize == 2) { node.lineIndexCount = indexCount; }
            else { node.pointIndexCount = indexCount; }
            mesh.nodes.push_back(std::move(node));
        } else if (nextIndex != indexCount) {
            throw std::runtime_error("Importer groups do not cover all primitives.");
        }
    };
    appendGroups(result.groups, result.group_count, result.index_count, 3);
    if (lines) {
        if (lines->index_count) { mesh.lineIndices.assign(lines->indices, lines->indices + lines->index_count); }
        for (const auto index : mesh.lineIndices) {
            if (index >= result.vertex_count) { throw std::runtime_error("Importer line index is outside the vertex buffer."); }
        }
        appendGroups(lines->groups, lines->group_count, lines->index_count, 2);
    }
    if (points) {
        if (points->index_count) { mesh.pointIndices.assign(points->indices, points->indices + points->index_count); }
        for (const auto index : mesh.pointIndices) {
            if (index >= result.vertex_count) { throw std::runtime_error("Importer point index is outside the vertex buffer."); }
        }
        appendGroups(points->groups, points->group_count, points->index_count, 1);
    }
    std::vector<MeshNode> patchGroups;
    auto patches = freeform ? copyFreeform(*freeform, patchGroups, groupNames, nameBytes, progress) : std::vector<FreeformPatch>{};
    // Choose the working frame from both ordinary positions and spline controls.
    std::vector<Coordinate> extent;
    const auto extend = [&](const Coordinate& p) {
        if (extent.empty()) { extent = {p,p}; }
        for (size_t k = 0; k < 3; ++k) { extent[0][k] = std::min(extent[0][k],p[k]); extent[1][k] = std::max(extent[1][k],p[k]); }
    };
    for (const auto& p : mesh.precisePositions) { extend(p); }
    for (const auto& patch : patches) for (const auto& p : patch.controls) { extend({p[0],p[1],p[2]}); }
    // Finish ordinary geometry before appending patches, preserving authored spline normals/UVs.
    localizeMesh(mesh);
    rebaseMesh(mesh, coordinateOrigin(extent));
    for (auto& node : mesh.nodes) { node.hasTexcoords = node.indexCount != 0u && (result.flags & WOBY_IMPORT_HAS_TEXCOORDS) != 0u; }
    captureSourceMesh(mesh, SourceProvenance::importerVertices,
        pointIds ? std::span<const uint64_t>{pointIds->ids, pointIds->vertex_count} : std::span<const uint64_t>{});
    if ((result.flags & WOBY_IMPORT_HAS_NORMALS) == 0u) { generateSmoothNormals(mesh.vertices, mesh.indices, progress); }
    if (freeform) {
        const size_t first = mesh.nodes.size();
        appendFreeformGeometry(mesh, std::move(patches), progress);
        for (size_t i = 0; i < patchGroups.size(); ++i) {
            mesh.nodes[first+i].defaultColor = patchGroups[i].defaultColor;
            mesh.nodes[first+i].defaultVisible = patchGroups[i].defaultVisible;
        }
    }
    if (hierarchy != nullptr) {
        if (hierarchy->struct_size < sizeof(WobyImportHierarchy) || hierarchy->node_count == 0u
            || hierarchy->node_count > 100000u || hierarchy->nodes == nullptr) {
            throw std::runtime_error("Invalid importer hierarchy buffers or size limits.");
        }
        std::vector<uint32_t> depths;
        std::vector<bool> seen(mesh.nodes.size(), false);
        size_t hierarchyNameBytes = 0;
        mesh.hierarchy.reserve(hierarchy->node_count);
        depths.reserve(hierarchy->node_count);
        for (uint32_t i = 0; i < hierarchy->node_count; ++i) {
            const auto& source = hierarchy->nodes[i];
            auto name = boundedString(source.name);
            hierarchyNameBytes += name.size();
            if (name.empty() || hierarchyNameBytes > 1024u * 1024u) {
                throw std::runtime_error("Importer hierarchy labels must be nonempty and within size limits.");
            }
            uint32_t depth = 1u;
            if (source.parent_index != WOBY_IMPORT_NO_PARENT) {
                if (source.parent_index >= i
                    || mesh.hierarchy[source.parent_index].groupIndex != WOBY_IMPORT_NO_GROUP) {
                    throw std::runtime_error("Importer hierarchy parents must be preceding container nodes.");
                }
                depth = depths[source.parent_index] + 1u;
            }
            if (depth > WOBY_IMPORT_MAX_HIERARCHY_DEPTH) {
                throw std::runtime_error("Importer hierarchy exceeds the maximum depth of 128.");
            }
            if (source.group_index != WOBY_IMPORT_NO_GROUP) {
                if (source.group_index >= seen.size() || seen[source.group_index]) {
                    throw std::runtime_error("Importer hierarchy must reference each mesh group exactly once.");
                }
                seen[source.group_index] = true;
                mesh.nodes[source.group_index].displayName = name;
            }
            mesh.hierarchy.push_back({std::move(name), source.parent_index, source.group_index});
            depths.push_back(depth);
        }
        if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
            throw std::runtime_error("Importer hierarchy must reference each mesh group exactly once.");
        }
    }
    finalizeMesh(mesh, false, progress);
    return mesh;
}

ImportedModel importModel(const std::filesystem::path& path, const std::string& requiredImporterId,
    const ImportCallbacks& callbacks)
{
    Importer importer;
    {
        auto& host = registry();
        std::lock_guard lock(host.mutex);
        const auto found = std::find_if(host.entries.begin(), host.entries.end(), [&](const Importer& entry) {
            return requiredImporterId.empty() ? supports(entry.info, path) : entry.info.id == requiredImporterId;
        });
        if (found == host.entries.end()) {
            throw std::runtime_error(requiredImporterId.empty()
                ? "Unsupported model file extension: " + path.string()
                : "Required importer is not loaded: " + requiredImporterId);
        }
        if (!supports(found->info, path)) {
            throw std::runtime_error("Required importer no longer supports this file: " + requiredImporterId);
        }
        importer = *found;
    }
    std::lock_guard callLock(*importer.callMutex);
    ImportedModel model;
    model.importerId = importer.info.id;
    CallbackContext context{&callbacks, {}};
    if (isCanceled(&context) != 0u) {
        if (context.error) { std::rethrow_exception(context.error); }
        model.canceled = true;
        return model;
    }
    const std::string absolutePath = utf8(std::filesystem::absolute(path));
    const WobyImportRequest request{sizeof(WobyImportRequest), absolutePath.c_str(), &context, isCanceled, reportProgress};
    WobyImportResult result{};
    result.struct_size = sizeof(result);
    const auto release = [&](WobyImportResult* value) { importer.api.release_result(value); };
    std::unique_ptr<WobyImportResult, decltype(release)> resultOwner(&result, release);
    const uint32_t status = importer.api.import_file(&request, &result);
    const bool canceled = isCanceled(&context) != 0u;
    if (context.error) {
        std::rethrow_exception(context.error);
    }
    if (status == WOBY_IMPORT_CANCELED || canceled) {
        model.canceled = true;
        return model;
    }
    if (status != WOBY_IMPORT_OK) {
        throw std::runtime_error("Importer " + importer.info.id + ": "
            + (result.error ? boundedString(result.error) : "Import failed."));
    }
    struct Canceled {};
    const auto progress = [&](const ModelLoadProgress& update) {
        if (isCanceled(&context)) {
            if (context.error) { std::rethrow_exception(context.error); }
            throw Canceled{};
        }
        if (callbacks.stageProgress) { callbacks.stageProgress(update); }
    };
    try {
        model.mesh = copyImportedMesh(result, importer.getHierarchy ? importer.getHierarchy(&result) : nullptr,
            importer.getLines ? importer.getLines(&result) : nullptr,
            importer.getPointIds ? importer.getPointIds(&result) : nullptr,
            importer.getPoints ? importer.getPoints(&result) : nullptr,
            importer.getFreeform ? importer.getFreeform(&result) : nullptr, progress);
    } catch (const Canceled&) { model.mesh = {}; model.canceled = true; }
    if (isCanceled(&context) != 0u) {
        if (context.error) { std::rethrow_exception(context.error); }
        model.mesh = {};
        model.canceled = true;
    }
    return model;
}

std::vector<std::filesystem::path> readImporterSettings(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) { return {}; }
    std::ifstream stream(path);
    if (!stream) { throw std::runtime_error("Cannot read importer settings."); }
    std::vector<std::filesystem::path> result;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) { continue; }
        std::istringstream record(line);
        std::string value;
        if (!(record >> std::quoted(value)) || value.empty() || !(record >> std::ws).eof()) {
            throw std::runtime_error("Invalid importer settings record.");
        }
        result.push_back(woby::pathFromUtf8(value));
    }
    if (stream.bad()) { throw std::runtime_error("Cannot read importer settings."); }
    return result;
}

void writeImporterSettings(const std::filesystem::path& path, const std::vector<std::filesystem::path>& plugins)
{
    const auto temporary = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream stream(temporary, std::ios::trunc);
        for (const auto& plugin : plugins) {
            stream << std::quoted(utf8(std::filesystem::absolute(plugin))) << '\n';
        }
        stream.close();
        if (!stream) { throw std::runtime_error("Cannot write importer settings."); }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace importer settings.");
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

} // namespace woby
