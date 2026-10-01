#include "woby/importer.h"

#include <cstring>
#include <vector>

namespace {
const WobyImportVertex vertices[] = {
    {{-1,-1,0}, {0,0,1}, {}}, {{1,-1,0}, {0,0,1}, {}},
    {{1,1,0}, {0,0,1}, {}}, {{-1,1,0}, {0,0,1}, {}},
    {{-1.5,0,-.25}, {0,0,1}, {}}, {{1.5,0,-.25}, {0,0,1}, {}},
    {{-.5,-1.4,.1}, {0,0,1}, {}}, {{-.5,1.4,.1}, {0,0,1}, {}},
};
const uint32_t triangles[] = {0,1,2,0,2,3};
const WobyImportGroup surface{"surface",0,6,WOBY_IMPORT_GROUP_HAS_COLOR,{.45f,.45f,.45f,1}};
struct Data {
    std::vector<uint32_t> indices{4,5,6,7};
    WobyImportGroup groups[2] = {
        {"rear-curve",0,2,WOBY_IMPORT_GROUP_HAS_COLOR,{0,1,0,1}},
        {"front-curve",2,2,WOBY_IMPORT_GROUP_HAS_COLOR,{1,0,0,1}},
    };
    std::vector<WobyImportHierarchyNode> nodes;
    WobyImportHierarchy hierarchy{};
    WobyImportLines lines{};
    bool absent = false;
};
int outstanding = 0;
uint32_t WOBY_IMPORT_CALL importFile(const WobyImportRequest* request, WobyImportResult* result)
{
    if (outstanding) { result->error = "Previous line result was not released exactly once."; return WOBY_IMPORT_ERROR; }
    try {
        auto* data = new Data;
        result->user_data = data;
        ++outstanding;
        const bool only = std::strstr(request->path, "lineonly") != nullptr;
        data->absent = std::strstr(request->path, "absent") != nullptr;
        result->vertices = vertices; result->vertex_count = 8;
        result->flags = WOBY_IMPORT_HAS_NORMALS;
        if (!only) {
            result->indices = triangles; result->index_count = 6;
            result->groups = &surface; result->group_count = 1;
        }
        data->nodes.push_back({"Assembly",WOBY_IMPORT_NO_PARENT,WOBY_IMPORT_NO_GROUP});
        if (!only) { data->nodes.push_back({"Surface",0,0}); }
        const auto parent = static_cast<uint32_t>(data->nodes.size());
        data->nodes.push_back({"Boundary curves",0,WOBY_IMPORT_NO_GROUP});
        data->nodes.push_back({"Rear curve",parent,only ? 0u : 1u});
        data->nodes.push_back({"Front curve",parent,only ? 1u : 2u});
        data->hierarchy = {sizeof(WobyImportHierarchy),data->nodes.data(),static_cast<uint32_t>(data->nodes.size())};
        data->lines = {sizeof(WobyImportLines),data->indices.data(),4,data->groups,2};
        if (std::strstr(request->path, "invalid_index")) { data->indices[0] = 99; }
        if (std::strstr(request->path, "odd_count")) { data->lines.index_count = 3; }
        if (std::strstr(request->path, "null_buffer")) { data->lines.indices = nullptr; }
        if (std::strstr(request->path, "short_lines")) { data->lines.struct_size = 0; }
        if (std::strstr(request->path, "hidden")) { data->groups[1].flags |= WOBY_IMPORT_GROUP_INITIALLY_HIDDEN; }
        if (std::strstr(request->path, "failure")) { result->error = "Line fixture failure."; return WOBY_IMPORT_ERROR; }
        request->report_progress(request->context,.5f);
        return request->is_canceled(request->context) ? WOBY_IMPORT_CANCELED : WOBY_IMPORT_OK;
    } catch (...) {
        result->error = "Line fixture allocation failed."; return WOBY_IMPORT_ERROR;
    }
}
void WOBY_IMPORT_CALL releaseResult(WobyImportResult* result)
{
    if (result->user_data) { delete static_cast<Data*>(result->user_data); result->user_data = nullptr; --outstanding; }
}
const WobyImportHierarchy* WOBY_IMPORT_CALL getHierarchy(const WobyImportResult* result)
{
    const auto& data = *static_cast<const Data*>(result->user_data);
    return data.absent ? nullptr : &data.hierarchy;
}
const WobyImportLines* WOBY_IMPORT_CALL getLines(const WobyImportResult* result)
{
    const auto& data = *static_cast<const Data*>(result->user_data);
    return data.absent ? nullptr : &data.lines;
}
const WobyImporterApiWithLines api{
    {{sizeof(WobyImporterApiWithLines),WOBY_IMPORTER_ABI_VERSION,
        "org.woby.test.lines","Line fixture importer","1","wline",importFile,releaseResult},getHierarchy},getLines};
}
extern "C" WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api.base.base : nullptr;
}
