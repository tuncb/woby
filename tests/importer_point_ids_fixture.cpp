#include "woby/importer.h"

#include <cstring>
#include <vector>

namespace {
struct Data {
    // Two closed tetrahedra sharing a face with opposite normals and winding.
    std::vector<WobyImportVertex> vertices = {
        {{0,0,0},{0,0,-1},{}}, {{1,0,0},{0,0,-1},{}},
        {{0,1,0},{0,0,-1},{}}, {{0,0,1},{0,0,1},{}},
        {{-0.0,0,0},{0,0,1},{1,1}}, {{1,0,0},{0,0,1},{1,0}},
        {{0,1,0},{0,0,1},{0,1}}, {{0,0,-1},{0,0,-1},{}},
        {{0,0,0},{1,0,0},{}}, // Optional unused, genuinely distinct point.
    };
    std::vector<uint64_t> ids = {0,UINT64_MAX,42,99,0,UINT64_MAX,42,100,101};
    WobyImportPointIds pointIds{};
    bool absent = false;
};
const uint32_t indices[] = {0,2,1, 0,1,3, 1,2,3, 2,0,3, 4,5,6, 4,7,5, 5,7,6, 6,7,4};
const WobyImportGroup groups[] = {{"upper",0,12,0,{}},{"lower",12,12,0,{}}};
int outstanding = 0;

uint32_t WOBY_IMPORT_CALL importFile(const WobyImportRequest* request, WobyImportResult* result)
{
    if (outstanding) { result->error = "Previous point ID result was not released."; return WOBY_IMPORT_ERROR; }
    try {
        auto* data = new Data;
        result->user_data = data;
        ++outstanding;
        result->flags = WOBY_IMPORT_HAS_NORMALS | WOBY_IMPORT_HAS_TEXCOORDS;
        result->vertices = data->vertices.data();
        result->vertex_count = std::strstr(request->path, "duplicate") ? 9u : 8u;
        result->indices = indices; result->index_count = 24;
        result->groups = groups; result->group_count = 2;
        data->pointIds = {sizeof(WobyImportPointIds),data->ids.data(),result->vertex_count};
        data->absent = std::strstr(request->path, "absent") != nullptr;
        if (std::strstr(request->path, "distinct")) {
            for (size_t i = 0; i < data->ids.size(); ++i) { data->ids[i] = i; }
        }
        if (std::strstr(request->path, "short_ids")) { data->pointIds.struct_size = 0; }
        if (std::strstr(request->path, "wrong_count")) { ++data->pointIds.vertex_count; }
        if (std::strstr(request->path, "null_ids")) { data->pointIds.ids = nullptr; }
        if (std::strstr(request->path, "different_position")) { data->vertices[4].position[0] = 1e-20; }
        if (std::strstr(request->path, "failure")) { result->error = "Point ID fixture failure."; return WOBY_IMPORT_ERROR; }
        request->report_progress(request->context, .5f);
        return request->is_canceled(request->context) ? WOBY_IMPORT_CANCELED : WOBY_IMPORT_OK;
    } catch (...) {
        result->error = "Point ID fixture allocation failed."; return WOBY_IMPORT_ERROR;
    }
}

void WOBY_IMPORT_CALL releaseResult(WobyImportResult* result)
{
    if (result->user_data) {
        auto* data = static_cast<Data*>(result->user_data);
        for (auto& id : data->ids) { id = 123; }
        delete data;
        result->user_data = nullptr;
        --outstanding;
    }
}

const WobyImportPointIds* WOBY_IMPORT_CALL getPointIds(const WobyImportResult* result)
{
    const auto& data = *static_cast<const Data*>(result->user_data);
    return data.absent ? nullptr : &data.pointIds;
}

const WobyImporterApiWithPointIds api{
    {{{sizeof(WobyImporterApiWithPointIds),WOBY_IMPORTER_ABI_VERSION,
        "org.woby.test.pointids","Point ID fixture importer","1","wpoint",importFile,releaseResult},nullptr},nullptr},getPointIds};
}

extern "C" WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api.base.base.base : nullptr;
}
