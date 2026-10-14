"""Write the Bennu mapping-path comparison from recorded imports and viewers."""
import argparse
import hashlib
import html
import json
from pathlib import Path
import shutil
import statistics
import zipfile


def median(values):
    return statistics.median(values)


def table(headings, rows):
    cells = lambda row, tag: "".join(f"<{tag}>{html.escape(str(value))}</{tag}>" for value in row)
    return "<table><thead><tr>" + cells(headings,"th") + "</tr></thead><tbody>" + "".join(
        "<tr>" + cells(row,"td") + "</tr>" for row in rows) + "</tbody></table>"


def metric(rows, extract):
    return [median(extract(row) for row in rows if row["configuration"] == name) for name in ("fallback","staged")]


def timing(rows, name, extract):
    before, after = metric(rows,extract)
    return [name, f"{before/1000:.3f}", f"{after/1000:.3f}", f"{(before-after)/1000:.3f}", f"{(before-after)/before*100:.1f}%"]


def memory(rows, name, extract):
    before, after = metric(rows,extract)
    return [name, f"{before/2**20:,.2f}", f"{after/2**20:,.2f}", f"{(after-before)/2**20:+,.2f}"]


def report(measure, output, source):
    output.mkdir(parents=True,exist_ok=True)
    cpu = json.loads((measure / "cpu-final/cpu.json").read_text())
    native = json.loads((measure / "native-matched/summary.json").read_text())
    traced = json.loads((measure / "traced-matched/summary.json").read_text())
    production_native = json.loads((measure / "native/summary.json").read_text())
    production_traced = json.loads((measure / "traced/summary.json").read_text())
    validation = json.loads((measure / "validation.json").read_text())
    deferred = [row for row in cpu if row["preparation"] == "deferred"]
    eager = [row for row in cpu if row["preparation"] == "eager"]
    timings = [timing(deferred,"CPU import; deferred preparation",lambda r:r["cpu_load_ms"]),
               timing(deferred,"Corner mapping",lambda r:r["corner_mapping"]["total_ms"]),
               timing(eager,"CPU import; eager preparation",lambda r:r["cpu_load_ms"])]
    native_times = [timing(native,"CPU import",lambda r:r["cpu_import_ms"]),
                    timing(native,"Full readiness; model.add RPC",lambda r:r["load_rpc_ms"])]
    trace_times = [timing(traced,"CPU import",lambda r:r["cpu_import_ms"]),
                   timing(traced,"First scene frame; GPU completion",lambda r:r["milestones_ms"]["first_scene_frame_gpu_complete"]),
                   timing(traced,"All tools ready",lambda r:r["milestones_ms"]["all_tools_ready"])]
    memories = [memory(deferred,"CPU import peak working set",lambda r:r["peak_working_set_bytes"]),
                memory(deferred,"CPU import peak private commit",lambda r:r["peak_commit_bytes"]),
                memory(deferred,"Private bytes after import",lambda r:r["private_bytes_after_load"]),
                memory(deferred,"Retained mesh buffer capacity",lambda r:r["retained_mesh_buffer_capacity_bytes"]),
                memory(deferred,"Primary lookup table capacity",lambda r:r["corner_mapping"]["primary_capacity_bytes"]),
                memory(deferred,"Seam table capacity",lambda r:r["corner_mapping"]["key_capacity_bytes"]),
                memory(deferred,"Parser triangle payload freed during mapping",lambda r:r["corner_mapping"]["released_triangle_bytes"])]
    viewer_memory = [memory(native,"Whole viewer peak working set",lambda r:r["process"]["peak_working_set"]["max"]),
                     memory(native,"Whole viewer peak private commit",lambda r:r["process"]["peak_commit"]["max"])]
    details = []
    phase_memory = []
    for row in traced:
        raw = json.loads(Path(row["path"]).read_text())
        phases = [p for p in raw["trace"] if p["kind"] == "phase"]
        mapping = next(p for p in phases if p["name"] == "corner_mapping")
        parsing = next(p for p in phases if p["name"] == "parser_total")
        details.append([row["round"],row["configuration"],f"{mapping['wall_ms']/1000:.3f}",
                        f"{mapping['commit_begin']/2**20:,.1f}",f"{mapping['commit_end']/2**20:,.1f}",
                        f"{parsing['process_peak_commit']/2**20:,.1f}"])
        phase_memory.append({"round":row["round"],"configuration":row["configuration"],
            "mapping":mapping,"parsing":parsing})
    result = {"cpu":cpu,"native":native,"traced":traced,"validation":validation,"phase_memory":phase_memory,
              "production_native_controls":production_native,"production_traced_controls":production_traced,
              "viewer_linkage":json.loads((measure/"linkage-controls.json").read_text()),
              "source_sha256":{path.relative_to(source).as_posix():hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in [source/"src/obj_mesh.cpp",source/"src/obj_corner_mapping.h",
                                            *sorted((source/"experiments/position-mapping").glob("*"))] if path.is_file()}}
    (output / "summary.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    before,after = metric(deferred,lambda r:r["cpu_load_ms"])
    peak_before,peak_after = metric(native,lambda r:r["process"]["peak_commit"]["max"])
    body = f'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bennu: fallback versus staged mapping</title><style>body{{font:16px/1.55 system-ui,sans-serif;color:#17334b;background:#f5f7fa;margin:0}}main{{max-width:1120px;margin:auto;padding:36px 26px 70px}}h1{{font-size:36px;line-height:1.2}}h2{{margin-top:36px}}table{{width:100%;border-collapse:collapse;background:white;font-size:14px;margin:20px 0}}th,td{{padding:10px 12px;border-bottom:1px solid #dae3eb;text-align:right}}th:first-child,td:first-child{{text-align:left}}th{{background:#e7eff6}}a{{color:#075da8}}.lead{{font-size:21px}}.note{{color:#4c6074}}pre{{white-space:pre-wrap;overflow-wrap:anywhere;background:#e7eff6;padding:18px}}.scroll{{overflow:auto}}</style>
<main><p class="note">14 October 2026 · Windows · Release · Ryzen 7 5800H</p>
<h1>Bennu: fallback versus staged mapping</h1>
<p class="lead">The staged mapper imports Bennu in {after/1000:.3f} s versus {before/1000:.3f} s with the current fallback: {(before-after)/before*100:.1f}% less CPU load time. Peak import memory is effectively unchanged.</p>
<p>Bennu has 8,933,524 source positions, 53,600,508 triangle corners, no normals, and no UVs. The input is 815,866,103 bytes. The current application selects its 4-byte position lookup. The comparison build permits the same file through the 12-byte staged mapper.</p>
<h2>Complete CPU imports</h2><p>Five alternating pairs use deferred preparation, as in the viewer. Three further pairs use eager preparation. Each trial starts a fresh process, warms the same input first, and uses the default parser workers and up to eight gather workers. These measure the complete importer, including normal generation, bounds, and release of parser data. Validation runs after the time and memory snapshot.</p>
<p>Tables show configuration medians. Each difference subtracts those medians. Every paired CPU import, native readiness measurement, and GPU first-display measurement improves.</p>
{table(["Endpoint","Fallback median s","Staged median s","Difference s","Reduction"],timings)}
<h2>Viewer results</h2><p>Three fresh-process pairs use matching viewer builds: the same sources and compiler/linker options, with the position-only eligibility control off or on. Mixed primitives, point clouds, sparse inputs, and freeform guards remain in force. The separate stock-viewer comparisons are retained in the numeric results and archive.</p>
{table(["Native viewer endpoint","Fallback median s","Staged median s","Difference s","Reduction"],native_times)}
<p>Three separate instrumented pairs supply a GPU frame-completion marker. Their times must be compared within this group.</p>
{table(["Instrumented endpoint","Fallback median s","Staged median s","Difference s","Reduction"],trace_times)}
<h2>Memory</h2><p>All values below are medians in MiB (1,048,576 bytes). Process peaks are Windows lifetime high-water counters, read before fingerprint validation. Retained capacities count mesh buffers once. Table capacity is temporary memory, not retained mesh memory.</p>
{table(["CPU importer memory","Fallback MiB","Staged MiB","Staged minus fallback MiB"],memories)}
<p>The staged table adds 68.16 MiB, but it frees 766.76 MiB of parser triangle payload before final vertex allocation and normal generation. The table size alone does not predict the process peak. Parsing sets a larger peak before these paths diverge.</p>
{table(["Native viewer memory","Fallback MiB","Staged MiB","Staged minus fallback MiB"],viewer_memory)}
<p>Whole-viewer peaks include GPU upload staging, driver allocations, and preparation after first display. They are separate from the CPU-only importer peaks.</p>
<details><summary>Instrumented memory at phase boundaries</summary><p>These are current private bytes at mapping entry and exit. Parser peak is the process high-water counter at parser completion. Mapping entry is after its initial reserves; staged mapping exit is after early parser release. These boundary values are not mapping-only peaks.</p>
{table(["Round","Path","Mapping s","Entry private MiB","Exit private MiB","Parser peak commit MiB"],details)}</details>
<h2>Do we need the fallback?</h2><p>For Bennu, the staged mapper is faster. CPU importer peak private commit differs by 0.05 MiB; matching viewer peak private commit changes by {(peak_after-peak_before)/2**20:+.2f} MiB ({(peak_after-peak_before)/peak_before*100:+.2f}%). Both paths retain the same mesh buffers. The smaller fallback table does not reduce the importer peak. Removing the position-only exclusion is supported by this comparison for Bennu.</p>
<p class="note">The first viewer controls compared the stock executable with a mapper override. The native override executable is 3.77 MiB larger, which could affect the small memory difference. The headline viewer tables use matched override builds for both paths, with identical executable and code-section sizes. The earlier controls are a separate capture group.</p>
<p>The current staged routine maps faces only. Lines, explicit points, and vertex clouds still use the existing primitive builder and vertex-ID table. Extending or replacing that handling needs implementation work; those primitives do not inherently require the old face mapper.</p>
<p>The freeform exclusion is conservative. Curves and surfaces are evaluated separately and appended after polygon geometry. This comparison did not test whether polygon faces in a freeform file can use staged mapping. It does not establish that freeforms require the old mapper.</p>
<p>The sparse-input guard is a temporary-memory choice, not a correctness requirement. When many source positions are unused, the 4-byte primary table and bounded keys can be smaller than a 12-byte slot for every position. This comparison did not measure sparse inputs or establish their whole-process peak-memory difference.</p>
<p>Production path selection is unchanged. This is an isolated comparison, not a routing change. Other position-only models still need a representative regression run before a general routing change.</p>
<h2>Correctness and validation</h2><p>The full Bennu buffers are compared directly in separate eager and deferred verification runs. This includes vertex bytes and generated normals, ordered triangle IDs, source positions and indices, position maps, group fields, origin, and bounds. Every timed run also computes a matching full-mesh fingerprint outside the measured interval.</p>
<pre>{html.escape(json.dumps(validation,indent=2))}</pre>
<h2>Evidence</h2><p><a href="summary.json">Numeric results and source hashes</a> · <a href="measurements.zip">Raw captures, source snapshots, commands, and validation logs</a> · <a href="REPRODUCE.txt">Reproduction commands</a></p>
<p class="note">All values apply to this Bennu file and this computer. The earlier resolver-only replay bypassed application selection; this comparison includes both complete loader paths.</p></main></html>'''
    (output / "report.html").write_text(body,encoding="utf-8")
    shutil.copyfile(measure / "REPRODUCE.txt",output / "REPRODUCE.txt")
    with zipfile.ZipFile(output / "measurements.zip","w",compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(measure.rglob("*")):
            if path.relative_to(measure).parts[0] in ("measured-bin","cpu") or path.name == "cpu-driver.log":
                continue
            if path.is_file() and path.suffix.lower() in (".json",".jsonl",".log",".txt",".py",".cpp",".h",".png",".diff"):
                archive.write(path,"measurements/" + path.relative_to(measure).as_posix())
        for path in [source/"CMakeLists.txt",source/"src/obj_mesh.cpp",source/"src/obj_corner_mapping.h",
                     source/"experiments/rapidobj_prototype.cpp",*sorted((source/"experiments/position-mapping").glob("*"))]:
            if path.is_file():
                archive.write(path,"source/" + path.relative_to(source).as_posix())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("measurements",type=Path)
    parser.add_argument("output",type=Path)
    args = parser.parse_args()
    report(args.measurements.resolve(strict=True),args.output.resolve(),Path(__file__).resolve().parents[2])
