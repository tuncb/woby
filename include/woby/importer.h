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

#define WOBY_IMPORTER_ABI_VERSION 2u
#define WOBY_IMPORT_OK 0u
#define WOBY_IMPORT_ERROR 1u
#define WOBY_IMPORT_CANCELED 2u
#define WOBY_IMPORT_HAS_NORMALS 1u
#define WOBY_IMPORT_HAS_TEXCOORDS 2u
#define WOBY_IMPORT_GROUP_HAS_COLOR 1u
#define WOBY_IMPORT_GROUP_INITIALLY_HIDDEN 2u
#define WOBY_IMPORT_NO_PARENT UINT32_MAX
#define WOBY_IMPORT_NO_GROUP UINT32_MAX
#define WOBY_IMPORT_MAX_HIERARCHY_DEPTH 128u

/* All strings are null-terminated UTF-8. No exceptions may cross this ABI.
 * The plugin owns all returned memory until release_result. See doc/importers.md. */
typedef struct WobyImportVertex {
    double position[3]; // Original coordinates; Woby chooses the working origin.
    float normal[3];
    float texcoord[2];
} WobyImportVertex;

typedef struct WobyImportGroup {
    const char* name;
    uint32_t index_offset;
    uint32_t index_count;
    /* Zero flags uses Woby's palette and initially shows this group. */
    uint32_t flags;
    /* Optional RGBA in [0, 1], used only with WOBY_IMPORT_GROUP_HAS_COLOR.
     * Alpha initializes group opacity. These are defaults; saved scenes override them. */
    float color[4];
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

/* Optional ABI 2 extension. Existing API and result layouts remain unchanged.
 * Nodes are ordered parent before child. Only containers (NO_GROUP) can parent
 * other nodes; every mesh group must occur exactly once. Names are display labels
 * and may repeat; WobyImportGroup.name remains the stable, unique saved identity. */
typedef struct WobyImportHierarchyNode {
    const char* name;
    uint32_t parent_index;
    uint32_t group_index;
} WobyImportHierarchyNode;

typedef struct WobyImportHierarchy {
    uint32_t struct_size;
    const WobyImportHierarchyNode* nodes;
    uint32_t node_count;
} WobyImportHierarchy;

typedef struct WobyImporterApiWithHierarchy {
    /* Set base.struct_size to sizeof(WobyImporterApiWithHierarchy). Return &base
     * from woby_get_importer_api. Older hosts read only the unchanged base. */
    WobyImporterApi base;
    /* Optional; called after a successful import_file. NULL means a flat import.
     * All metadata is borrowed until release_result. Do not retain the request
     * or perform further import work here. No exceptions may cross this ABI. */
    const WobyImportHierarchy* (WOBY_IMPORT_CALL *get_hierarchy)(const WobyImportResult*);
} WobyImporterApiWithHierarchy;

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
