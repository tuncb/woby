# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Audit current OBJ files and calculate the storage saved by indexed triangles.

Unique render-vertex counts come from existing, contract-tested import probes.
This script checks the complete file hash and source-face topology before using
those counts. It does not build a second, expanded copy of a large model.
"""
import argparse
import collections
import csv
import hashlib
import html
import json
import re
import time
from pathlib import Path


RECORD = re.compile(rb"(?m)^[ \t]*(f|l|p|surf|curv)[ \t]+([^\r\n]*)")
VERTEX_BYTES = 32
INDEX_BYTES = 4
MODEL_LABELS = {
    "20190810_BearTrap_Ground_Model.obj": "BearTrap",
    "bennu_OLA_v21_PTM_very-high.obj": "Bennu",
    "powerplant.obj": "Powerplant",
    "san-miguel.obj": "San Miguel",
    "uploads_files_2720101_BusGameMap.obj": "BusGameMap",
}


def audit_obj(stream, chunk_bytes=8 * 1024 * 1024):
    if chunk_bytes < 1:
        raise ValueError("Chunk size must be positive.")
    digest = hashlib.sha256()
    pending = b""
    face_sizes = collections.Counter()
    other = collections.Counter()
    size = 0

    def inspect(block):
        for match in RECORD.finditer(block):
            kind, body = match.groups()
            if kind == b"f":
                count = len(body.split(b"#", 1)[0].split())
                if count < 3:
                    raise ValueError("A source face has fewer than three corners.")
                face_sizes[count] += 1
            else:
                other[kind.decode("ascii")] += 1

    while block := stream.read(chunk_bytes):
        digest.update(block)
        size += len(block)
        block = (pending + block).replace(b"\\\r\n", b" ").replace(b"\\\n", b" ")
        cut = block.rfind(b"\n") + 1
        inspect(block[:cut])
        pending = block[cut:]
    inspect(pending)
    return {
        "bytes": size,
        "sha256": digest.hexdigest(),
        "source_faces_by_corner_count": dict(sorted(face_sizes.items())),
        "source_faces": sum(face_sizes.values()),
        "triangles_from_faces": sum((n - 2) * count for n, count in face_sizes.items()),
        "line_records": other["l"], "point_records": other["p"],
        "surface_records": other["surf"], "curve_records": other["curv"],
    }


def storage(triangles, vertices, vertex_bytes=VERTEX_BYTES, index_bytes=INDEX_BYTES):
    for count in (triangles, vertices, vertex_bytes, index_bytes):
        if isinstance(count, bool) or not isinstance(count, int) or count < 1:
            raise ValueError("Counts and element sizes must be positive integers.")
    corners = 3 * triangles
    if vertices > corners:
        raise ValueError("Render vertices exceed the triangle corner count.")
    expanded = corners * vertex_bytes
    vertex_payload = vertices * vertex_bytes
    index_payload = corners * index_bytes
    indexed = vertex_payload + index_payload
    saved = expanded - indexed
    return {
        "triangles": triangles, "render_vertices": vertices, "corners": corners,
        "vertex_stride_bytes": vertex_bytes, "index_stride_bytes": index_bytes,
        "average_corner_references_per_vertex": corners / vertices,
        "vertex_records_removed": corners - vertices,
        "vertex_payload_reduction_percent": 100 * (1 - vertices / corners),
        "expanded_vertex_bytes": expanded, "shared_vertex_bytes": vertex_payload,
        "triangle_index_bytes": index_payload, "indexed_bytes": indexed,
        "saved_bytes": saved, "net_saving_percent": 100 * saved / expanded,
        "indexed_bytes_per_triangle": indexed / triangles,
    }


def probe_counts(batch):
    if batch.get("status") != "complete" or not batch.get("records"):
        raise ValueError("The measurement batch is incomplete.")
    counts = None
    seams = set()
    for record in batch["records"]:
        probe = record["probe"]
        geometry = probe["geometry"]
        if geometry["line_segments"] or geometry["point_indices"]:
            raise ValueError("This comparison requires face-only geometry.")
        current = tuple(geometry[key] for key in (
            "triangles", "vertices", "source_positions", "geometry_fingerprint",
            "normal_fingerprint", "source_fingerprint"))
        if counts is not None and current != counts:
            raise ValueError("Import records disagree on geometry.")
        counts = current
        if probe["memory"]["vertex_size_bytes"] != geometry["vertices"] * VERTEX_BYTES:
            raise ValueError("The measured vertex layout is not 32 bytes.")
        for event in probe["events"]:
            if event["name"] == "corner_mapping":
                seams.add(event["data"]["secondary_vertices"])
    if len(seams) != 1:
        raise ValueError("Import records disagree on seam vertex count.")
    triangles, vertices, positions, *fingerprints = counts
    secondary = seams.pop()
    used_positions = vertices - secondary
    if not 0 < used_positions <= positions:
        raise ValueError("Used source position count is invalid.")
    return {
        **storage(triangles, vertices), "source_positions": positions,
        "used_source_positions": used_positions,
        "unused_source_positions": positions - used_positions,
        "extra_vertices_for_normal_or_uv_seams": secondary,
        "seam_extra_vertex_bytes": secondary * VERTEX_BYTES,
        "geometry_fingerprint": fingerprints[0],
        "normal_fingerprint": fingerprints[1], "source_fingerprint": fingerprints[2],
        "import_records_checked": len(batch["records"]),
    }


def measure(batch_path):
    batch = json.loads(batch_path.read_text(encoding="utf-8"))
    result = probe_counts(batch)
    model = Path(batch["model"]["path"])
    started = time.perf_counter()
    with model.open("rb") as stream:
        audit = audit_obj(stream)
    if audit["bytes"] != batch["model"]["bytes"] or audit["sha256"] != batch["model"]["sha256"]:
        raise ValueError(f"The current model differs from the measured file: {model}")
    if any(audit[key] for key in ("line_records", "point_records", "surface_records", "curve_records")):
        raise ValueError("The source contains additional primitive types.")
    if audit["triangles_from_faces"] != result["triangles"]:
        raise ValueError("Source face counts do not match the imported triangle count.")
    result.update({"model": model.name, "model_path": str(model), "batch_path": str(batch_path.resolve()),
                   "batch_sha256": hashlib.sha256(batch_path.read_bytes()).hexdigest(),
                   "audit_seconds": time.perf_counter() - started, "audit": audit})
    return result


def mib(value):
    return f"{value / (1024 ** 2):,.2f}"


def render_report(results):
    rows = []
    charts = []
    total_expanded = sum(r["expanded_vertex_bytes"] for r in results)
    total_indexed = sum(r["indexed_bytes"] for r in results)
    for r in results:
        name = html.escape(MODEL_LABELS.get(r["model"], r["model"]))
        rows.append(f"<tr><th>{name}</th><td>{r['triangles']:,}</td><td>{r['render_vertices']:,}</td>"
                    f"<td>{r['average_corner_references_per_vertex']:.2f}</td>"
                    f"<td>{mib(r['expanded_vertex_bytes'])}</td><td>{mib(r['shared_vertex_bytes'])}</td>"
                    f"<td>{mib(r['triangle_index_bytes'])}</td><td>{mib(r['indexed_bytes'])}</td>"
                    f"<td>{mib(r['saved_bytes'])}</td><td>{r['net_saving_percent']:.2f}%</td></tr>")
        vb = 100 * r["shared_vertex_bytes"] / r["expanded_vertex_bytes"]
        ib = 100 * r["triangle_index_bytes"] / r["expanded_vertex_bytes"]
        charts.append(f"<article><h3>{name}</h3><div class='bar expanded'>Separate vertices · {mib(r['expanded_vertex_bytes'])} MiB</div>"
                      f"<div class='bar'><span class='vertices' style='width:{vb}%'></span>"
                      f"<span class='indices' style='width:{ib}%'></span></div>"
                      f"<p>Indexed: {mib(r['indexed_bytes'])} MiB · saves {r['net_saving_percent']:.2f}%</p></article>")
    topology = "".join(f"<tr><th>{html.escape(MODEL_LABELS.get(r['model'], r['model']))}</th>"
                       f"<td>{html.escape(str(r['audit']['source_faces_by_corner_count']))}</td>"
                       f"<td>{r['used_source_positions']:,}</td><td>{r['unused_source_positions']:,}</td>"
                       f"<td>{r['extra_vertices_for_normal_or_uv_seams']:,}</td></tr>" for r in results)
    return """<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>woby · Vertex reuse storage</title><style>
