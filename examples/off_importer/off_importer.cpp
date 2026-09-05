#include "woby/importer.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct OffResult {
    std::vector<WobyImportVertex> vertices;
    std::vector<uint32_t> indices;
};

uint32_t WOBY_IMPORT_CALL importOff(const WobyImportRequest* request, WobyImportResult* result)
{
    try {
        auto storage = std::make_unique<OffResult>();
        const std::string_view pathText(request->path);
        std::ifstream stream{std::filesystem::path(std::u8string(pathText.begin(), pathText.end()))};
        std::string header;
        uint32_t vertices = 0;
        uint32_t faces = 0;
        uint32_t edges = 0;
        if (!(stream >> header >> vertices >> faces >> edges) || header != "OFF"
            || vertices == 0u || faces == 0u || vertices > 1000000u || faces > 1000000u) {
            result->error = "Expected OFF header and 1..1000000 vertices/faces.";
            return WOBY_IMPORT_ERROR;
        }
        storage->vertices.resize(vertices);
        storage->indices.reserve(size_t(faces) * 3u);
        for (uint32_t i = 0; i < vertices; ++i) {
            if (request->is_canceled(request->context)) { return WOBY_IMPORT_CANCELED; }
            auto& v = storage->vertices[i];
            if (!(stream >> v.position[0] >> v.position[1] >> v.position[2])) {
                result->error = "Invalid OFF vertex.";
                return WOBY_IMPORT_ERROR;
            }
        }
        for (uint32_t i = 0; i < faces; ++i) {
            if (request->is_canceled(request->context)) { return WOBY_IMPORT_CANCELED; }
            uint32_t count = 0;
            uint32_t a = 0, b = 0, c = 0;
            if (!(stream >> count) || count != 3u || !(stream >> a >> b >> c)
                || a >= vertices || b >= vertices || c >= vertices) {
                result->error = "This example supports triangular OFF faces only.";
                return WOBY_IMPORT_ERROR;
            }
            storage->indices.insert(storage->indices.end(), {a, b, c});
            request->report_progress(request->context, static_cast<float>(i + 1u) / static_cast<float>(faces));
        }
        result->vertices = storage->vertices.data();
        result->vertex_count = vertices;
        result->indices = storage->indices.data();
        result->index_count = static_cast<uint32_t>(storage->indices.size());
        result->user_data = storage.release();
        return WOBY_IMPORT_OK;
    } catch (...) {
        result->error = "Cannot read OFF file or allocate its mesh.";
        return WOBY_IMPORT_ERROR;
    }
}

void WOBY_IMPORT_CALL releaseOff(WobyImportResult* result)
{
    delete static_cast<OffResult*>(result->user_data);
    *result = {};
}

const WobyImporterApi api = {
    sizeof(WobyImporterApi), WOBY_IMPORTER_ABI_VERSION,
    "org.woby.example.off", "OFF triangle importer", "1.0.0", "off",
    importOff, releaseOff,
};

} // namespace

extern "C" WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api : nullptr;
}
