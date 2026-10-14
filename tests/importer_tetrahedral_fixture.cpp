#include "importer_tetrahedral_fixture.h"
#include <cstring>

namespace {
int outstanding = 0;
uint32_t WOBY_IMPORT_CALL importFile(const WobyImportRequest* request, WobyImportResult* result)
{
    if (outstanding) { result->error = "Previous tetrahedral result was not released exactly once."; return WOBY_IMPORT_ERROR; }
    try {
        auto* data = new ImportTetrahedralData;
        ++outstanding;
        *result = data->result; result->user_data = data;
        if (std::strstr(request->path,"absent")) { result->indices = data->cells[0].nodes; result->index_count = 3; }
        if (std::strstr(request->path,"invalid")) { data->cells[0].nodes[4] = 99; }
        if (std::strstr(request->path,"linear")) { for (auto& cell : data->cells) { cell.node_count = 4; } }
        if (std::strstr(request->path,"failure")) { result->error = "Tetrahedral fixture failure."; return WOBY_IMPORT_ERROR; }
        request->report_progress(request->context,.5f);
        return request->is_canceled(request->context) ? WOBY_IMPORT_CANCELED : WOBY_IMPORT_OK;
    } catch (...) { result->error = "Tetrahedral fixture allocation failed."; return WOBY_IMPORT_ERROR; }
}
void WOBY_IMPORT_CALL releaseResult(WobyImportResult* result)
{
    if (result->user_data) { delete static_cast<ImportTetrahedralData*>(result->user_data); result->user_data = nullptr; --outstanding; }
}
const WobyImportTetrahedra* WOBY_IMPORT_CALL getTetrahedra(const WobyImportResult* result)
{
    if (result->index_count) { return nullptr; }
    return &static_cast<const ImportTetrahedralData*>(result->user_data)->tetrahedra;
}
const WobyImportHierarchy* WOBY_IMPORT_CALL getHierarchy(const WobyImportResult* result)
{
    if (result->index_count) { return nullptr; }
    return &static_cast<const ImportTetrahedralData*>(result->user_data)->hierarchy;
}
const WobyImporterApiWithTetrahedra api = [] {
    WobyImporterApiWithTetrahedra value{};
    value.base.base.base.base.base = {sizeof(value),WOBY_IMPORTER_ABI_VERSION,"org.woby.test.tetrahedra","Tetrahedral fixture","1","wtet",importFile,releaseResult};
    value.base.base.base.base.get_hierarchy = getHierarchy;
    value.get_tetrahedra = getTetrahedra;
    return value;
}();
}
extern "C" WOBY_IMPORT_EXPORT const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t version)
{
    return version == WOBY_IMPORTER_ABI_VERSION ? &api.base.base.base.base.base : nullptr;
}