body{margin:0;background:#f6f4ed;color:#193d35;font:16px/1.6 system-ui,sans-serif}main{max-width:1150px;margin:auto;padding:40px 24px}h1{font:48px/1.1 Georgia,serif;max-width:850px}h2{margin-top:40px}code{background:#e5eae2;padding:3px 6px}a{color:#175d49}.scroll{overflow:auto}table{border-collapse:collapse;width:100%;font-size:14px}th,td{text-align:right;border-bottom:1px solid #c9d1c6;padding:10px;white-space:nowrap}th:first-child{text-align:left}article{padding:20px 0;border-bottom:1px solid #c9d1c6}article h3{font-size:17px;overflow-wrap:anywhere}.bar{display:flex;height:30px;background:#e5eae2;border-radius:4px;overflow:hidden}.expanded{height:auto;min-height:30px;background:#c9d1c6;padding-left:8px}.vertices{background:#236b55}.indices{background:#d8973b}.key{display:inline-block;width:12px;height:12px;margin-right:4px}.note{color:#4d665c}li{margin:12px 0}@media(max-width:600px){h1{font-size:36px}}
</style><main><p>WOBY / STORAGE AUDIT / 13 OCTOBER 2026</p><h1>Store each complete vertex once. Reference it from each triangle.</h1>
<p>Five face-based OBJ models from <code>D:\\temp\\obj_tests</code>. Current files were fully scanned and SHA-256 checked against the earlier import probes. Counts agree across all 36 baseline and prototype confirmation records. The folder's point-cloud and native parametric datasets are excluded.</p>
<p>For T triangles and U render vertices: separate vertices = <code>96 × T</code> bytes; indexed geometry = <code>32 × U + 12 × T</code> bytes. The separate case uses no index buffer.</p>
<p>Each 32-byte vertex stores float32 position, normal, and UV. Each triangle has three uint32 indices. A render vertex is a distinct source position/normal/UV index tuple. Coincident positions with different source IDs are not welded.</p>
<p class="note">These are exact geometry payload sizes, not total VRAM allocation or CPU peak memory. Textures, source data, wireframe/point buffers, parser arrays, lookup tables, vector spare capacity, and allocator overhead are excluded. Missing UVs still occupy space in the current fixed vertex layout.</p>
<h2>Measured counts and calculated buffer sizes</h2><p>All sizes in MiB (1 MiB = 1,048,576 bytes). Reuse = triangle corner references ÷ render vertices.</p><div class="scroll"><table><thead><tr><th>Model</th><th>Triangles</th><th>Render vertices</th><th>Reuse</th><th>Separate MiB</th><th>Shared vertices MiB</th><th>Indices MiB</th><th>Indexed MiB</th><th>Saved MiB</th><th>Net saving</th></tr></thead><tbody>""" + "".join(rows) + f"""</tbody></table></div>
<p>Combined: {mib(total_expanded)} MiB separate → {mib(total_indexed)} MiB indexed. Saved {mib(total_expanded - total_indexed)} MiB ({100 * (1 - total_indexed / total_expanded):.2f}%). This is a sum across models, not a measured simultaneous GPU load.</p>
<h2>Where the bytes go</h2><p><span class='key vertices'></span>Shared vertex records <span class='key indices'></span>Triangle indices. Each model's full bar is its own separate-vertex size; bars are not absolute size comparisons across models.</p>""" + "".join(charts) + """
<h2>Source topology and seams</h2><p>BearTrap, Bennu, and Powerplant contain triangle faces only. San Miguel contains some polygons; BusGameMap is mostly quads. These two rows describe the triangle mesh produced by triangulation. Face histogram keys are corner counts. Extra seam vertices are additional normal/UV tuples for an already used source position.</p><div class="scroll"><table><thead><tr><th>Model</th><th>Source face histogram</th><th>Used positions</th><th>Unused positions</th><th>Extra seam vertices</th></tr></thead><tbody>""" + topology + """</tbody></table></div>
<h2>What the result means</h2><p>For a well-connected triangle surface, a complete vertex can serve about six corners. At six references, indexed storage uses 28 bytes per triangle rather than 96: a 70.83% saving. UV seams and normal splits reduce reuse.</p><p>The mapping redesign keeps these same vertex and index counts. It makes the conversion faster and reduces temporary CPU storage; vertex reuse already existed in the original importer.</p><p>Indexing is beneficial when average reuse exceeds 32/28 = 1.143 references per vertex. A mesh with no shared complete vertices grows by 12.5% if we add uint32 indices.</p>
<h2>Methods used for large meshes</h2><ol>
<li><b>Index complete vertex records and improve locality.</b> meshoptimizer provides remapping, triangle ordering for vertex reuse, and vertex fetch ordering. Its implementation uses contiguous hash buckets with quadratic probing. Our tuple-first mapping avoids expanding a large OBJ into 3T full vertex records before deduplication. <a href="https://github.com/zeux/meshoptimizer">Library pipeline</a> · <a href="https://github.com/zeux/meshoptimizer/blob/master/src/indexgenerator.cpp">Index generator</a></li>
<li><b>Pack attributes more tightly.</b> Quantized normals and UVs reduce resident GPU memory. glTF defines integer vertex formats for this purpose. A candidate float32 position + packed 4-byte normal + 4-byte UV uses 20 bytes per vertex before any backend padding. This is a proposal, not measured here. UV range and precision must be checked. <a href="https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_mesh_quantization/README.md">glTF quantization</a></li>
<li><b>Compress a prepared binary cache.</b> meshopt codecs reduce disk and transfer size, then decode to the normal vertex/index buffers. This alone does not shrink resident GPU buffers. The glTF extension supports independent compressed buffer views. <a href="https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Vendor/EXT_meshopt_compression/README.md">glTF meshopt compression</a></li>
<li><b>Partition large models.</b> Smaller draws can use 16-bit local indices, at a cost in boundary vertex copies and draw bookkeeping. Meshlets use local triangle indices and global vertex references, and support fine-grained culling. NVIDIA reports a large-scan example, but its timing is not a prediction for woby. <a href="https://developer.nvidia.com/blog/using-mesh-shaders-for-professional-graphics/">NVIDIA professional graphics</a></li>
<li><b>Stream clusters and select detail.</b> Nanite builds hierarchical triangle clusters and loads visible detail on demand. This makes resident memory depend on the visible working set. It requires a wider rendering design. <a href="https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine">Nanite</a> · <a href="https://gpuopen.com/learn/mesh_shaders/mesh_shaders-meshlet_compression/">AMD GPU meshlet decompression</a></li>
<li><b>Limit concurrent work by memory.</b> The meshoptimizer author's billion-triangle studies describe avoiding redundant copies, processing only needed attributes, and scheduling large meshes with memory limits. <a href="https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/">2025 study</a> · <a href="https://zeux.io/2026/09/30/billions-of-triangles-redux/">2026 follow-up</a></li>
</ol><p>For woby, the next small experiments are packed GPU normals/UVs and a reusable binary import cache. Triangle reordering needs a mapping back to source triangle IDs and must preserve group/material ranges. The sparse-position fallback addresses temporary mapping cost; it does not add vertex reuse.</p>
<p><a href="statistics.csv">Download exact statistics CSV</a> · <a href="statistics.json">Audit and provenance JSON</a></p></main></html>"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError("Use a new output directory.")
    results = []
    for path in args.batch:
        print(f"Auditing {path.name}...", flush=True)
        result = measure(path)
        results.append(result)
        print(f"  {result['model']}: {result['net_saving_percent']:.2f}% saved, "
              f"{result['audit_seconds']:.2f}s audit, faces {result['audit']['source_faces_by_corner_count']}", flush=True)
    if len({r["model_path"] for r in results}) != len(results):
        raise ValueError("Do not include a model more than once.")
    args.output.mkdir(parents=True)
    fields = [key for key in results[0] if key != "audit"]
    with (args.output / "statistics.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows({key: r[key] for key in fields} for r in results)
    document = {"method": "Current full-file SHA-256 and source-face audit; matching exact import probe counts",
                "scope": "Render vertex and triangle index payloads only; uint32 indices and 32-byte vertices",
                "models": results}
    (args.output / "statistics.json").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    (args.output / "report.html").write_text(render_report(results), encoding="utf-8")


if __name__ == "__main__":
    main()
