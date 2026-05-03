# Plugin Architecture Sketch

## Summary

A plugin architecture is a good fit for woby if plugins are treated as CPU-side scene
producers and woby remains the owner of logical UI state, GPU resources, persistence,
and final scene commits.

The current app already has much of the right shape:

- CPU model and scene loading happen in the background through `loadModelBatchCpu`
  and `loadSceneCpu`.
- bgfx resources are created on the main thread during GPU finalization.
- `UiState` is only changed after the load/finalize operation succeeds.
- Scene parent/child relationships are already represented in `UiSceneNode`.

The main missing piece is stable identity. Today scene relationships are mostly
index-based through `fileIndex` and `groupIndex`. That is workable for static loaded
files, but plugin add/remove/modify operations need stable IDs such as
`plugin_id + object_id + mesh_id`.

## Recommended Direction

Plugins should not directly mutate `UiState`, call bgfx, call ImGui, or own scene
memory after handoff. They should produce snapshots or diffs, and woby should validate,
stage, finalize, and commit those changes.

The host-side flow should be:

1. Load a plugin DLL/shared library and check its ABI version.
2. Run plugin CPU work in a background worker.
3. Convert plugin output into app-owned staged data.
4. Validate mesh arrays, index ranges, hierarchy references, transforms, and names.
5. Create or replace bgfx resources on the main/render thread.
6. Commit the prepared update to `UiState` only after all required resources exist.
7. If validation or GPU finalization fails, destroy staged resources and leave the
   visible scene unchanged.

This keeps the current "background prepares, main thread commits" model intact.

## C ABI Shape

Do not expose C++ containers, `std::string`, `std::vector`, `Mesh`, bgfx, SDL, or ImGui
through the plugin boundary. Use plain versioned C structs with explicit lifetimes.

Example sketch:

```c
#define WOBY_PLUGIN_ABI_VERSION 1

typedef struct WobyPluginVertex {
    float position[3];
    float normal[3];
    float texcoord[2];
} WobyPluginVertex;

typedef struct WobyPluginMeshPart {
    const char* id;
    const char* name;
    uint32_t index_offset;
    uint32_t index_count;
} WobyPluginMeshPart;

typedef struct WobyPluginMesh {
    const char* id;
    const char* name;
    const WobyPluginVertex* vertices;
    uint32_t vertex_count;
    const uint32_t* indices;
    uint32_t index_count;
    const WobyPluginMeshPart* parts;
    uint32_t part_count;
} WobyPluginMesh;

typedef struct WobyPluginNode {
    const char* id;
    const char* parent_id;
    const char* mesh_id;
    const char* mesh_part_id;
    const char* name;
    float translation[3];
    float rotation_degrees[3];
    float scale;
} WobyPluginNode;
```

The exact ABI can change, but the important rules are:

- Every object needs a stable plugin-provided ID.
- Arrays must include counts.
- Ownership and release rules must be explicit.
- The plugin should never receive mutable pointers into `UiState`.
- The plugin output should be copied into woby-owned structures before finalization.

## UiState Changes

`UiState` should remain the canonical logical state. Plugin-backed scene data should be
represented as normal scene data with source metadata.

Possible shape:

```cpp
enum class UiModelSourceKind {
    file,
    plugin,
};

struct UiModelSource {
    UiModelSourceKind kind = UiModelSourceKind::file;
    std::filesystem::path path;
    std::string pluginId;
    std::string objectId;
    uint64_t generation = 0;
};

struct UiFileState {
    UiModelSource source;
    std::filesystem::path path;
    Mesh mesh;
    std::vector<UiGroupState> groupSettings;
    UiFileSettings fileSettings;
    float vertexSizeScale = 1.0f;
};
```

Keeping `path` temporarily can reduce churn while migrating existing UI and file-based
scene code.

## Hierarchy

Do not put parent/child relationships into `MeshNode`. `MeshNode` is currently a flat
range into an index buffer. Hierarchy should stay in `UiSceneNode`, because the UI,
renderer, bounds calculation, and save/load mapping already understand that tree.

Plugins should provide:

- Meshes.
- Mesh parts or groups.
- Scene nodes that reference mesh or mesh-part IDs.
- Parent IDs for hierarchy.

woby should translate that into `UiFileState`, `UiGroupState`, and `UiSceneNode`.

## Updates

Plugin updates should be staged as snapshots or diffs. A snapshot is simpler and safer
for the first implementation.

Example staged update concepts:

```cpp
enum class PluginSceneChangeKind {
    addOrReplaceObject,
    removeObject,
};

struct PluginSceneObject {
    std::string objectId;
    std::string displayName;
    Mesh mesh;
    std::vector<UiSceneNode> nodes;
};

struct PluginSceneUpdate {
    std::string pluginId;
    uint64_t generation = 0;
    std::vector<PluginSceneObject> objects;
    std::vector<std::string> removedObjectIds;
};
```

The final commit operation should live in `ui_operations`, not in ImGui or plugin host
code. It should clamp and validate at the operation boundary.

## Persistence

The current `.woby` format assumes every model is path-backed. Plugin scenes need a
versioned extension.

Example direction:

```toml
[[plugins]]
id = "com.example.generator"
path = "plugins/example.dll"
config = "opaque plugin config"

[[files]]
source = "plugin"
plugin_index = 0
object_id = "mesh-42"
display_name = "Generated part"
```

The save/load behavior should be explicit:

- File-backed models keep using path records.
- Plugin-backed models save plugin identity and plugin object identity.
- Plugin configuration should be persisted only if the plugin contract supports it.
- If a plugin is unavailable on load, woby should report the missing plugin and either
  skip those objects or preserve unresolved records without crashing.

## Implementation Gaps

The likely implementation gaps are:

- Add stable IDs for plugin objects, meshes, and nodes.
- Add source metadata to `UiFileState`.
- Add a small `plugin_host.*` module for dynamic loading and ABI checks.
- Add C ABI headers for plugin authors.
- Add staged plugin update structs.
- Add `ui_operations` functions to apply plugin updates atomically.
- Extend `.woby` save/load mapping for plugin-backed records.
- Update renderer/runtime mappings so `LoadedModelRuntime` stays aligned with logical
  model state after plugin add/remove/replace.
- Add tests for add, remove, modify, hierarchy preservation, failed update rollback,
  and save/load mapping.

## Risk Notes

The main design risk is allowing plugin-driven changes to leak across boundaries. Keep
the plugin contract narrow:

- Plugins produce CPU data.
- woby owns copied scene data.
- woby validates at load/update boundaries.
- bgfx resources remain runtime-only.
- `UiState` remains the canonical logical state.
- Scene changes are committed only after background CPU work and main-thread GPU
  finalization have completed successfully.

