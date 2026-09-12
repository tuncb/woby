#include "importer_host.h"
#include "utf8_path.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
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

bool hasImporterForPath(const std::filesystem::path& path)
{
    auto& host = registry();
    std::lock_guard lock(host.mutex);
    return std::any_of(host.entries.begin(), host.entries.end(), [&](const Importer& entry) { return supports(entry.info, path); });
}

Mesh copyImportedMesh(const WobyImportResult& result)
{
    constexpr uint64_t maxBytes = 256u * 1024u * 1024u;
    const uint64_t bytes = uint64_t(result.vertex_count) * sizeof(WobyImportVertex)
        + uint64_t(result.index_count) * sizeof(uint32_t);
    if (result.struct_size < sizeof(WobyImportResult) || (result.flags & ~3u) != 0u
        || result.vertex_count == 0u || result.index_count == 0u || result.index_count % 3u != 0u
        || result.vertices == nullptr || result.indices == nullptr || bytes > maxBytes
        || result.group_count > 100000u || (result.group_count != 0u && result.groups == nullptr)) {
        throw std::runtime_error("Invalid importer mesh buffers, flags or size limits.");
    }
    Mesh mesh;
    mesh.vertices.resize(result.vertex_count);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& source = result.vertices[i];
        auto& vertex = mesh.vertices[i];
        for (size_t axis = 0; axis < 3u; ++axis) {
            if (!std::isfinite(source.position[axis]) || std::abs(source.position[axis]) > 1.0e9f) {
                throw std::runtime_error("Invalid importer vertex position.");
            }
            vertex.position[axis] = source.position[axis];
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
    mesh.indices.assign(result.indices, result.indices + result.index_count);
    for (uint32_t index : mesh.indices) {
        if (index >= result.vertex_count) {
            throw std::runtime_error("Importer index is outside the vertex buffer.");
        }
    }
    uint32_t nextIndex = 0;
    size_t nameBytes = 0;
    std::set<std::string> groupNames;
    for (uint32_t i = 0; i < result.group_count; ++i) {
        const auto& group = result.groups[i];
        if (group.index_offset != nextIndex || group.index_count == 0u || group.index_count % 3u != 0u
            || group.index_count > result.index_count - nextIndex) {
            throw std::runtime_error("Importer groups must partition the triangle buffer in order.");
        }
        auto name = boundedString(group.name);
        nameBytes += name.size();
        if (name.empty() || nameBytes > 1024u * 1024u || !groupNames.insert(name).second) {
            throw std::runtime_error("Importer group names must be unique, nonempty and within size limits.");
        }
        mesh.nodes.push_back({std::move(name), nextIndex, group.index_count});
        nextIndex += group.index_count;
    }
    if (result.group_count == 0u) {
        mesh.nodes.push_back({"Mesh", 0u, result.index_count});
    } else if (nextIndex != result.index_count) {
        throw std::runtime_error("Importer groups do not cover all triangles.");
    }
    captureSourceMesh(mesh, SourceProvenance::importerVertices);
    // Remove unused vertices before the existing optimizer (which allocates its
    // remap table using index count). Keep triangle and group order unchanged.
    std::vector<uint32_t> remap(mesh.vertices.size(), std::numeric_limits<uint32_t>::max());
    std::vector<Vertex> usedVertices;
    usedVertices.reserve(std::min(mesh.vertices.size(), mesh.indices.size()));
    for (auto& index : mesh.indices) {
        auto& mapped = remap[index];
        if (mapped == std::numeric_limits<uint32_t>::max()) {
            mapped = static_cast<uint32_t>(usedVertices.size());
            usedVertices.push_back(mesh.vertices[index]);
        }
        index = mapped;
    }
    mesh.vertices = std::move(usedVertices);
    finalizeMesh(mesh, (result.flags & WOBY_IMPORT_HAS_NORMALS) == 0u);
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
    model.mesh = copyImportedMesh(result);
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
