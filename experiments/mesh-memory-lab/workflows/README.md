# Mesh lab workflow files

Store new workflows in this folder as UTF-8 JSON with the `.meshflow` extension.
The app discovers files in its active folder, sorted by filename. Discovery is
not recursive. Build staging copies these examples into `workflows/` beside the
executable; `--workflows-dir` selects another folder.

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
2 MiB and folders to 256 workflow files. Unsupported versions produce a clear
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
file, preserving its diagram and embedded sample. The active selection and
camera are temporary inspector state. Edit the saved file and use **Reload
folder** to refresh the library.
