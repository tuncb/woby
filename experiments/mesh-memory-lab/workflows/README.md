# Mesh lab workflow files

Store new workflows in this folder as UTF-8 JSON with the `.meshflow` extension.
The app discovers files recursively in its active folder, sorted by path. Each
immediate subfolder is a commit group in the library pane; use commit hashes or
descriptive revision names as folder names. Files at the root continue to work.
Directory symlinks are not followed. Build staging preserves the folder structure
under `workflows/` beside the executable; `--workflows-dir` selects another root.

## Workflow catalog

The [catalog](../workflow-catalog.md) links all 80 numbered diagrams in
[`catalog/`](catalog/). Their filenames and title prefixes match catalog IDs
01–80. Open the **catalog** library group to browse them; the three introductory
examples remain at the library root. This group is a descriptive collection,
not a captured Git revision.

Diagrams 01–06 embed focused OBJ fixtures for vertex mapping, concave
triangulation, normal fallback, UV seams, hard edges, and large-coordinate
precision. Their bound stages expose the existing live inspectors. Other nodes
and diagrams 07–80 provide authored stage explanations, decisions, intermediate
data, and implementation references; they do not run those application workflows.
All files use the existing version 1 schema.

From the repository root, open a catalog diagram with:

```powershell
.\build\vs2026-vcpkg\bin\Debug\mesh_memory_lab.exe `
  --workflows-dir .\experiments\mesh-memory-lab\workflows `
  --workflow catalog/65-local-uv-stretch-and-anisotropy.meshflow
```

Select a node for its explanation and connected stages. Use **100%** for full
cards or **Fit** for an overview. To compare revisions, copy the desired files
into revision folders while preserving filenames and meaningful node IDs.
Embedded sources are rebuilt by the current executable; historical intermediate
results require the future capture extension described in the catalog.

## Compare commit snapshots

For example, keep the same workflow filename in each commit folder:

```text
workflows/
  a1b2c3d/
    mesh-pipeline.meshflow
    background-loading.meshflow
  e4f5a6b/
    mesh-pipeline.meshflow
    background-loading.meshflow
```

Open the first workflow, click **Add comparison**, then select the second.
The two diagrams stack **top / bottom**, with their relative paths shown in
the headers. You can also right-click any workflow and choose **Open on top**
or **Open below**. Clicking a node focuses that pane; selecting a library item
then replaces the focused pane. **Replace bottom** explicitly selects a new
bottom workflow.

Both diagrams have their own inspector on the right. Switch between **Stage**,
**Mesh**, and **Bytes** (or **Outline** for diagrams without a mesh). Drag either
vertical divider to resize both inspectors. The **Library** eye button frees
sidebar space, and **Inspectors** hides or shows both inspectors. **100%** and
**Fit** switch between full diagram cards and a compact overview.
Each pane keeps its own tab, selected node, corner, vertex field, camera, and
GPU capture. **Close** removes either pane and expands
the survivor. Reload keeps both selected relative paths, even when new files
change their ordering, and clamps selections if a mesh changes. Removed or
invalid selections close; any surviving workflow stays open.

Folders are snapshots you provide; the app does not check out Git commits or
generate an automatic diff. The `.meshflow` format remains version 1.

```powershell
mesh_memory_lab.exe --workflows-dir D:/my-workflows `
  --workflow a1b2c3d/mesh-pipeline.meshflow `
  --compare e4f5a6b/mesh-pipeline.meshflow
```

Use relative paths for duplicate filenames. A bare filename selects a root file
or a unique match; ambiguous filenames are rejected. `--inspect top|bottom`
focuses the corresponding pane when launching a comparison. Both inspectors
are visible by default. `--inspector-tab stage|mesh|bytes` selects their initial
tabs; `--hide-library` and `--hide-inspectors` control initial panel visibility.

## Minimal diagram

```json
{
  "format": "mesh-memory-lab.workflow",
  "version": 1,
  "title": "Prepare a mesh",
  "description": "An example diagram without live mesh data.",
  "initial_node": "prepare",
  "nodes": [
    {
      "id": "input",
      "title": "Mesh input",
      "kind": "data",
      "summary": "Original arrays",
      "description": "Owned mesh data before preparation.",
      "position": [0, 0]
    },
    {
      "id": "prepare",
      "title": "Prepare",
      "kind": "transform",
      "summary": "Build derived data",
      "description": "Describe the operation here.",
      "position": [324, 0]
    }
  ],
  "edges": [
    { "from": "input", "to": "prepare", "label": "snapshot" }
  ]
}
```

## Version 1 fields

| Field | Meaning |
| --- | --- |
| `format`, `version` | Required, exactly `mesh-memory-lab.workflow` and integer `1`. |
| `title` | Required display name, at most 120 UTF-8 bytes. |
| `description` | Optional overview, at most 8,000 bytes. |
| `initial_node` | Optional node ID to select on opening; defaults to the first node. |
| `nodes` | Required array of 1–64 nodes, in outline order. |
| `edges` | Required array of 0–128 directed connections. |
| `source` | Optional embedded OBJ source for live mesh inspection. |

Each node requires a unique `id` (1–64 ASCII letters, digits, underscores, or
hyphens), `title` (120 bytes), `kind` (`data` or `transform`), and `position`
(`[x, y]`, finite coordinates within 0–20,000). Cards are 196 by 108 logical
pixels. Leave gaps between them, especially for connection labels; the diagram
pane scrolls when it exceeds the available space.

Optional node fields are `summary` (240 bytes, shown on the card), `description`
(16,000 bytes, shown in the inspector), and `inspector` (a live binding below).
Each edge requires `from` and `to` node IDs and may have a `label` (120 bytes).
Branches, cycles, and disconnected nodes are supported; duplicate connections,
self-connections, unknown IDs, and unknown fields are rejected. Required strings
cannot be blank; strings cannot contain null characters. Files are limited to
2 MiB and libraries to 256 workflow files across all commit folders. Unsupported versions produce a clear
error rather than being interpreted as version 1.

## Live mesh inspection

Add an embedded source to the top-level object:

```json
"source": {
  "format": "obj",
  "text": "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
}
```

This is self-contained: no model paths are resolved against the current working
directory. The existing polygonal OBJ tracer validates source data against the
production loader. Its 512 KiB source and 60,000-corner limits still apply.

Attach an `inspector` to a compatible node to open its live tables:

| Node kind | Inspector keys |
| --- | --- |
| `data` | `source`, `attributes`, `corners`, `mesh`, `gpu` |
| `transform` | `parse`, `triangulate`, `pack`, `upload` |

For example, `"inspector": "mesh"` opens the CPU mesh arrays for a data node.
Nodes without bindings retain their authored description and graph navigation.
Bindings select existing inspectors; they do not execute scripts or change the
production pipeline. GPU handles, readback bytes, calculated payload sizes, and
runtime jobs are rebuilt from the source and are never serialized.

**Save copy** serializes the selected document to a fresh `-copy-N.meshflow`
file in the original workflow's commit folder, preserving its diagram and embedded sample. The active selection and
camera are temporary inspector state. Edit the saved file and use **Reload
folder** to refresh the library.
