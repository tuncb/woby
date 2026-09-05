#ifndef WOBY_IMPORTER_H
#define WOBY_IMPORTER_H

#include <stdint.h>

#if defined(_WIN32)
#define WOBY_IMPORT_CALL __cdecl
#define WOBY_IMPORT_EXPORT __declspec(dllexport)
#else
#define WOBY_IMPORT_CALL
#define WOBY_IMPORT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WOBY_IMPORTER_ABI_VERSION 1u
#define WOBY_IMPORT_OK 0u
#define WOBY_IMPORT_ERROR 1u
#define WOBY_IMPORT_CANCELED 2u
#define WOBY_IMPORT_HAS_NORMALS 1u
#define WOBY_IMPORT_HAS_TEXCOORDS 2u

/* All strings are null-terminated UTF-8. No exceptions may cross this ABI.
 * The plugin owns all returned memory until release_result. See doc/importers.md. */
typedef struct WobyImportVertex {
    float position[3];
    float normal[3];
    float texcoord[2];
} WobyImportVertex;

typedef struct WobyImportGroup {
    const char* name;
    uint32_t index_offset;
    uint32_t index_count;
} WobyImportGroup;

typedef struct WobyImportRequest {
    uint32_t struct_size;
    const char* path;
    void* context;
    uint32_t (WOBY_IMPORT_CALL *is_canceled)(void* context);
    void (WOBY_IMPORT_CALL *report_progress)(void* context, float fraction);
} WobyImportRequest;

typedef struct WobyImportResult {
    uint32_t struct_size;
    uint32_t flags;
    const WobyImportVertex* vertices;
    uint32_t vertex_count;
    const uint32_t* indices;
    uint32_t index_count;
    const WobyImportGroup* groups;
    uint32_t group_count;
    const char* error;
    void* user_data;
} WobyImportResult;

typedef struct WobyImporterApi {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* id;
    const char* name;
    const char* version;
    /* Semicolon-separated extensions without dots, e.g. "ply;off". */
    const char* extensions;
    uint32_t (WOBY_IMPORT_CALL *import_file)(const WobyImportRequest*, WobyImportResult*);
    /* Called exactly once after every import_file call, including failure/cancel. */
    void (WOBY_IMPORT_CALL *release_result)(WobyImportResult*);
} WobyImporterApi;

typedef const WobyImporterApi* (WOBY_IMPORT_CALL *WobyGetImporterApi)(uint32_t host_abi_version);

/* Each plugin exports this exact symbol; return NULL for an unsupported ABI. */
#ifdef WOBY_IMPORTER_BUILD
WOBY_IMPORT_EXPORT
#endif
const WobyImporterApi* WOBY_IMPORT_CALL woby_get_importer_api(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif
#endif
