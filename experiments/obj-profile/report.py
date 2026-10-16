# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib>=3.10"]
# ///
"""Write a local research report and export figures from the measurement records."""
import argparse
from collections import Counter, defaultdict
import html
import json
from pathlib import Path
import re
import shutil
import statistics
import zipfile

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from data_flow import FLOW_SCRIPT, FLOW_STYLE, write_data_flow
from deep_report import write_deep_report
from svg_embed import namespace_svg


def phases(record):
    output = defaultdict(list)
    for event in record["events"]:
        output[event["name"]].append(event)
    return output


def value(record, name, key="wall_ms"):
    return sum(event[key] for event in phases(record).get(name, []))


def med(records, name, key="wall_ms"):
    return statistics.median(value(record, name, key) for record in records)


def cpu_total(record):
    return sum(value(record, name) for name in ("cpu_import", "group_state", "scene_cpu_preparation"))


def table(headers, rows):
    return '<table><thead><tr>' + ''.join(f'<th>{html.escape(str(item))}</th>' for item in headers) + '</tr></thead><tbody>' + ''.join(
        '<tr>' + ''.join(f'<td>{html.escape(str(item))}</td>' for item in row) + '</tr>' for row in rows) + '</tbody></table>'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.input.resolve()
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    data = {name: json.loads((source / f"{name}.json").read_text(encoding="utf-8"))
            for name in ("baseline", "parser-scaling", "isa-comparison", "io", "memory", "diagnostic", "sampled", "annotation", "instructions")}
    viewer = json.loads((source / "viewer/viewer.json").read_text(encoding="utf-8"))
    shutil.copy2(source / "viewer/beartrap.png", destination / "beartrap.png")
    viewer_log = (source / "viewer/viewer.log").read_text(encoding="utf-8")
    cpu_load = re.search(r'perf model_cpu_load .*parse_ms=([\d.]+) total_ms=([\d.]+)', viewer_log)
    upload = re.search(r'perf model_gpu_finalize .*bytes=(\d+) upload_ms=([\d.]+) longest_step_ms=([\d.]+)', viewer_log)
    render_events = viewer["frames"]["events"]
    gpu_frame_ms = statistics.median(event["gpuFrameMilliseconds"] for event in render_events)
    render_wall_ms = statistics.median(event["frameMilliseconds"] for event in render_events)
    gpu = viewer.get("gpu", {})
    render_gpu_samples = [point for point in gpu.get("samples", []) if
        viewer["render_started_ms"] <= point["time_ms"] <= viewer["render_started_ms"] + viewer["render_duration_ms"]]
    gpu_peak_bytes = max((point["memory_used"] for point in gpu.get("samples", [])), default=0)
    gpu_busy = statistics.median(point["gpu_percent"] for point in render_gpu_samples) if render_gpu_samples else 0
    baseline = [record for record in data["isa-comparison"]["records"] if record["compiler_isa"] == "SSE2"]
    avx = [record for record in data["isa-comparison"]["records"] if record["compiler_isa"] == "AVX2"]
    series = data["parser-scaling"]["records"]
    total = statistics.median(cpu_total(record) for record in baseline)
    avx_total = statistics.median(cpu_total(record) for record in avx)
    sample = min(baseline, key=lambda record: abs(cpu_total(record) - total))
    geometry = baseline[0]["geometry"]
    diagnostics = next(event["data"] for event in data["diagnostic"]["records"][0]["events"] if event["name"] == "corner_mapping")
    counts = diagnostics["diagnostic_counts"]
    corners = geometry["triangles"] * 3
    mapping_time = med(baseline, "corner_mapping")
    scaling = []
    for workers in (1, 2, 4, 8, 16):
        chosen = [record for record in series if record["workers_requested"] == workers]
        times = [value(record, "parser_total") for record in chosen]
        scaling.append({"workers": workers, "wall_ms": statistics.median(times), "min_ms": min(times), "max_ms": max(times),
                        "scan_ms": med(chosen, "parser_scan_collect"), "merge_ms": med(chosen, "parser_merge"),
                        "average_cpus": statistics.median(value(record, "parser_total", "cpu_ms") / value(record, "parser_total") for record in chosen)})
    stage_names = ["capacity_preflight", "parser_total", "parser_scan_collect", "parser_attribute_allocation", "parser_merge_execute",
        "coordinate_origin", "coordinate_rebase", "triangulation", "mesh_allocation", "corner_mapping", "normal_clear",
        "normal_face_accumulation", "normal_vertex_normalize", "render_bounds", "original_bounds", "parser_lookup_release",
        "group_state", "group_render_bounds", "group_original_bounds", "gpu_index_validation", "point_membership_allocate",
        "point_membership_build", "point_hierarchy", "point_range_bounds", "point_morton_keys", "point_radix_sort",
        "point_sorted_gather", "point_tree_build", "point_navigation_cuts"]
    stage_rows, stage_summary = [], []
    for name in stage_names:
        wall = med(baseline, name)
        cpus = statistics.median(value(record, name, "cpu_ms") / max(value(record, name), .001) for record in baseline)
        row = {"name": name, "sse2_ms": wall, "avx2_ms": med(avx, name), "average_logical_cpus": cpus,
               "total_cpu_percent": cpus / 16 * 100,
               "read_bytes": med(baseline, name, "read_bytes"), "page_faults": med(baseline, name, "page_faults")}
        stage_summary.append(row)
        stage_rows.append([name, f'{wall/1000:.3f}', f'{row["avx2_ms"]/1000:.3f}', f'{cpus:.2f}', f'{cpus/16*100:.1f}%', f'{row["read_bytes"]/1e9:.3f}'])
    io_rows, io_summary = [], []
    for mode in ("read-cached", "read-native"):
        for workers in (1, 8, 16):
            chosen = [record for record in data["io"]["records"] if record["mode"] == mode and record["workers_requested"] == workers]
            wall = med(chosen, "read_only")
            rate = data["io"]["file_bytes"] / wall / 1e6
            times = [value(record, "read_only") for record in chosen]
            io_summary.append({"mode": mode, "workers": workers, "wall_ms": wall, "min_ms": min(times),
                               "max_ms": max(times), "GB_per_second": rate})
            io_rows.append([mode, workers, f'{wall/1000:.3f}', f'{min(times)/1000:.3f}–{max(times)/1000:.3f}', f'{rate:.2f}'])
    annotation = data["annotation"]["records"][0]
    annotation_rows = [[name, f'{value(annotation,name)/1000:.3f}'] for name in (
        "annotation_cache", "annotation_blocks_fingerprints", "annotation_bounds", "annotation_fingerprints",
        "annotation_morton_keys", "annotation_radix_sort", "annotation_tree_build") if name in phases(annotation)]
    peak = statistics.median(next(event["process_peak_working_set"] for event in record["events"] if event["name"] == "cpu_import") for record in baseline)
    retained = geometry["retained_buffer_capacity_bytes"]
    actual = geometry["vertex_bytes"] + geometry["index_bytes"] * 2 + geometry["vertices"] * 4 + geometry["source_positions"] * 24
    memcpy_rate = statistics.median(record["stats"]["payload_GB_per_second"] for record in data["memory"]["records"])
    memory_rows = []
    for name in ("parser_scan_collect", "parser_merge_execute", "corner_mapping", "parser_lookup_release", "group_state", "point_hierarchy"):
        event = phases(sample)[name][0]
        memory_rows.append([name, f'{event["working_set_end"]/2**30:.3f}', f'{event["commit_end"]/2**30:.3f}', f'{event["page_faults"]:,}'])

    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "axes.spines.top": False, "axes.spines.right": False})
    fig, axes = plt.subplots(2, 2, figsize=(13, 9), constrained_layout=True)
    colors = {"SSE2": "#216e76", "AVX2": "#c4772d"}
    labels = ["Read, decode, merge", "Origin + coordinate update", "Inspect triangles", "Map corners", "Generate normals",
              "Whole-mesh bounds", "Other import work", "Group bounds", "Validation + point lists", "Point hierarchy"]
    def components(records):
        parts = [med(records,"parser_total"), med(records,"coordinate_origin")+med(records,"coordinate_rebase"),
                 med(records,"triangulation"), med(records,"corner_mapping"), med(records,"normal_generation"),
                 med(records,"render_bounds")+med(records,"original_bounds")]
        parts += [med(records,"cpu_import") - sum(parts), med(records,"group_state"),
                  med(records,"scene_cpu_preparation")-med(records,"point_hierarchy"), med(records,"point_hierarchy")]
        return [part/1000 for part in parts]
    y = list(range(len(labels)))
    for offset, records, label in ((-.18,baseline,"SSE2"),(.18,avx,"AVX2")):
        axes[0,0].barh([item+offset for item in y],components(records),height=.34,color=colors[label],label=label)
    axes[0,0].set_yticks(y,labels); axes[0,0].invert_yaxis(); axes[0,0].set_xlabel("Wall time (seconds)")
    axes[0,0].set_title("CPU path to upload-ready geometry"); axes[0,0].legend(frameon=False)
    axes[0,1].errorbar([row["workers"] for row in scaling],[row["wall_ms"]/1000 for row in scaling],
        yerr=[[ (row["wall_ms"]-row["min_ms"])/1000 for row in scaling],[(row["max_ms"]-row["wall_ms"])/1000 for row in scaling]],
        color=colors["SSE2"],marker="o",capsize=4)
    axes[0,1].set_xticks([1,2,4,8,16]); axes[0,1].set_xlabel("Parser workers"); axes[0,1].set_ylabel("Parser wall time (seconds)")
    axes[0,1].set_title("Cached parser scaling · three runs per setting"); axes[0,1].grid(axis="y",alpha=.15)
    occupancy_names = ["parser_scan_collect","parser_merge_execute","corner_mapping","normal_generation","group_state","point_hierarchy"]
    occupancy = [statistics.median(value(record,name,"cpu_ms")/value(record,name) for record in baseline) for name in occupancy_names]
    axes[1,0].barh(["Parser scan","Parser merge","Corner mapping","Normals","Group bounds","Point hierarchy"],occupancy,color=colors["SSE2"])
    axes[1,0].invert_yaxis(); axes[1,0].set_xlim(0,16); axes[1,0].set_xlabel("Average logical CPUs used (of 16)")
    axes[1,0].set_title("Process CPU time ÷ wall time")
    end = max(event["end_ms"] for event in sample["events"] if event["name"] == "scene_cpu_preparation")
    samples = [point for point in sample["resource_samples"] if point["time_ms"] <= end]
    axes[1,1].plot([point["time_ms"]/1000 for point in samples],[point["working_set"]/2**30 for point in samples],color=colors["SSE2"],label="Working set")
    axes[1,1].plot([point["time_ms"]/1000 for point in samples],[point["commit"]/2**30 for point in samples],color=colors["AVX2"],label="Private commit")
    axes[1,1].set_xlabel("Time since probe startup (seconds)"); axes[1,1].set_ylabel("GiB")
    axes[1,1].set_title("CPU import and preparation memory · selected SSE2 run"); axes[1,1].legend(frameon=False)
    fig.suptitle("BearTrap OBJ · 7.109 GB · Ryzen 7 5800H · Release",fontsize=17,fontweight="bold")
    fig.savefig(destination / "overview.png",dpi=180)
    fig.savefig(destination / "overview.svg")
    plt.close(fig)

    summary = {"hardware": baseline[0]["cpu"], "revision": data["isa-comparison"]["revision"], "file_sha256": data["isa-comparison"]["file_sha256"],
        "geometry": geometry, "cpu_ready_sse2_ms": total, "cpu_ready_avx2_ms": avx_total, "stages": stage_summary,
        "scaling": scaling, "io": io_summary, "mapping": diagnostics, "memcpy_payload_GB_per_second": memcpy_rate,
        "peak_working_set_bytes": peak, "actual_geometry_bytes": actual, "retained_capacity_bytes": retained,
        "annotation_events": annotation["events"], "viewer": {key: val for key,val in viewer.items() if key != "samples"}}
    (destination / "summary.json").write_text(json.dumps(summary,indent=2),encoding="utf-8")
    with zipfile.ZipFile(destination / "measurements.zip","w",compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for path in sorted(source.rglob("*")):
            if path.is_file() and path.suffix in (".json", ".log"):
                archive.write(path,str(path.relative_to(source)))
    scale_rows = [[row["workers"],f'{row["wall_ms"]/1000:.3f}',f'{row["min_ms"]/1000:.3f}–{row["max_ms"]/1000:.3f}',
                   f'{scaling[0]["wall_ms"]/row["wall_ms"]:.2f}×',f'{row["average_cpus"]:.2f}'] for row in scaling]
    hash_share = counts["secondary_lookups"]/corners*100
    parser_fraction = med(baseline,"parser_total")/total
    root_fraction = med(baseline,"parser_total")/med(baseline,"cpu_import")
    normal_hash_equal = len({record["geometry"]["normal_fingerprint"] for record in baseline+avx}) == 1
    instruction_rows = []
    instruction_categories = []
    mnemonic_counts = defaultdict(Counter)
    for row in data["instructions"]["decoded_samples"]:
        mnemonic_counts[row["phase"]][row["mnemonic"]] += row["count"]
    for name, hot in data["instructions"]["hot_symbols"].items():
        if name in ("geometry_validation", "idle"):
            continue
        instruction_rows.append([name,'; '.join(f'{symbol[:110]} ({count})' for symbol,count in hot[:3])])
        categories = data["instructions"]["sample_categories"][name]
        instruction_categories.append([name, '; '.join(f'{category}: {count}' for category,count in categories.items()),
                                       ', '.join(mnemonic for mnemonic,count in mnemonic_counts[name].most_common(8))])
    sampled_runs = [record for record in data["sampled"]["records"] if record["mode"] != "memory"]
    sampling_rows = [[record["mode"], f'{record["process_wall_ms"]/1000:.2f}', f'{record["sampler_cpu_ms"]/1000:.2f}',
                     sum(row["count"] for row in record["instruction_samples"]), record["suspend_failures"]] for record in sampled_runs]
    raw_relative = '../../../experiments/obj-profile/'
    data_flow = write_data_flow(destination, data, baseline, viewer, upload)
    deep_report = write_deep_report(source / "deep", destination)
    text = f'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BearTrap OBJ performance · 11–12 October 2026</title><style>{FLOW_STYLE}th,td{{overflow-wrap:anywhere}}code,pre{{overflow-wrap:anywhere}}img{{max-width:100%;height:auto}}
body{{font:16px/1.55 Segoe UI,Arial,sans-serif;color:#203139;background:#f3f5f5;margin:0}}main{{max-width:1160px;margin:40px auto;background:white;padding:38px 44px;border:1px solid #dce2e3}}h1{{font-size:32px;line-height:1.2}}h2{{font-size:23px;margin-top:34px}}h3{{font-size:18px}}p{{max-width:98ch}}table{{border-collapse:collapse;width:100%;font-size:14px;margin:16px 0 24px}}th,td{{padding:9px 12px;border-bottom:1px solid #dce2e3;text-align:left;vertical-align:top}}th{{background:#edf4f4}}td:not(:first-child){{font-variant-numeric:tabular-nums}}code,pre{{font-family:Consolas,monospace;font-size:13px}}pre{{white-space:pre-wrap;background:#edf4f4;padding:16px}}.lead{{font-size:19px}}.note{{border-left:4px solid #216e76;padding:8px 18px;background:#f3f8f8}}svg{{width:100%;height:auto}}a{{color:#166573}}li{{margin:10px 0}}details{{margin:18px 0}}.small{{font-size:13px;color:#586e77}}@media(max-width:700px){{main{{padding:20px;margin:0}}table{{font-size:12px}}th,td{{padding:6px}}}}</style><main>
<p class="small">11–12 October 2026 · woby 0.29.0 · revision <code>{summary['revision']}</code></p>
<h1>BearTrap OBJ: where the load time goes</h1>
{deep_report}
<p class="lead">The largest costs are serial corner mapping and bounds preparation. The parser already uses most CPU threads. The AVX2 compiler experiment helps bounds work, with little change to parsing or mapping.</p>
<p>The current SSE2 probe takes <strong>{total/1000:.2f} s</strong> to import the model and prepare CPU data for upload. The same six source units compiled with AVX2 take <strong>{avx_total/1000:.2f} s</strong>: {(1-avx_total/total)*100:.1f}% less time, or {total/avx_total:.2f}× throughput. These values are medians of three paired, rotating, fresh-process runs per build. The SSE2 range is {min(map(cpu_total,baseline))/1000:.2f}–{max(map(cpu_total,baseline))/1000:.2f} s; the AVX2 range is {min(map(cpu_total,avx))/1000:.2f}–{max(map(cpu_total,avx))/1000:.2f} s. GPU upload, rendering, and the validation hash are outside this CPU total. Three runs do not establish a confidence interval for small differences.</p>
{namespace_svg('<svg' + (destination/'overview.svg').read_text(encoding='utf-8').split('<svg',1)[-1], 'first-pass-overview-')}
{data_flow}
<h2>Computer and model</h2>
{table(['Item','Measured value'],[['CPU','AMD Ryzen 7 5800H; 8 physical cores; 16 logical processors'],['RAM','Two 32 GiB DDR4-3200 modules; 63.86 GiB usable'],['Input drive','D: on SKHynix HFS001TDE9X084N NVMe SSD'],['Build drive','C: on Samsung SSD 990 PRO 4TB NVMe SSD'],['GPU','NVIDIA GeForce RTX 3070 Laptop GPU'],['OS / compiler','Windows 11 build 26300; MSVC 19.51.36243; VS 2026 Release /O2'],['Power plan','Legion Balance Mode; unchanged'],['File size','7,108,991,827 bytes (7.109 GB; 6.621 GiB)'],['Geometry',f"{geometry['source_positions']:,} source positions; {geometry['vertices']:,} render vertices; {geometry['triangles']:,} triangles; one group"],['Attributes','38,414,285 UV coordinates; no supplied normals; vertex records include RGB']])}
<p>CPUID and XCR0 checks confirm SSE2, SSE4.2, AVX, AVX2, FMA, BMI1 and BMI2 support. The OS enables AVX state. This CPU does not support AVX-512. The application baseline does not request AVX2. Microsoft documents SSE2 as the default x64 instruction set in <a href="https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64?view=msvc-170">/arch (x64)</a>.</p>
<p>The processor has 4 MiB of L2 cache and 16 MiB of L3 cache, as reported by Windows. The inventory is saved in hardware.json in the archive. CPU clock frequency and temperature were not sampled during each stage. Paired run order limits time-order effects but does not remove power or thermal variation.</p>
<p class="small">Input SHA-256: <code>{summary['file_sha256']}</code>. The parser geometry fingerprint agrees across all 15 worker-count runs. Mesh geometry agrees across the SSE2, AVX2 and diagnostic runs. Normal buffers are {'bit-for-bit equal' if normal_hash_equal else 'different at the bit level; geometry fingerprints exclude normals'} across the paired compiler comparison.</p>
<h2>Wall time and CPU use by stage</h2>
<p>Seconds are wall time. Average CPUs means process CPU time divided by wall time. One active logical processor is 6.25% of this computer's 16 logical processors. This value measures scheduler occupancy; it does not measure SIMD lane use or arithmetic-unit use. CPU counters are coarse for very short stages. Nested stages appear below their totals and must not be added again.</p>
{table(['Stage','SSE2 s','AVX2 s','Avg. CPUs','CPU share','Read GB'],stage_rows)}
<p>The origin pass validates double source coordinates. The following pass subtracts the origin. This model uses a zero origin, but the current code still traverses and rewrites its position array. Triangulation mainly inspects already triangular faces. The mesh builder adopts the source coordinate buffer; it does not copy a second full XYZ buffer.</p>
<p>The final CPU mesh then needs per-group local and original-coordinate bounds. The group original-coordinate pass walks all {corners:,} corners, while whole-mesh original bounds earlier walked render vertices. On this one-group model, that is repeated work over the same geometry. The group radius uses the bounding box; the whole-mesh radius uses vertex distances, so those radius values need distinct handling when bounds are reused.</p>
<h2>Parser scaling</h2>
{table(['Workers','Median s','Range s','Speedup vs 1','Avg. CPUs'],scale_rows)}
<p>Going from 8 to 16 workers improves parser time by {(1-scaling[-1]['wall_ms']/scaling[-2]['wall_ms'])*100:.1f}%. The 16-worker setting is useful on this eight-core CPU. Thread affinity was not fixed, so this does not isolate SMT from scheduling effects. One worker uses the bounded chunk path; larger worker counts use persistent file ranges, so this comparison includes that internal path difference. At 16 workers there are 16 ranges and a reported 64 MiB text-storage bound. Parsed chunk data is approximately 4.55 GB and is separate from that text bound.</p>
<p>The read/decode interval overlaps file reads, conversion, chunk allocation, and ordered collection. Worker submit/wait times are summed across overlapping workers. They are not a separate wall-time stage and cannot be subtracted from total parser time to obtain disk latency.</p>
<h2>Cached I/O, device reads, and memory copy</h2>
{table(['Read control','Workers','Median s','Range s','Payload GB/s'],io_rows)}
<p>The production unified parser opens a buffered, overlapped Windows handle. Each benchmark warms the file immediately before the process starts. These parser timings therefore measure cached loading. The read-native control bypasses the Windows file cache and uses aligned 256 KiB reads. It measures the input device separately; it is not a measurement of cold production parsing. No system cache was flushed. Cached and native read controls use synchronous reads in each worker, while the parser overlaps reads with decoding.</p>
<p>A separate 512 MiB copy test, with two initialized buffers and 16 copies per process, reaches <strong>{memcpy_rate:.2f} GB/s</strong> of payload copy throughput (at least {2*memcpy_rate:.2f} GB/s of read-plus-write traffic). This is a calibration for contiguous copies. Mesh gathers, validation, table updates and branches cannot be assigned that bandwidth.</p>
<h2>Corner mapping: measured lookup counts</h2>
{table(['Counter','Value'],[[name,f'{amount:,}'] for name,amount in counts.items()])}
<p>Only <strong>{hash_share:.2f}%</strong> of corners enter the secondary seam hash lookup. First use and primary hits account for the rest. The diagnostic build adds counters, so its time is not used in the timing tables. Mapping is {corners/(mapping_time/1000)/1e6:.2f} million corners/s in the current SSE2 timing probe.</p>
<p>Mapping also validates UV values, checks source indices, gathers source coordinates, constructs vertices, records source IDs, and writes two index streams. The primary array occupies {diagnostics['primary_bytes']/2**20:.1f} MiB; key capacity is {diagnostics['key_capacity_bytes']/2**20:.1f} MiB; hash buckets occupy {diagnostics['bucket_bytes']/2**20:.1f} MiB. A faster hash function alone cannot remove most of this work.</p>
<p>The primary and key arrays each exceed the 16 MiB L3 cache. This makes access order and repeated coordinate gathers useful targets for investigation. It does not prove a measured cache-miss rate.</p>
<h2>Memory</h2>
<p>Median import peak working set is <strong>{peak/2**30:.3f} GiB</strong>, approximately {peak/68564348928*100:.1f}% of installed usable RAM. This excludes GPU allocation and the Windows input file cache. Retained mesh buffer capacity is {retained/2**30:.3f} GiB; populated geometry arrays account for about {actual/2**30:.3f} GiB. Capacity growth leaves about {(retained-actual)/2**30:.3f} GiB unused. Vertex, key and position-map capacities grow from a source-position estimate when UV seams add vertices.</p>
{table(['Stage end in selected SSE2 run','Working set GiB','Private commit GiB','Page faults during stage'],memory_rows)}
<p>Page-fault counters include soft faults and demand-zero allocation faults. Large fault counts during fresh buffer allocation are not evidence of paging to disk. Process I/O counters are request bytes, not a physical-device bandwidth measurement. The record archive also contains system disk counters, memory samples and process CPU deltas.</p>
<h2>CPU instructions: source, assembly and samples</h2>
{table(['Stage','Instruction evidence'],[['Text and number decode','Integer loads, branches, shifts and multiplies; fast_float uses 64-bit SWAR for eligible eight-digit runs. Scalar SSE2 double operations remain in numeric conversion.'],['Corner mapping','Integer index/table operations plus SSE moves and float conversion. The AVX2 build adds VEX forms and wider record moves; it does not become a wide parallel mapping kernel.'],['Normal generation','Scalar mulss, subss, sqrtss, divss and addss; a packed divps operation normalizes components. AVX2 uses the corresponding VEX forms, still mostly scalar arithmetic.'],['Bounds','Scalar FP comparisons and coordinate operations. AVX2 enables blend instructions and changes register/copy code. Much of the gain occurs in original-coordinate group bounds.'],['Point hierarchy','Integer Morton coding and radix sorting; scalar FP normalization and coordinate gathers. No explicit AVX2 sorting kernel.'],['Runtime memory routines','Read the sampled-instruction results below. The Windows CRT can dispatch CPU-specific memory routines even when application code targets SSE2.']])}
<p>Compiler assembly was read from /FAs listings. A separate run samples instruction pointers only after a thread's CPU time advances. Threads are suspended briefly for context capture, then resumed before allocation or symbol lookup. Sampled runs are excluded from performance medians. Samples identify code locations; they are not retired instruction counts and have scheduling and wait-site bias. Scalar SSE or scalar AVX instructions do not process several independent mesh vertices at once.</p>
<p>The SSE2 probe actually executes <code>vmovdqu</code> and <code>vmovntdq</code> with YMM registers in VCRUNTIME memory routines. MSVC search helpers also execute AVX2 integer instructions such as <code>vpmovmskb</code> and <code>vpmaskmovd</code>. Thus the baseline already uses CPU-specific vector memory and search code. This does not make the application corner loop an AVX2 geometry kernel. No FMA instruction was found in the inspected normal-generation assembly.</p>
{table(['Instruction-sampled run','Process wall s','Sampler CPU s','IP samples','Failed suspensions'],sampling_rows)}
<p>System-wide thread snapshots still account for most sampler CPU time. The records retain inventory and context-capture costs. The first sampler trial used much more CPU and was discarded from conclusions. Short stages have few samples; zero samples do not mean zero execution. CPU-progress filtering still permits wait-site samples because Windows thread CPU counters have coarse resolution.</p>
<details><summary>Decoded instruction categories and examples by stage</summary>{table(['Stage','IP sample categories: counts','Common sampled mnemonics'],instruction_categories)}</details>
<details><summary>Most frequent sampled code locations by stage</summary>{table(['Sampled stage','Code locations (sample counts)'],instruction_rows)}</details>
<p>Full decoded samples, source lines, module offsets, and per-function static instruction categories are in the archive. Application locations have matching PDB source lines. Runtime libraries can resolve to the nearest exported symbol, including names such as _NLG_Return2; use the module offset and decoded instruction rather than treating that export name as an exact function. AVX2 compiler code is experimental and requires a compatible CPU. The comparison compiles the six loader/preparation source units and the harness with AVX2; common core libraries remain at their baseline ISA.</p>
<p class="note">In this first pass, Windows rejected the WPR CPU trace with error 0xc5585011. Hardware counters were not measured in that pass. The 12 October section adds AMD core-PMC estimates for IPC, retired instructions, demand cache fills, branch misses, DTLB misses and FP work. Physical DRAM bandwidth and exact dynamic SIMD counts remain unmeasured. QueryProcessCycleTime is an OS process counter and is not used to claim IPC or an exact CPU frequency.</p>
<h2>Asynchronous annotation work</h2>
{table(['Stage','One run without instruction sampling, seconds'],annotation_rows)}
<p>The current viewer prepares this cache on a separate worker after scene publication. It is not added to initial CPU preparation time. It validates geometry fingerprints, creates triangle centroid keys, sorts them, and builds a spatial tree. It shares immutable mesh geometry. In the viewer it can compete with rendering for CPU and memory bandwidth; the isolated annotation run measures the work without concurrent rendering.</p>
<h2>Production viewer check</h2>
<p>The unmodified production viewer imported the real file through model.add, uploaded resources, returned scene statistics and produced a screenshot. The hidden-window RPC load took {viewer['load_rpc_ms']/1000:.2f} s. A verified screenshot was returned {viewer['load_to_verified_screenshot_ms']/1000:.2f} s after the load request. This is a completed readback endpoint, not an exact first-visible-frame time. The hidden viewer's pacing, progress callbacks, GPU upload, UI work and asynchronous preparation differ from the isolated CPU probe.</p>
<p>The production log reports {float(cpu_load[2])/1000:.2f} s of CPU loading and preparation. Its field named <code>parse_ms</code> is {float(cpu_load[1])/1000:.2f} s and includes the complete CPU mesh importer, including mapping, normals and bounds. It is not the text parser interval. Upload covers {int(upload[1])/1e9:.3f} GB. Summed upload submission steps take {float(upload[2])/1000:.2f} s, with a longest step of {float(upload[3]):.1f} ms. The remaining { (viewer['load_rpc_ms']-float(cpu_load[2]))/1000:.2f} s of RPC load time includes incremental upload, frame pacing, driver work and final publication. Submission time does not equal the elapsed upload interval.</p>
<p>The fixed-camera recording contains {len(render_events)} frames over {viewer['render_duration_ms']/1000:.2f} s. Median GPU frame time is {gpu_frame_ms:.2f} ms; median wall frame time is {render_wall_ms:.2f} ms. The hidden viewer caps itself at 20 FPS and uses 4× MSAA at 1280×720, with an 805×687 model viewport. CPU frame timers include waits and pacing. These data do not establish maximum rendering FPS. The recording starts after screenshot capture and can overlap annotation-cache work.</p>
<p>NVML reports {gpu.get('memory_total',0)/2**30:.0f} GiB of GPU memory. Peak device-wide memory use is {gpu_peak_bytes/2**30:.2f} GiB, including other applications. Device-wide GPU busy time in the fixed-camera interval has a median of {gpu_busy:.0f}%. NVML returns a constant 100% for its memory-utilization field on this system; those readings are not used as a bandwidth measure. GPU rendering is outside OBJ text parsing. This check does not assign GPU busy percentages to CPU loader stages.</p>
<p><a href="beartrap.png">Rendered model</a>. Production load, preparation, upload and frame records are in the archive. Two production viewer runs are retained; their load endpoints are about 53.9 and 55.8 s.</p>
<h2>Priorities for an optimization change</h2>
<ol>
<li><strong>Reduce bounds work.</strong> Reuse cached whole-mesh bounds when a group covers the same complete geometry. Preserve the group's bounding-box radius rule. For general groups, use independent local reductions and merge their results. This work is deterministic and does not need shared vertex insertion.</li>
<li><strong>Investigate the corner loop.</strong> Preserve global first-use vertex IDs and UV/normal seam behavior. Separate index resolution from attribute gathering in a research version, or partition source-position ownership and order final IDs by first corner. Measure the extra buffers and passes. The current primary shortcut already avoids most hashing.</li>
<li><strong>Use the AVX2 result narrowly.</strong> The compiler experiment saves {(total-avx_total)/1000:.2f} s on this CPU. Consider CPU dispatch for the measured bounds/coordinate kernels, with an SSE2 fallback. A global AVX2 build raises the minimum CPU requirement. Wide math alone does not fix mapping's dependencies.</li>
<li><strong>Check eager point preparation.</strong> The point hierarchy costs {med(baseline,'point_hierarchy')/1000:.2f} s before upload even with markers off. Deferring it moves that cost to first use. Picking, marker IDs, group counts and cancellation must retain correct behavior.</li>
<li><strong>Reduce allocation growth.</strong> UV seams add {diagnostics['secondary_vertices']:,} vertices ({diagnostics['secondary_vertices']/geometry['vertices']*100:.2f}% of render vertices), but vector growth reserves much more than the required count. Compare a better initial estimate, bounded chunks, or a two-pass count without repeating the entire expensive mapping pass.</li>
<li><strong>Keep parser work in proportion.</strong> Parsing is {parser_fraction*100:.1f}% of this CPU preparation total, and {root_fraction*100:.1f}% of CPU import alone. Even removing parsing completely would improve preparation throughput by at most {1/(1-parser_fraction):.2f}×. RGB values account for extra numeric conversion, but are already discarded without a colour buffer. Any faster handling must retain syntax and finite-value validation.</li>
</ol>
<p>As a work-priority model, halving corner mapping with no added work would save about {mapping_time/2000:.2f} s and improve CPU preparation throughput by {total/(total-mapping_time/2):.2f}×. This is an upper estimate for that change, not a measured optimization. Parallel mapping can add sorting, buffers and ordering costs that reduce the gain.</p>
<h2>Reproduce and inspect</h2>
<pre>cmake --preset vs2026-vcpkg -DWOBY_BUILD_OBJ_PROFILE=ON
cmake --build --preset vs2026-vcpkg --config Release
ctest --preset vs2026-vcpkg -C Release
uv run experiments/obj-profile/run.py build/vs2026-vcpkg/bin/Release/woby_obj_profile_sse2.exe "D:\\temp\\obj_tests\\20190810_BearTrap_Ground_Model.obj" --modes parser --workers 1,2,4,8,16 --rounds 3 --output build/parser-scaling.json</pre>
<p>For the paired compiler comparison, use <code>--modes prepare --workers 16 --rounds 3 --compare-executable build/vs2026-vcpkg/bin/Release/woby_obj_profile_avx2.exe</code>. For read controls, use <code>--modes read-cached,read-native --workers 1,8,16</code>. Add <code>--instructions</code> to a separate run for code-location samples. Decode samples against the matching executable before rebuilding it. The run records contain executable and probe-source SHA-256 hashes. The instruction script accepts <code>--samples</code> and <code>--assembly</code> paths. AVX2 measurements need an AVX2-capable CPU with OS support.</p>
<p><a href="summary.json">Summary JSON</a> · <a href="measurements.zip">Raw measurement archive</a> · <a href="overview.png">PNG figure</a> · <a href="overview.svg">SVG figure</a> · <a href="{raw_relative}generate.py">Source instrumentation</a> · <a href="{raw_relative}profile.cpp">Counters and sampler</a> · <a href="{raw_relative}instructions.py">Instruction decoder</a></p>
<p>Production loader source was not changed. The research targets are opt-in. The Release application and probes build without warnings. All 80 default-preset tests and all 8 slow-preset tests pass. No tests were added for report text.</p>
<p class="small">Counter definitions: <a href="https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes">GetProcessTimes</a>, <a href="https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessiocounters">GetProcessIoCounters</a>, <a href="https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex">PROCESS_MEMORY_COUNTERS_EX</a>, <a href="https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadcontext">GetThreadContext</a>.</p>
</main>{FLOW_SCRIPT}</html>'''
    # Matplotlib emits trailing spaces in SVG path lines, including inline figures.
    for svg_path in destination.glob("*.svg"):
        exported = svg_path.read_text(encoding="utf-8")
        svg_path.write_text(re.sub(r"[ \t]+$", "", exported, flags=re.MULTILINE), encoding="utf-8")
    (destination / "report.html").write_text(re.sub(r"[ \t]+$", "", text, flags=re.MULTILINE), encoding="utf-8")
    print(json.dumps({"report": str(destination/'report.html'), "cpu_ready_sse2_ms": total,
                      "cpu_ready_avx2_ms": avx_total,"mapping_secondary_percent": hash_share,"memory_copy_GB_per_second": memcpy_rate},indent=2))


if __name__ == "__main__":
    main()
