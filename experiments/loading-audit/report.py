# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib>=3.10", "numpy>=2", "psutil>=6", "duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Create a self-contained HTML report and reusable research plots."""
from collections import defaultdict
import csv
import html
import json
import re
from pathlib import Path
import shutil
import statistics
import sys
import zipfile

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from analyze import summarize, read

COLORS = {"parser": "#1f7283", "annotation": "#b05a38", "other": "#667786", "points": "#788747", "gpu": "#826c96"}
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "svg.fonttype": "none",
    "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.alpha": .15})
LABELS = {"parser_total": "OBJ parser", "parser_scan_collect": "Parser read + decode", "parser_merge": "Parser merge",
    "coordinate_origin": "Coordinate origin", "coordinate_rebase": "Coordinate rebase", "corner_mapping": "Corner mapping",
    "mapping_id_resolution": "Vertex ID resolution", "normal_generation": "Normal generation",
    "normal_face_accumulation": "Normal face accumulation", "render_bounds": "Render bounds",
    "post_model_preparation": "Point / model branch", "post_source_indices": "Source triangle indices",
    "post_original_bounds": "Original bounds", "point_membership_build": "Point membership",
    "point_morton_keys": "Point Morton keys", "point_radix_sort": "Point radix sort", "point_sorted_gather": "Point gather",
    "point_tree_build": "Point tree", "post_annotation_cache": "Annotation branch",
    "annotation_blocks_fingerprints": "Annotation blocks + fingerprints", "annotation_morton_keys": "Annotation Morton keys",
    "annotation_radix_sort": "Annotation radix sort + output", "annotation_tree_build": "Annotation tree",
    "cpu_import": "CPU import", "gpu_upload_worker_total": "GPU upload worker", "post_load_cpu_total": "Post-load CPU",
    "post_point_preparation": "Point preparation", "gpu_buffer_allocate_and_staging": "GPU allocate + staging"}


def label(name):
    return LABELS.get(name, name.replace("_", " ").capitalize())


def parser_function_label(row):
    name = row["function"]
    names = (("fast_float::", "Decimal conversion"), ("PrototypeSimpleFace", "Position-only face probe"),
        ("ParseFace", "General face references"), ("PrototypePosition", "Position records"),
        ("PrototypeAttribute", "UV records"), ("PrototypeCompleteLines", "Line boundary dispatch"),
        ("PrototypeLine", "Prototype line dispatch"), ("ProcessLine", "Ordinary line dispatch"),
        ("_Find_impl", "STL byte find (AVX2)"), ("CopyIndices", "Index merge / validation"))
    for pattern, caption in names:
        if pattern in name:
            return caption
    if name == "[unresolved function]":
        return Path(row["module"]).name + " copies / fills"
    return name.split("(", 1)[0].split("::")[-1]


def micro_group(data, kind, scope, workers=16, placement="all"):
    return next(row for row in data["parser_microarchitecture"]["groups"] if
        (row["kind"], row["scope"], row["workers"], row["placement"]) == (kind, scope, workers, placement))


def table(headers, rows):
    return '<div class="table-wrap"><table><thead><tr>' + ''.join('<th>' + html.escape(str(v)) + '</th>' for v in headers) + '</tr></thead><tbody>' + ''.join(
        '<tr>' + ''.join('<td>' + html.escape(str(v)) + '</td>' for v in row) + '</tr>' for row in rows) + '</tbody></table></div>'


def save(fig, output, name, caption):
    fig.savefig(output / (name + ".png"), dpi=170, bbox_inches="tight")
    fig.savefig(output / (name + ".svg"), bbox_inches="tight")
    plt.close(fig)
    svg = (output / (name + ".svg")).read_text(encoding="utf-8")
    svg = svg[svg.index("<svg"):]
    for identifier in re.findall(r'\bid="([^"]+)"', svg):
        unique = name + "-" + identifier
        svg = svg.replace('id="' + identifier + '"', 'id="' + unique + '"')
        svg = svg.replace('url(#' + identifier + ')', 'url(#' + unique + ')')
        svg = svg.replace('href="#' + identifier + '"', 'href="#' + unique + '"')
    svg = svg.replace('<svg ', '<svg role="img" aria-label="' + html.escape(caption, quote=True) + '" ', 1)
    return '<figure>' + svg + '<figcaption>' + html.escape(caption) + f' · <a href="{name}.png">PNG</a> · <a href="{name}.svg">SVG</a></figcaption></figure>'


def charts(data, output):
    result = {}
    endpoints = data["endpoint_rows"]
    groups = data["native"]["groups"]
    fig, ax = plt.subplots(figsize=(10, 3.2))
    for i, name in enumerate(("production-visible", "traced-visible", "production-hidden", "traced-hidden")):
        values = [r["ready_rpc_ms"] / 1000 for r in endpoints if r["configuration"] == name]
        ax.scatter(values, [i] * len(values), s=45, color=COLORS["parser"])
        ax.plot([min(values), max(values)], [i, i], color=COLORS["parser"], alpha=.5)
        ax.text(max(values) + .08, i, f"median {statistics.median(values):.3f} s", va="center")
    ax.set_yticks(range(4), ["Production · visible", "Traced · visible", "Production · hidden", "Traced · hidden"])
    ax.invert_yaxis(); ax.set_xlabel("Request → completed load RPC (seconds)"); ax.set_xlim(12.8, max(r["ready_rpc_ms"] / 1000 for r in endpoints) + 1.3)
    result["native"] = save(fig, output, "native-controls", "Three fresh processes per configuration. Range lines connect the observed minimum and maximum; they are not confidence intervals.")

    rep = data["representative"]
    zero = rep["phase_zero_ms"]
    selected = [("cpu_import", 0), ("parser_total", 1), ("corner_mapping", 2), ("normal_generation", 3),
                ("gpu_upload_worker_total", 4), ("post_model_preparation", 5), ("post_annotation_cache", 6),
                ("annotation_blocks_fingerprints", 7), ("annotation_morton_keys", 8), ("annotation_radix_sort", 9), ("annotation_tree_build", 10)]
    fig, ax = plt.subplots(figsize=(12, 5.2))
    tick_names = []
    for name, lane in selected:
        tick_names.append(label(name))
        for phase in rep["phases"]:
            if phase["name"] != name:
                continue
            color = COLORS["annotation"] if name.startswith("annotation") or name == "post_annotation_cache" else COLORS["points"] if name == "post_model_preparation" else COLORS["gpu"] if name.startswith("gpu") else COLORS["parser"]
            ax.barh(lane, phase["wall_ms"] / 1000, left=(phase["start_ms"] - zero) / 1000, height=.66, color=color)
    for field, text in (("first_scene_frame_gpu_complete", "First model frame"), ("ready_toast_drawn", "Ready toast")):
        value = rep["endpoints_ms"][field] / 1000
        ax.axvline(value, color="#444444", linewidth=1, linestyle="--"); ax.text(value + .06, -.8, f"{text}: {value:.2f} s", fontsize=9, rotation=0)
    ax.set_yticks(range(len(tick_names)), tick_names); ax.invert_yaxis(); ax.set_xlabel("Seconds after model.add is handled"); ax.set_xlim(0, rep["endpoints_ms"]["ready_toast_frame_gpu_complete"] / 1000 + .6)
    result["timeline"] = save(fig, output, "pipeline-timeline", "One actual visible trace near the median. Rows overlap. Point and annotation work start together; the longer branch controls readiness.")

    names = ["parser_total", "corner_mapping", "normal_generation", "post_source_indices", "point_membership_build",
             "point_sorted_gather", "annotation_blocks_fingerprints", "annotation_morton_keys", "annotation_radix_sort", "annotation_tree_build"]
    selected_rows = [next(p for p in rep["phase_rows"] if p["name"] == name) for name in names]
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5), sharey=True, gridspec_kw={"width_ratios": [1.2, 1]})
    y = np.arange(len(selected_rows))
    axes[0].barh(y, [p["wall_ms"] / 1000 for p in selected_rows], color=COLORS["parser"])
    axes[1].barh(y, [p["logical_cpus"] for p in selected_rows], color=COLORS["other"])
    axes[0].set_yticks(y, [label(p["name"]) for p in selected_rows]); axes[0].invert_yaxis()
    axes[0].set_xlabel("Wall time (s)"); axes[1].set_xlabel("Average busy logical CPUs"); axes[1].set_xlim(0, 16)
    axes[1].axvline(16, color="#888888", linewidth=1)
    result["budgets"] = save(fig, output, "phase-budgets", "Process CPU time ÷ phase wall time. Concurrent point work and rendering are included. These rows are not additive; caller CPU is separate in the detailed table.")

    series = rep["series"]
    fig, axes = plt.subplots(4, 1, figsize=(12, 8), sharex=True)
    cpu = series["cpu"]
    axes[0].plot([r["time_s"] for r in cpu], [r["logical_cpus"] for r in cpu], color=COLORS["parser"])
    axes[0].set_ylabel("Busy logical CPUs"); axes[0].set_ylim(0, 16.5)
    for field, caption, color in (("private_bytes", "CPU private commit", COLORS["parser"]), ("working_set", "CPU working set", COLORS["other"])):
        axes[1].plot([r["time_s"] for r in series["memory"]], [r[field] / 1024**3 for r in series["memory"]], label=caption, color=color)
    system = series["system"]
    for field, caption, color in (("gpu_process_dedicated_bytes", "Process dedicated GPU memory", COLORS["gpu"]), ("gpu_process_shared_bytes", "Process shared GPU memory", COLORS["annotation"])):
        valid = [r for r in system if isinstance(r["values"].get(field), list)]
        axes[1].plot([r["time_s"] for r in valid], [sum(v["value"] for v in r["values"][field]) / 1024**3 for r in valid], label=caption, color=color)
    axes[1].set_ylabel("GiB"); axes[1].legend(loc="upper right", fontsize=8)
    axes[2].plot([r["time_s"] for r in series["gpu"]], [r["gpu_percent"] for r in series["gpu"]], color=COLORS["gpu"], label="NVML device utilization")
    for kind, color in (("3D", COLORS["annotation"]), ("Copy", COLORS["points"])):
        from analyze import engine_values
        rows = [(r["time_s"], engine_values(r["values"].get("gpu_engines"), rep["pid"], kind)) for r in system]
        rows = [(t, max(v)) for t, v in rows if v]
        axes[2].plot([r[0] for r in rows], [r[1] for r in rows], label="Process " + kind + " engine", color=color)
    axes[2].set_ylabel("GPU counter (%)"); axes[2].legend(loc="upper right", fontsize=8)
    valid = [r for r in system if isinstance(r["values"].get("disk_read_bytes_per_second"), (int, float))]
    axes[3].plot([r["time_s"] for r in valid], [r["values"]["disk_read_bytes_per_second"] / 1e6 for r in valid], color=COLORS["other"], label="System physical disk reads")
    axes[3].set_ylabel("Disk MB/s"); axes[3].set_xlabel("Seconds after model.add is handled")
    for ax in axes:
        for field in ("first_scene_frame_gpu_complete", "ready_toast_drawn"):
            ax.axvline(rep["endpoints_ms"][field] / 1000, color="#666666", alpha=.4, linewidth=1)
        ax.set_xlim(-.5, rep["endpoints_ms"]["ready_toast_frame_gpu_complete"] / 1000 + 1)
    result["resources"] = save(fig, output, "resources", "A fresh warm-cache load. CPU/memory samples are about 100 ms apart; Windows engine and disk counters are about one second apart. NVML values include other GPU processes.")

    if "power" in data:
        power = data["power"]
        fig, axes = plt.subplots(2, 1, figsize=(12, 4.8), sharex=True)
        axes[0].plot([s["time_s"] for s in power], [s["max_lp_frequency_MHz"] / 1000 for s in power], color=COLORS["parser"], label="Maximum effective logical CPU frequency")
        axes[0].plot([s["time_s"] for s in power], [s["mean_effective_frequency_MHz"] / 1000 for s in power], color=COLORS["other"], label="Mean across all 16 logical CPUs", alpha=.8)
        axes[0].set_ylabel("GHz"); axes[0].legend(fontsize=8)
        axes[1].plot([s["time_s"] for s in power], [s["package_power_W"] for s in power], color=COLORS["annotation"])
        axes[1].set_ylabel("CPU package W"); axes[1].set_xlabel("Seconds after model.add is handled, separate power capture")
        axes[1].set_xlim(-.5, 17)
        result["power"] = save(fig, output, "frequency-power", "100 ms AMD uProf samples. The mean includes idle logical CPUs. A lower mean during serial work does not by itself indicate throttling; these are effective-frequency counters, not instantaneous clock readings.")

    threads = series["threads"]
    totals = defaultdict(float)
    for sample in threads:
        for tid, value in sample["usage"].items(): totals[tid] += value
    tids = sorted(totals, key=totals.get, reverse=True)[:32]
    names_by_tid = {}
    for worker in rep["parser_workers"]: names_by_tid[str(worker["thread_id"])] = "parser"
    for p in rep["phases"]:
        if p["name"] in ("cpu_import", "post_model_preparation", "post_annotation_cache", "gpu_upload_worker_total"):
            names_by_tid[str(p["thread_id"])] = label(p["name"])
    matrix = np.array([[sample["usage"].get(tid, 0) for sample in threads] for tid in tids])
    fig, ax = plt.subplots(figsize=(12, 7.8))
    im = ax.imshow(matrix, aspect="auto", interpolation="nearest", vmin=0, vmax=1, cmap="Blues",
        extent=[threads[0]["time_s"], threads[-1]["time_s"], len(tids) - .5, -.5])
    ax.set_yticks(range(len(tids)), [names_by_tid.get(tid, "helper / runtime") + " · " + tid for tid in tids], fontsize=8)
    ax.set_xlim(0, rep["endpoints_ms"]["ready_toast_frame_gpu_complete"] / 1000 + .2); ax.set_xlabel("Seconds after model.add is handled"); ax.grid(False)
    fig.colorbar(im, ax=ax, label="Fraction of one logical CPU", shrink=.7)
    result["threads"] = save(fig, output, "worker-timeline", "CPU progress for the 32 most active threads. Thread creation/exit can hide part of a sample interval. The Windows scheduler can move these threads between physical cores.")

    parsers = data["parser"]
    fig, axes = plt.subplots(1, 2, figsize=(12, 3.8))
    for prefix, workers, color, caption in (("file", [1, 2, 4, 8, 16, 32], COLORS["parser"], "Current fast_float 3.5.1"),
                                            ("modern", [1, 8, 16], COLORS["annotation"], "Research fast_float 8.3.2")):
        rows = [parsers[f"{prefix}-w{w}"] for w in workers]
        values = [r["total_ms"]["median"] / 1000 for r in rows]
        errors = [[(r["total_ms"]["median"] - r["total_ms"]["min"]) / 1000 for r in rows], [(r["total_ms"]["max"] - r["total_ms"]["median"]) / 1000 for r in rows]]
        axes[0].errorbar(workers, values, yerr=errors, marker="o", color=color, label=caption, capsize=3)
        axes[1].plot(workers, [r["parse_cpu_ms"]["median"] / 1000 for r in rows], marker="o", color=color, label=caption)
    for ax in axes:
        ax.set_xscale("log", base=2); ax.set_xticks([1, 2, 4, 8, 16, 32], [1, 2, 4, 8, 16, 32]); ax.set_xlabel("Requested parser workers")
    axes[0].set_ylabel("Wall seconds"); axes[0].legend(fontsize=8); axes[1].set_ylabel("Total process CPU seconds")
    result["parser_scaling"] = save(fig, output, "parser-scaling", "Three runs per point. The one-worker file path uses bounded blocks; multiple workers use native read ranges. CPU seconds show the extra resource cost of lower latency.")

    cases = ["file-w16", "file-w32", "file-block-1MiB", "file-block-2MiB", "file-block-8MiB", "file-block-16MiB", "modern-w16", "memory-w16", "modern-memory-w16", "legacy-w16"]
    fig, ax = plt.subplots(figsize=(12, 5))
    for i, key in enumerate(cases):
        row = parsers[key]; scan = row["scan_ms"]["median"] / 1000; merge = row["merge_ms"]["median"] / 1000
        total = row["total_ms"]["median"] / 1000; preload = row["input_prepare_ms"]["median"] / 1000
        ax.barh(i, scan, color=COLORS["parser"], label="Read + decode / collect" if i == 0 else None)
        ax.barh(i, merge, left=scan, color=COLORS["points"], label="Merge" if i == 0 else None)
        ax.barh(i, max(0, total - scan - merge), left=scan + merge, color=COLORS["other"], label="Setup + cleanup" if i == 0 else None)
        if preload > .001:
            ax.barh(i, preload, left=total, color="#d8c8bb", label="Preload + allocation" if key == "memory-w16" else None)
        ax.text(total + preload + .05, i, f"parse {total:.2f} s" + (f" + preload {preload:.2f} s" if preload > .001 else ""), va="center", fontsize=8)
    ax.set_yticks(range(len(cases)), cases); ax.invert_yaxis(); ax.set_xlabel("Seconds"); ax.legend(fontsize=8, loc="lower right"); ax.set_xlim(0, 11)
    result["parser_cases"] = save(fig, output, "parser-controls", "Preload is outside the parse timer, but is included here. Stacked segment medians can differ slightly from the median total. Memory input also has a different task partition.")

    hardware = data["hardware"]["branch"]
    stage_names = ["parser_total", "mapping_id_resolution", "normal_face_accumulation", "annotation_blocks_fingerprints", "annotation_morton_keys", "annotation_tree_build"]
    selected = [next(s for s in hardware["stages"] if s["name"] == name) for name in stage_names]
    categories = ["integer/other", "control flow", "AVX scalar", "AVX128 packed/move", "AVX256", "SSE scalar", "SSE128 packed/move", "AVX512", "unresolved"]
    colors = ["#8a9299", "#bbc0c4", "#1f7283", "#7caaa9", "#b05a38", "#826c96", "#b1a3c6", "#495d42", "#e1b45e"]
    fig, ax = plt.subplots(figsize=(12, 4))
    offsets = np.zeros(len(selected))
    for category, color in zip(categories, colors):
        values = []
        for stage in selected:
            counts = stage["instruction_sample_categories"]
            values.append(counts.get(category, 0) / max(1, sum(counts.values())) * 100)
        ax.barh(range(len(selected)), values, left=offsets, color=color, label=category)
        offsets += values
    ax.set_yticks(range(len(selected)), [label(s["name"]) for s in selected]); ax.invert_yaxis(); ax.set_xlim(0, 100)
    ax.set_xlabel("Share of decoded retired-event samples (%)"); ax.legend(ncol=3, fontsize=8, bbox_to_anchor=(0, -.18), loc="upper left")
    result["instructions"] = save(fig, output, "sampled-instructions", "Biased sample locations, with sampling skid. These are not exact opcode counts or vectorization percentages. Concurrent worker and renderer instructions are included in each interval.")

    if "kernel_summary" in data:
        keys = ["combined_current_calls", "combined_inline_hash", "combined_inline_hash_direct_position", "hash_inline_only", "bounds_api_only", "bounds_direct_only", "bounds_direct_parallel"]
        fig, ax = plt.subplots(figsize=(12, 4.5))
        for i, key in enumerate(keys):
            row = data["kernel_summary"][key]
            ax.barh(i, row["median"] / 1000, color=COLORS["annotation"] if key.startswith("combined") else COLORS["other"])
            ax.errorbar(row["median"] / 1000, i, xerr=[[(row["median"] - row["min"]) / 1000], [(row["max"] - row["median"]) / 1000]], color="#333333", capsize=3)
            ax.text(row["max"] / 1000 + .04, i, f"{row['median'] / 1000:.3f} s", va="center")
        ax.set_yticks(range(len(keys)), [key.replace("_", " ") for key in keys]); ax.invert_yaxis(); ax.set_xlabel("Isolated replay seconds; 3 repetitions")
        result["kernels"] = save(fig, output, "annotation-replays", "The same full model and stable geometry. Applicable fingerprints and every block/precise group bound match the production cache. The three combined kernels do the same work; the smaller probes isolate parts of that work.")
    if "parser_simd" in data:
        simd = data["parser_simd"]
        scope = simd["native_samples"]["scopes"][0]
        functions = scope["functions"][:10]
        values = [row["cycle_samples"] / scope["cycle_samples"] * 100 for row in functions]
        names = [parser_function_label(row) for row in functions]
        names.append("Other worker locations"); values.append(100 - sum(values))
        fig, ax = plt.subplots(figsize=(12, 5))
        ax.barh(range(len(values)), values, color=[COLORS["annotation"] if "face" in name else COLORS["parser"] for name in names])
        for i, value in enumerate(values):
            ax.text(value + .35, i, f"{value:.1f}%", va="center", fontsize=9)
        ax.set_yticks(range(len(names)), names); ax.invert_yaxis(); ax.set_xlim(0, 35)
        ax.set_xlabel("Share of cycle-event samples on the 16 read/decode workers (%)")
        result["parser_hotspots"] = save(fig, output, "parser-worker-hotspots", "One production-like AVX2 diagnostic load. Only native parser read/decode workers are included. This is sampled CPU cost at instruction locations, not a wall-time split. Runtime instructions with unresolved function names are retained.")
        comparison = simd["compiler_comparison"]
        fig, axes = plt.subplots(1, 2, figsize=(12, 4))
        for ax, workers in zip(axes, (1, 16)):
            for i, isa in enumerate(("SSE2", "AVX2")):
                group = next(row for row in comparison["groups"] if row["workers"] == workers and row["isa"] == isa)
                row = group["wall_ms"]
                ax.bar(i, row["median"] / 1000, color=COLORS["other"] if isa == "SSE2" else COLORS["parser"], width=.5)
                ax.scatter(np.linspace(i - .12, i + .12, len(row["values"])), np.array(row["values"]) / 1000,
                    color="#26343d", marker="o", s=28, zorder=3)
                ax.text(i, row["max"] / 1000 + (.7 if workers == 1 else .08), f"{row['median'] / 1000:.3f} s", ha="center")
            ax.set_xticks((0, 1), ("SSE2-targeted parser", "AVX2-targeted parser"))
            ax.set_title(f"{workers} worker" + (" (bounded blocks)" if workers == 1 else "s (native ranges)"))
            ax.set_ylabel("Parser seconds"); ax.set_ylim(0, 24 if workers == 1 else 4)
        result["parser_isa"] = save(fig, output, "parser-isa-comparison", "Three fresh processes per compiler setting and worker count; cache warmed before each, rotating order. Bars are medians; dots are individual runs. Shared STL/runtime helpers can execute AVX2 in both builds. Sampled runs are excluded.")
    if "parser_microarchitecture" in data:
        micro = data["parser_microarchitecture"]
        branch = micro_group(data, "branch", "read_decode_workers")
        functions = [row for row in branch["functions"] if row["function"] != "[unresolved function]"][:8]
        fig, axes = plt.subplots(1, 2, figsize=(12, 4.6), sharey=True)
        y = np.arange(len(functions))
        axes[0].barh(y, [row["metrics"]["branches_per_kinst"] for row in functions], color=COLORS["parser"])
        axes[1].barh(y, [row["metrics"]["branch_misses_per_kinst"] for row in functions], color=COLORS["annotation"])
        axes[0].set_yticks(y, [parser_function_label(row) for row in functions]); axes[0].invert_yaxis()
        axes[0].set_xlabel("Estimated branches / 1,000 retired instructions")
        axes[1].set_xlabel("Estimated branch misses / 1,000 retired instructions")
        result["parser_branches"] = save(fig, output, "parser-branches", "Three pooled parser-only branch captures, 16 workers. Separate axes show branch density and misses. Event-based attribution has skid; function ratios are approximate. Sparse miss counts are retained in the evidence table.")

        read_cache = micro_group(data, "data", "read_decode_workers")["metrics"]
        merge_cache = micro_group(data, "data", "merge_all")["metrics"]
        fig, axes = plt.subplots(1, 2, figsize=(12, 3.5))
        offsets = np.zeros(2)
        for key, caption, color in (("043:01", "Local L2", COLORS["parser"]), ("043:02", "L3 / peer L2", COLORS["points"]),
                                    ("043:08", "Local DRAM / IO", COLORS["annotation"])):
            values = [row["demand_source_percent"][key] for row in (read_cache, merge_cache)]
            axes[0].barh((0, 1), values, left=offsets, color=color, label=caption)
            offsets += values
        axes[0].set_yticks((0, 1), ("Read/decode workers", "Merge")); axes[0].invert_yaxis()
        axes[0].set_xlim(0, 100); axes[0].set_xlabel("Source share of selected demand refills (%)")
        axes[0].legend(ncol=3, fontsize=8, bbox_to_anchor=(0, -.3), loc="upper left")
        values = [row["demand_fills_per_1000_ls_dispatch"] for row in (read_cache, merge_cache)]
        axes[1].barh((0, 1), values, color=(COLORS["parser"], COLORS["annotation"]))
        axes[1].set_yticks((0, 1), ("", "")); axes[1].invert_yaxis(); axes[1].tick_params(axis="y", length=0)
        for i, value in enumerate(values): axes[1].text(value + .06, i, f"{value:.3f}", va="center")
        axes[1].set_xlim(0, 5); axes[1].set_xlabel("Demand refills / 1,000 dispatched LS operations")
        result["parser_cache"] = save(fig, output, "parser-cache-refills", "Two pooled data-cache captures. These are demand refill events, not the percentage of all accesses that miss cache. Prefetches, write traffic, and kernel-mode work are outside this source mix.")

        pressure_names = [("Store queue", "backend", "store_queue_stall_cycle_percent"),
            ("Load queue", "backend", "load_queue_stall_cycle_percent"),
            ("Integer schedulers", "backend", "integer_scheduler_stall_cycle_percent"),
            ("Integer registers", "backend", "integer_register_stall_cycle_percent"),
            ("Op queue empty", "frontend", "op_queue_empty_cycle_percent")]
        fig, ax = plt.subplots(figsize=(12, 4))
        y = np.arange(len(pressure_names))
        for shift, scope, caption, color in ((-.18, "read_decode_workers", "Read/decode", COLORS["parser"]),
                                             (.18, "merge_all", "Merge", COLORS["annotation"])):
            values = [micro_group(data, kind, scope)["metrics"][key] for _, kind, key in pressure_names]
            ax.barh(y + shift, values, height=.32, color=color, label=caption)
        ax.set_yticks(y, [name for name, _, _ in pressure_names]); ax.invert_yaxis()
        ax.set_xlabel("Selected queue/dispatch event counts / non-halted cycle count (%)"); ax.legend()
        result["parser_pressure"] = save(fig, output, "parser-dispatch-pressure", "Separate frontend/backend diagnostic captures. Stall conditions overlap, and selected scheduler masks combine classes. These ratios do not partition all CPU cycles or prove a specific memory-bandwidth limit.")

        names = {"w8-physical": "8 workers / 8 cores pinned", "w8-all": "8 workers / all CPUs allowed",
            "w8-four-core-smt": "8 workers / 4 cores + SMT", "w16-all": "16 workers / all CPUs"}
        rows = micro["affinity"]
        fig, axes = plt.subplots(1, 2, figsize=(12, 4.2), sharey=True)
        y = np.arange(len(rows))
        for i, row in enumerate(rows):
            wall = row["wall_ms"]
            axes[0].barh(i, wall["median"] / 1000, color=COLORS["parser"])
            axes[0].scatter(np.array(wall["values"]) / 1000, np.linspace(i - .09, i + .09, len(wall["values"])), color="#26343d", s=22)
            axes[1].barh(i, row["cpu_ms"] / 1000, color=COLORS["annotation"])
        axes[0].set_yticks(y, [names[row["name"]] for row in rows]); axes[0].invert_yaxis()
        axes[0].set_xlabel("Parser wall seconds; dots are individual runs")
        axes[1].set_xlabel("Median process CPU seconds during parsing")
        result["parser_affinity"] = save(fig, output, "parser-affinity", "Three fresh unprofiled processes per setting. Child affinity is verified before measurements. The eight-worker placement controls keep worker/range counts equal. Comparing eight with sixteen also changes range partitioning and allocation work.")

        ranges = sorted(micro["allocation"]["allocation_audit"]["ranges"], key=lambda row: row["byte_begin"])
        y = np.arange(len(ranges)); fig, axes = plt.subplots(1, 2, figsize=(12, 6.7), sharey=True)
        offsets = np.zeros(len(ranges))
        for key, caption, color in (("positions", "v", COLORS["parser"]), ("texcoords", "vt", COLORS["points"]), ("faces", "f", COLORS["annotation"])):
            values = np.array([row[key] / max(1, row["positions"] + row["texcoords"] + row["faces"]) * 100 for row in ranges])
            axes[0].barh(y, values, left=offsets, color=color, label=caption); offsets += values
        axes[0].set_yticks(y, [f"Range {i}: {row['byte_begin'] / 1e9:.2f} GB" for i, row in enumerate(ranges)])
        axes[0].invert_yaxis(); axes[0].set_xlim(0, 100); axes[0].set_xlabel("Decoded record mix (% of rows)")
        axes[0].legend(ncol=3, loc="lower left", bbox_to_anchor=(0, 1.01))
        axes[1].barh(y - .18, [row["wall_ms"] / 1000 for row in ranges], height=.32, color=COLORS["other"], label="Wall")
        axes[1].barh(y + .18, [row["cpu_ms"] / 1000 for row in ranges], height=.32, color=COLORS["parser"], label="Thread CPU")
        axes[1].set_xlabel("Range seconds in the allocation diagnostic")
        axes[1].legend(ncol=2, loc="lower left", bbox_to_anchor=(0, 1.01))
        result["parser_ranges"] = save(fig, output, "parser-range-work", "One separate allocation/range diagnostic, excluded from timing medians. Nearly equal byte ranges contain different record types and row counts. Edge reconstruction on the caller is outside these range counts. Wall/CPU gaps include scheduling and overlapped IO.")

        logical = {"position-double-tuples": micro["allocation"]["positions"] * 24,
            "index-triplets": micro["allocation"]["faces"] * 3 * 12,
            "double-attributes": data["census"]["records"]["vt"] * 16,
            "byte-buffers(flags-and-face-counts)": micro["allocation"]["faces"] * 4}
        rows = [row for row in micro["allocation"]["allocation_audit"]["buffers"] if row["growths"]]
        labels = {"position-double-tuples": "Position doubles", "index-triplets": "Index triplets",
            "double-attributes": "UV doubles", "byte-buffers(flags-and-face-counts)": "Offset flags + face counts"}
        fig, ax = plt.subplots(figsize=(12, 3.6)); y = np.arange(len(rows))
        ax.barh(y - .18, [logical[row["kind"]] / 1e9 for row in rows], height=.32, color=COLORS["parser"], label="Logical temporary payload")
        ax.barh(y + .18, [row["copy_payload_bytes"] / 1e9 for row in rows], height=.32, color=COLORS["annotation"], label="Extra growth-copy payload")
        ax.set_yticks(y, [labels[row["kind"]] for row in rows]); ax.invert_yaxis(); ax.set_xlabel("Decimal GB"); ax.legend()
        result["parser_growth"] = save(fig, output, "parser-buffer-growth", "Counters execute only on growable Buffer allocations. Growth-copy payload is measured at each actual copy. Read plus write traffic for those copies is at least twice the payload. This is logical traffic, not measured DRAM bandwidth.")
    return result


def make(data, root, output):
    output.mkdir(parents=True, exist_ok=True)
    figures = charts(data, output)
    rep = data["representative"]; p = data["parser"]; groups = data["native"]["groups"]
    ready = groups["production-visible"]["load_rpc_ms"]["median"] / 1000
    toast = statistics.median(r["ready_toast_drawn"] for r in data["endpoint_rows"] if r["configuration"] == "traced-visible") / 1000
    first = statistics.median(r["first_scene_frame_gpu_complete"] for r in data["endpoint_rows"] if r["configuration"] == "traced-visible") / 1000
    current_parser = p["file-w16"]["total_ms"]["median"] / 1000
    modern_parser = p["modern-w16"]["total_ms"]["median"] / 1000
    best_replay = data.get("kernel_summary", {}).get("combined_inline_hash_direct_position", {}).get("median", 0) / 1000
    base_replay = data.get("kernel_summary", {}).get("combined_current_calls", {}).get("median", 0) / 1000
    phases_by_name = {r["name"]: r for r in rep["phase_rows"]}
    annotation = phases_by_name["post_annotation_cache"]["wall_ms"] / 1000
    blocks = phases_by_name["annotation_blocks_fingerprints"]["wall_ms"] / 1000
    model = phases_by_name["post_model_preparation"]["wall_ms"] / 1000
    sections = []
    sections.append(f'''<header><p class="eyebrow">Woby · current loading study · 14 October 2026</p><h1>BearTrap loading:<br>where the {ready:.1f} seconds go</h1>
<p class="lede">Measure from the load request through the <strong>Ready</strong> toast. Identify the slow stages, the resources they use, and the CPU instructions on their hot paths.</p>
<p class="meta">Revision <code>{data['provenance']['revision'][:12]}</code> · Release /O2 /arch:AVX2 · 7.11 GB OBJ · 75.77 million triangles · 38.41 million render vertices</p></header>
<nav><a href="#endpoints">Endpoints</a><a href="#pipeline">Wall time</a><a href="#resources">Resources</a><a href="#instructions">CPU instructions</a><a href="#parser">OBJ parsing</a><a href="#parser-simd">Parser SIMD</a><a href="#parser-deep">Branches / cache</a><a href="#replays">Annotation probes</a><a href="#next">Next steps</a><a href="#evidence">Evidence</a></nav>
<div class="kpis"><div><strong>{ready:.2f} s</strong><span>Production Ready RPC · median</span></div><div><strong>{toast:.2f} s</strong><span>Ready toast draw · traced median</span></div><div><strong>{first:.2f} s</strong><span>First model GPU frame · traced median</span></div><div><strong>{rep['process_peak_private_bytes'] / 1024**3:.2f} GiB</strong><span>Peak process private commit · traced run</span></div></div>
<aside><strong>The next large target is the annotation block and fingerprint pass.</strong> It takes {blocks:.2f} s in the representative run and largely runs on one thread. The parser is already parallel. A newer number decoder saves about {current_parser - modern_parser:.2f} s in the 16-worker parser control.</aside>
<p>The current Windows plan is <strong>Legion Performance Mode</strong>, with AC power and a 96% battery reading. The older BearTrap report used Balance Mode and an older revision. Its longer load times are historical context. This study makes no code-only speed claim from that comparison.</p>''')

    sections.append('<section id="endpoints"><h2>1. First display and Ready are separate endpoints</h2>' + figures["native"])
    sections.append(table(["Configuration", "Runs", "Ready RPC median s", "Observed range s"], [[name, row["load_rpc_ms"]["count"], f'{row["load_rpc_ms"]["median"] / 1000:.3f}', f'{row["load_rpc_ms"]["min"] / 1000:.3f}–{row["load_rpc_ms"]["max"] / 1000:.3f}'] for name, row in groups.items()]))
    sections.append('<p>The first visible production run is slower than the later two. It is retained in the table. Repeated file warming does not control CPU temperature, boost state, driver caches, or all background work. The trace is not consistently slower than production; the difference is within the run spread.</p>')
    sections.append(table(["Endpoint", "Representative time s", "Meaning"], [["Model frame GPU complete", f'{rep["endpoints_ms"]["first_scene_frame_gpu_complete"] / 1000:.3f}', "The first submitted frame with the model has completed on the GPU."],
        ["All tools ready", f'{rep["endpoints_ms"]["all_tools_ready"] / 1000:.3f}', "Post-load data and point upload are published."],
        ["Ready toast draw", f'{rep["endpoints_ms"]["ready_toast_drawn"] / 1000:.3f}', "The UI generates the Ready toast in this frame."],
        ["Ready toast frame GPU complete", f'{rep["endpoints_ms"]["ready_toast_frame_gpu_complete"] / 1000:.3f}', "The submitted frame with the Ready toast has completed."]]))
    sections.append('<p>These trace times start when the UI handles <code>model.add</code>. The external RPC timer starts just before the HTTP request and includes polling/response delay. GPU completion is an observed fence endpoint; monitor scan-out is not measured. Startup, model hashing, screenshots, and output validation are outside the loading endpoint.</p></section>')

    sections.append('<section id="pipeline"><h2>2. The wall-time path</h2>' + figures["timeline"] + figures["budgets"])
    sections.append(f'<p>CPU import takes {phases_by_name["cpu_import"]["wall_ms"] / 1000:.2f} s. The model then uploads and draws. Post-load model/point preparation takes {model:.2f} s while annotation preparation takes {annotation:.2f} s. Both start together. Annotation work controls the join. Improving the shorter point branch alone will have little effect on the Ready endpoint.</p>')
    sections.append('<div class="explorer"><h3>Inspect the recorded timeline</h3><label for="time">Time after request <output id="time-value">0.0 s</output></label><input id="time" type="range" min="0" step="0.1"><div id="time-detail" aria-live="polite"></div></div>')
    resource_table = [[label(r["name"]), f'{r["wall_ms"] / 1000:.3f}', f'{r["logical_cpus"]:.2f}', f'{r["machine_cpu_percent"]:.1f}%', f'{r["caller_cpu_ms"] / 1000:.3f}', f'{r["commit_end"] / 1024**3:.2f}', f'{r["read_bytes"] / 1e9:.2f}'] for r in rep["phase_rows"]]
    sections.append('<details><summary>Every substantial measured phase · wall time, CPU, memory, reads</summary>' + table(["Phase", "Wall s", "Process logical CPUs", "Of 16 logical CPUs", "Caller CPU s", "End private GiB", "Process reads GB"], resource_table) + '</details>')
    sections.append('<p>Parent phases include child phases. Overlapping worker phases are not added to obtain wall time. Process counters cover all viewer threads. A caller can be waiting while its helpers run. Windows thread/process CPU time is quantized; short intervals can show apparent caller utilization above 100%.</p></section>')

    sections.append('<section id="resources"><h2>3. What the computer can use in each part</h2>' + figures["resources"] + figures["threads"] + figures.get("power", ""))
    sections.append(table(["Part", "Observed use", "Available opportunity"], [["Read + parse", f'{phases_by_name["parser_scan_collect"]["logical_cpus"]:.1f} busy logical CPUs; multiple native read ranges', "16 workers nearly saturate scheduled CPU capacity. Extra workers show little latency gain."],
        ["ID resolution and normals", "About one working caller, plus UI/runtime activity", "Parallelism needs stable vertex IDs or deterministic normal reduction. A thread count change alone is insufficient."],
        ["Point branch", "Serial membership/gather work with parallel keys and sort", "The branch finishes before annotations; most of this time is hidden by the join."],
        ["Annotation blocks/fingerprints", "One annotation caller; other work overlaps at the start", "Reduce helper calls and repeated coordinate access. The fingerprint has an ordered dependency."],
        ["GPU uploads", "A worker copies into a bounded 16 MiB staging ring", "The initial upload is short relative to CPU work. Increase ring size only after a full-path test."],
        ["Memory", f'{rep["process_peak_working_set"] / 1024**3:.2f} GiB peak working set on 64 GB installed RAM', "Parser chunk and merged arrays coexist. Lower peak memory could matter more on smaller computers."]]))
    sections.append(table(["Read/copy control", "Median seconds", "Payload GB/s"], [[key, f'{v["wall_ms"]["median"] / 1000:.3f}', f'{v["GBps"]["median"]:.2f}'] for key, v in data["controls"].items()]))
    sections.append('<p>Cached reads use Windows cached file handles. Native read controls bypass the file cache and test device reads. They are not cold production loads: the current parser uses buffered native handles. No system cache was flushed. The copy test reports payload bandwidth; minimum read plus write traffic is twice that value. No memory-controller bandwidth was measured.</p>')
    sections.append('<p>GPU utilization from NVML is device-wide, not shader occupancy. The Windows engine counters are filtered to the viewer PID. Physical-disk and paging counters cover the system. GPU upload can use copy operations and wait for fences while CPU use is low. Such waits do not imply idle hardware.</p></section>')
    power_rows = [[label(row["name"]), row["samples"], f'{row["max_lp_frequency_MHz"].get("median", 0) / 1000:.2f}',
        f'{row["package_power_W"].get("median", 0):.1f}'] for row in data.get("phase_power", []) if row["name"] in
        ("parser_total", "mapping_id_resolution", "normal_generation", "annotation_blocks_fingerprints", "annotation_morton_keys", "annotation_tree_build")]
    sections[-1] = sections[-1].replace('</section>', '') + table(["Power capture interval", "Samples", "Median max effective LP GHz", "Median package W"], power_rows)
    gpu = data["gpu_trace"]
    sections.append(f'<p>Nsight recorded {gpu["nvtx_count"]} NVTX ranges and {gpu["target_events"]} target-associated WDDM events. GPU queue pairing retained {gpu["queue"]["matched"]} matched packets and {gpu["queue"]["unmatched_starts"]} unmatched starts. A matched packet duration is not total GPU occupancy. Transfers can include driver copies beyond the application payload.</p>' +
        table(["WDDM transfer class", "Events", "Recorded GB"], [[row["label"], row["count"], f'{row["bytes"] / 1e9:.3f}'] for row in gpu["transfer_rows"]]) + '</section>')

    sections.append('<section id="instructions"><h2>4. CPU instructions: permitted, present, and sampled</h2>' + figures["instructions"])
    sections.append(table(["Part", "Instruction path", "Meaning"], [["OBJ token and number parsing", "Integer loads, branches, decimal multiply/shift; scalar floating-point conversion", "The embedded fast_float ASCII path uses 64-bit SWAR for eight digits. It is not a wide floating-point parser."],
        ["Coordinate origin/rebase", "AVX/AVX2 baseline; explicit 256-bit coordinate kernels", "These short passes already process several values per vector. They are small wall-time targets."],
        ["ID resolution", "Integer addressing/comparison and lookup; vector moves where useful", "Each corner resolves to a stable ID. The primary path handles most corners; this is not an arithmetic kernel."],
        ["Normals", "Scalar and packed VEX floating-point operations, indexed writes", "AVX encoding can hold scalar operations. Triangle writes share vertices and constrain naive parallelism."],
        ["Annotation bounds / tree", "Integer hash chain, calls, scalar/128-bit FP and bounds operations", "Data layout and repeated access matter. Wider registers alone do not remove hash dependencies."],
        ["Morton keys / radix sort", "Integer bit interleave, histogram/offset work, copies; compiler vector operations", "These paths already use helper threads. Sorting is sensitive to memory traffic and task count."],
        ["GPU staging / waits", "CPU memory-copy/runtime code and Vulkan/driver calls", "A fence wait uses little CPU; CPU opcode mix does not describe GPU execution."]]))
    sections.append('<p>This Ryzen 7 5800H supports SSE2, SSE4.2, AVX, AVX2, FMA, BMI1, and BMI2. AVX-512 is not supported. Woby builds an isolated AVX-512 rebase backend but runtime dispatch does not select it here. The Release compiler baseline is <code>/arch:AVX2</code>. That permits AVX2; it does not make every loop wide SIMD. A 256-bit floating-point instruction can require AVX rather than AVX2. Moves also carry vectors without doing useful vector arithmetic.</p>')
    sections.append('<p>The chart uses native retired-instruction event sample locations decoded from the matching executable. Samples have skid and bias; the chart cannot state the percentage of all executed instructions that use SIMD. The known-loop calibration and raw instruction addresses are retained. <a href="https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64?view=msvc-170">Microsoft: x64 /arch</a> explains the distinction between AVX scalar, packed, and AVX2 code generation.</p>')
    if "calibration" in data:
        control = data["calibration"]; loop = control["control_loop"]
        fp = control["stages"][0]["counts"]["003:0f"]["estimated_events"]
        expected = control["probe"]["stats"]["expected_flops"]
        sections.append(f'<p>The current known SSE2 loop performs {expected:,} FLOPs; the sampled estimate is {fp:,.0f}, a {(fp / expected - 1) * 100:+.1f}% difference. Arithmetic occupies {loop["actual_arithmetic_percent"]:.1f}% of decoded loop instruction locations but {(loop["arithmetic_samples"] / loop["retired_samples"]) * 100:.1f}% of its retired-event samples. This directly shows why sampled opcode percentages must not be treated as dynamic instruction percentages.</p>')
    hardware_rows = []
    cache_by_name = {r["name"]: r for r in data["hardware"]["cache"]["stages"]}
    for row in data["hardware"]["branch"]["stages"]:
        if row["name"] not in ("parser_total", "mapping_id_resolution", "normal_face_accumulation", "annotation_blocks_fingerprints", "annotation_morton_keys", "annotation_tree_build"):
            continue
        cache = cache_by_name.get(row["name"], {}); counts = cache.get("counts", {})
        instructions = counts.get("0c0:00", {}).get("estimated_events", 0)
        dram = counts.get("043:08", {}).get("estimated_events", 0)
        hardware_rows.append([label(row["name"]), f'{row["ipc_estimate"]:.2f}', f'{row["branch_mpki_estimate"]:.2f}', f'{row["branch_mispredict_percent_estimate"]:.2f}%', f'{dram / instructions * 1000:.2f}' if instructions else '—'])
    sections.append(table(["Profile interval", "Estimated IPC", "Branch misses / 1k retired", "Branch miss ratio", "Local DRAM/IO demand fills / 1k retired"], hardware_rows))
    sections.append('<p>Each counter set is a separate diagnostic load. These are sampled estimates, with QPC metadata alignment and a 16 ms sensitivity check. Concurrent renderer/helpers are included. Demand fills exclude some traffic, include local IO, and do not yield total DRAM bytes. FP operations also need the processor-specific merge-counter rules; an FP event total cannot distinguish vector width. <a href="https://docs.amd.com/r/en-US/57368-uProf-user-guide/Predefined-Core-PMC-Events">AMD: core PMC definitions</a>.</p>')
    sections.append('<details><summary>Independent VTune CPU hotspot check</summary>' + table(["Function", "CPU time", "Module"], [[r.get("Function", ""), r.get("CPU Time", ""), r.get("Module", "")] for r in data["vtune_hotspots"][:15]]) + '</details></section>')

    sections.append('<section id="parser"><h2>5. OBJ parsing: scaling, memory, and practical trials</h2>' + figures["parser_scaling"] + figures["parser_cases"])
    if "census" in data:
        sections.append(table(["Physical record prefix", "Count"], [[key, f'{value:,}'] for key, value in data["census"]["records"].items() if value]))
        sections.append('<p>The full-file prefix census matches the parser position and face counts. This model has triangle faces and UVs, and no supplied normal records. Normal generation therefore adds work after parsing. The census measures column-zero physical record prefixes; it is not a full OBJ grammar validator.</p>')
    sections.append(table(["Configuration", "Median parse s", "Range s", "Busy logical CPUs", "CPU seconds", "Peak working set GiB", "Tasks/chunks"], [[key,
        f'{v["total_ms"]["median"] / 1000:.3f}', f'{v["total_ms"]["min"] / 1000:.3f}–{v["total_ms"]["max"] / 1000:.3f}', f'{v["average_logical_cpus"]["median"]:.2f}',
        f'{v["parse_cpu_ms"]["median"] / 1000:.2f}', f'{v["peak_working_set_after_parse"]["median"] / 1024**3:.2f}', int(v["chunks"]["median"])] for key, v in p.items()]))
    sections.append(f'<p><strong>Keep 16 as the current latency baseline.</strong> Eight workers take {p["file-w8"]["total_ms"]["median"] / 1000:.3f} s; sixteen take {current_parser:.3f} s; thirty-two take {p["file-w32"]["total_ms"]["median"] / 1000:.3f} s. Compare CPU seconds as well as wall seconds. Sixteen logical processors share eight physical cores.</p>')
    sections.append('<p><strong>The block-size setting changes the algorithm.</strong> At 4 MiB or more, the multi-worker file path selects long native read ranges, double buffers 256 KiB reads, and decodes while the next read is pending. Thus 8 and 16 MiB settings still produce 16 ranges; they do not enlarge the native read buffer. At 1 and 2 MiB, thousands of bounded blocks and async tasks increase allocation, merge, and thread overhead. <code>read_bytes</code> configures non-seekable streams, so changing it does not tune these file reads.</p>')
    sections.append(f'<p><strong>The modern number decoder gives a small full-load opportunity.</strong> With the same parser grammar, the research build of fast_float 8.3.2 takes {modern_parser:.3f} s versus {current_parser:.3f} s at 16 workers: {(1 - modern_parser / current_parser) * 100:.1f}% less parser wall time. Holding all other work constant predicts about {current_parser - modern_parser:.2f} s saved to Ready, or {(current_parser - modern_parser) / ready * 100:.1f}% of this full-load median. That prediction has not been verified in the production viewer. <a href="https://github.com/fastfloat/fast_float/releases/tag/v8.3.2">Upstream fast_float release</a>.</p>')
    sections.append('<p>For this ASCII OBJ, the new decoder still uses the byte/SWAR path. Its explicit eight-digit SIMD helper is selected for <code>char16_t</code>, not <code>char</code>. Updating the decoder does not automatically create an AVX2 OBJ parser.</p>')
    sections.append('<p><strong>The sampled position rows have six values: XYZ and RGB.</strong> The prototype parses and checks all three color values, then discards them for these rows. Woby retains the coordinates. Such rows need twice as many number conversions as plain XYZ rows. An unused-color validation fast path is a useful further parser trial, but it must preserve accepted syntax and rejection of non-finite/out-of-range values. The six-value observation comes from 4,096 sampled rows; it is not a full-file field-count census.</p>')
    sections.append('<p><strong>The current simple-face path does not fit the sampled BearTrap faces.</strong> <code>PrototypeSimpleFace</code> accepts positive position-only references. All 4,096 sampled face rows have three slash references. The probe converts the first position, sees the slash, and falls back to <code>ParseFace</code>. The general face parser is the largest parser cycle hotspot. A positive <code>v/vt</code> triangle path, or avoiding the failed probe, is a concrete next experiment. Keep the general path for negative references, normals, polygons, and diagnostic behavior.</p>')
    sections.append('<p><strong>Whole-file preloading is a poor trade in this control.</strong> It allocates and retains the 7.11 GB input before parsing. That preparation adds several seconds; peak working set rises to about 15.7 GiB. Memory parsing can reduce input calls but has 1,695 bounded tasks rather than 16 native ranges. This is not a clean IO-only subtraction, and it is not a memory-mapped-file test.</p>')
    sections.append('<p>All 48 parser samples use fresh processes, warmed input, rotated variant order, and the same ordered output fingerprint. The fingerprint checks position doubles, rendered float UV/normal values, shapes, indices, material IDs, smoothing, lines, and points. Material payloads and colors are not checked. It does not prove identical low bits for every prototype double UV/normal, or correctness for all OBJ grammar. Add full-precision and grammar tests before adopting the decoder.</p>')
    sections.append('<details><summary>Parser design and next experiments</summary><ul><li>Separate numeric decoding, face-reference parsing, allocation, and merge hot paths in the profile. Preserve the general grammar as a fallback for a measured triangle fast path.</li><li>Try a persistent bounded worker pool with more balanced tasks. Current long ranges are balanced by bytes, not decode cost. Avoid a thread launch for every tiny block.</li><li>Test batched newline/space/slash classification with AVX2 only if structural scanning is a hotspot. Decimal conversion already uses a fast scalar/SWAR algorithm.</li><li>Investigate counted output ranges or fewer simultaneously live chunk/merged arrays. A full sizing pass adds input work; measure its total cost and memory reduction.</li><li>Keep double source coordinates and deterministic source IDs. Float-only parsing can make the benchmark faster while reducing model accuracy.</li></ul></details>')
    if "parser_simd" in data:
        simd = data["parser_simd"]; comparison = simd["compiler_comparison"]
        scan, merge = simd["native_samples"]["scopes"]
        isa_groups = {(r["isa"], r["workers"]): r for r in comparison["groups"]}
        sse16 = isa_groups[("SSE2", 16)]["wall_ms"]["median"] / 1000
        avx16 = isa_groups[("AVX2", 16)]["wall_ms"]["median"] / 1000
        face = next(r for r in scan["functions"] if "::ParseFace(" in r["function"])
        decimal = next(r for r in scan["functions"] if "fast_float::from_chars_advanced<double>" in r["function"])
        probe = next(r for r in scan["functions"] if "PrototypeSimpleFace(" in r["function"])
        find = next(r for r in scan["functions"] if "_Find_impl" in r["function"])
        indices = next(r for r in merge["functions"] if "CopyIndices::Execute" in r["function"])
        sections.append('<h3 id="parser-simd">5a. SIMD inside the OBJ parser</h3>')
        sections.append('<p><strong>The parser already uses SIMD for byte search and bulk copying. Its main number and face decoders operate on one number or reference at a time.</strong> SWAR means several digit bytes packed into one 64-bit integer register. This is useful data parallelism, but it does not use the SSE/AVX vector registers.</p>')
        sections.append(table(["Parser part", "Instruction evidence", "What is processed together"], [
            ["ASCII decimal digits", "64-bit loads, multiply/add/shift and digit masks; fast_float 3.5.1 eight-digit SWAR helper", "Up to eight consecutive digits of one number. Separate OBJ numbers are not decoded as an AVX2 batch."],
            ["Decimal to double", "Compiled vcvtsi2sd, vmulsd, vdivsd; mostly integer mantissa/exponent work", "One double at a time. AVX encoding does not turn these scalar instructions into packed floating-point work."],
            ["Position and UV records", "Scalar decoder calls plus XMM stores and register setup", "XYZ/UV fields are parsed individually. Copying several bytes with an XMM register does not vectorize their text conversion."],
            ["Face references", "ParseFace: 308 static integer/control instruction locations in both compiler variants; no XMM/YMM instructions in this function", "One signed decimal index at a time, with slash, offset, and format checks. The simple-face probe is also integer/control code."],
            ["Newline / byte search", "Executed vpcmpeqb with YMM operands and vpmovmskb; SSE2-targeted probe also executes vpand with YMM operands", "A comparison can test 32 input bytes, then extract a delimiter mask. This is actual AVX2 structural search."],
            ["Chunk copies / output fills", "Executed vmovdqu and vmovntdq with YMM operands; rep stosb also appears", "32 bytes per vector transfer. These instructions move output/text buffers and do not decode numbers."],
            ["Index merge / validation", "CopyIndices::Execute: 80 static integer/control instruction locations in both variants; no XMM/YMM instructions", "Copies and validates each index triplet with its offset flags. Other merge copy tasks can still use SIMD runtime routines."]]))
        sections.append('<p>The AVX2 decimal-conversion listing also contains BMI2 shifts and two 128-bit packed comparisons in helper/fallback code. BMI2 operates on general-purpose integer registers. Those instructions do not form a batch decoder for separate OBJ numbers; the inspected conversion function contains no YMM instructions. The fast_float 8.3.2 ASCII path remains SWAR. Its explicit SIMD eight-digit helper is enabled for <code>char16_t</code>, while this OBJ uses <code>char</code>.</p>')
        sections.append(figures["parser_hotspots"])
        sections.append(f'<p>On the 16 read/decode workers, general face parsing accounts for {face["cycle_samples"] / scan["cycle_samples"] * 100:.1f}% of sampled cycles, decimal conversion {decimal["cycle_samples"] / scan["cycle_samples"] * 100:.1f}%, the position-only face probe {probe["cycle_samples"] / scan["cycle_samples"] * 100:.1f}%, and the already-vectorized byte finder {find["cycle_samples"] / scan["cycle_samples"] * 100:.1f}%. The scalar index-validation function accounts for {indices["cycle_samples"] / merge["cycle_samples"] * 100:.1f}% of samples in the selected merge-thread scope. These shares locate CPU work; they are not additive wall-time stages or exact executed-instruction percentages.</p>')
        sections.append(figures["parser_isa"])
        sections.append(table(["Parser compiler setting", "Workers", "Median s", "Range s", "Read/decode median s", "Merge median s", "CPU seconds"], [[row["isa"], row["workers"],
            f'{row["wall_ms"]["median"] / 1000:.3f}', f'{row["wall_ms"]["min"] / 1000:.3f}–{row["wall_ms"]["max"] / 1000:.3f}',
            f'{row["scan_ms"]["median"] / 1000:.3f}', f'{row["merge_ms"]["median"] / 1000:.3f}', f'{row["cpu_ms"]["median"] / 1000:.2f}'] for row in comparison["groups"]]))
        sections.append(f'<p><strong>No established AVX2 compiler gain at 16 workers.</strong> The median changes from {sse16:.3f} s to {avx16:.3f} s, only {(1 - avx16 / sse16) * 100:.2f}% ({(sse16 - avx16) * 1000:.1f} ms). The individual ranges overlap widely. At one worker the AVX2 median is lower, but the ranges also overlap. These 12 runs use the same parser and rotating order. All ordered fingerprints match, including full double positions/UVs/normals, face indices, and face counts; this control does not validate every OBJ grammar or material field.</p>')
        sections.append('<p>The comparison changes the parser/harness translation-unit compiler setting. Common linked core code and the STL/runtime helpers remain available in both builds. Separate CPU-progress-filtered context captures confirm AVX2 byte-search and 256-bit copy instructions even in the SSE2-targeted variant. Thus it is not an all-code SSE2 deployment test or a pure “SIMD off versus on” test. <a href="https://learn.microsoft.com/en-us/cpp/standard-library/vectorized-stl-algorithms?view=msvc-170">Microsoft: vectorized STL algorithms</a> describes the runtime choice of supported vector instructions.</p>')
        sections.append(table(["Next parser trial", "Reason", "Required check"], [
            ["Avoid the failed position-only face probe for common slash references", "The sampled v/vt format falls back after its first index has already been decoded; the probe costs 5.4% of sampled read/decode worker cycles.", "Preserve comments, whitespace, positive/negative indices, invalid-input errors, and the position-only fast path."],
            ["Add a validated common v/vt triangle path", "General face parsing is the largest worker hotspot. Reduce repeated format checks before considering wider arithmetic.", "Keep the general fallback for normals, negative indices, larger polygons, continuations, and freeform statements."],
            ["Trial packed index validation/offset application", "The merge-validation loop is scalar and regular. Specialize the common positive/no-offset case; measure buffer traffic and fallback cost.", "Match ordered index triples, range checks, error behavior, and deterministic output; compare whole parse time and CPU seconds."],
            ["Compare combined newline/slash/space classification", "Byte find already uses AVX2 and is about 4.1% of sampled worker cycles. A replacement needs to remove work beyond the existing finder.", "Preserve chunk-boundary handling and cancellation; include masks, temporary storage, and the general decoder in the timing."],
            ["Batch numeric conversion only after grammar/layout measurements", "Variable field widths, signs, decimal points, exponents, and rounding complicate a wide decoder. Existing SWAR already handles eligible digit runs.", "Retain double precision and exact rounding/error checks. Use a tested scalar fallback; do not assume RGB values can be skipped without validation."]]))
        sections.append('<details><summary>Inspect the instruction addresses and compiler listings</summary><p><a href="parser-simd.json">Parser SIMD evidence JSON</a> retains every scoped native sample location, decoded opcode, module RVA, function name, compiler-function inventory, binary hashes, and the separate context captures. <a href="parser-isa.csv">Compiler comparison CSV</a> contains the 12 timed runs. Preserved EXE/PDB pairs and full assembly listings are under <code>build/beartrap-loading-audit-20261014/parser-simd</code>. Runtime symbols can be unresolved or nearest exports; use the module offset and instruction instead of treating the displayed export as the function identity.</p></details>')
    if "parser_microarchitecture" in data:
        micro = data["parser_microarchitecture"]
        branch = micro_group(data, "branch", "read_decode_workers")
        cache = micro_group(data, "data", "read_decode_workers")
        merge_cache = micro_group(data, "data", "merge_all")
        whole_branch = micro_group(data, "branch", "parser_total")
        pressure = micro_group(data, "backend", "read_decode_workers")
        merge_pressure = micro_group(data, "backend", "merge_all")
        traffic = micro["traffic"]
        sections.append('<h3 id="parser-deep">5b. Branches, cache, translation, and shared cores</h3><p class="meta">Extended 15 October 2026 · 17 validated parser-only hardware captures · 12 fresh affinity controls · one buffer-growth/range audit</p>')
        sections.append(f'<p><strong>Prediction is accurate on this model. The next parser work should reduce instruction volume, repeated decoding, buffer copies, and merge writes.</strong> Read/decode workers execute about {branch["metrics"]["branches_per_kinst"]:.0f} branches per 1,000 retired instructions, with an estimated {branch["metrics"]["branch_miss_percent"]:.4f}% branch miss ratio. The complete parser averages about {whole_branch["counts"]["0c0:00"]["estimated_events"] / whole_branch["captures"] / traffic["input_bytes"]:.1f} retired instructions per input byte. These are sampled estimates, not exact dynamic instruction counts.</p>')
        sections.append(figures["parser_branches"])
        branch_rows = []
        for row in branch["functions"][:10]:
            values = row["metrics"]; support = row["counts"]["0c3:00"]["samples"]
            branch_rows.append([parser_function_label(row), f'{values["ipc"]:.2f}', f'{values["branches_per_kinst"]:.1f}',
                f'{values["branch_misses_per_kinst"]:.4f}', f'{values["branch_miss_percent"]:.4f}%', f'{support:.0f}' + (" (sparse)" if support < 30 else "")])
        sections.append(table(["Associated function", "Estimated IPC", "Branches / 1k retired", "Misses / 1k retired", "Miss ratio", "Miss sample support, 3 captures"], branch_rows))
        spread = branch["per_capture_metrics"]["branch_miss_percent"]
        sections.append(f'<p>The read/decode miss ratio ranges from {spread["min"]:.4f}% to {spread["max"]:.4f}% across the three branch captures. Direction-mismatch and indirect-target events are {branch["metrics"]["direction_misses_per_kinst"]:.4f} and {branch["metrics"]["indirect_misses_per_kinst"]:.5f} per 1,000 retired instructions. These event populations and per-function attribution are imperfect; do not add them into an exact classification of every miss. The source contains many predictable digit, separator, sign, bounds, and error checks. Removing redundant checks can reduce work even when prediction succeeds.</p>')
        sections.append('<div class="explorer"><h4>Explore an assumed branch recovery cost</h4><p>This calculation assigns the same penalty to every reported miss and compares its nominal sum with the sampled user-mode cycle count. Actual recovery latency and overlap are unmeasured; this is not a wall-time prediction.</p><label for="branch-penalty">Assumed cycles per miss <output id="branch-penalty-value">20</output></label><input id="branch-penalty" type="range" min="0" max="100" value="20"><div id="branch-budget" aria-live="polite"></div></div>')
        sections.append('<p>The position-only face probe still decodes an index before it falls back for the sampled slash format. Its source and sampled CPU cost give a specific redundant-work target. A broad branchless rewrite would need evidence that its added loads or calculations cost less than the existing predictable paths.</p>')
        sections.append('<p>A positive <code>v/vt</code> triangle needs six integer conversions. The failed position-only probe adds a seventh by decoding the first position again. The general path also checks the optional normal and index formats for each corner. A measured common-format decoder can remove that repeated work while preserving the general fallback.</p>')

        sections.append('<h4>Data caches and the merge</h4>' + figures["parser_cache"] + figures["parser_pressure"])
        cache_rows = []
        for scope, caption in (("read_decode_workers", "Read/decode workers"), ("merge_all", "Merge")):
            c = micro_group(data, "data", scope)["metrics"]
            l2 = micro_group(data, "l2", scope)["metrics"]
            back = micro_group(data, "backend", scope)["metrics"]
            cache_rows.append([caption, f'{c["ipc"]:.2f}', f'{c["demand_fills_per_1000_ls_dispatch"]:.3f}',
                f'{c["demand_dram_io_per_kinst"]:.4f}', f'{l2["selected_l2_data_miss_percent"]:.2f}%',
                f'{back["store_queue_stall_cycle_percent"]:.2f}%', f'{back["load_queue_stall_cycle_percent"]:.2f}%'])
        sections.append(table(["Scope", "Estimated IPC", "Demand fills / 1k LS dispatch", "DRAM/IO demand fills / 1k retired", "Selected L2 data miss outcomes", "Store queue event / cycles", "Load queue event / cycles"], cache_rows))
        sections.append(f'<p>The merge has about {merge_cache["metrics"]["demand_dram_io_per_kinst"] / cache["metrics"]["demand_dram_io_per_kinst"]:.1f} times as many local DRAM/IO demand refills per retired instruction as read/decode. Its store queue event ratio is {merge_pressure["metrics"]["store_queue_stall_cycle_percent"]:.1f}% versus {pressure["metrics"]["store_queue_stall_cycle_percent"]:.1f}%. This supports investigating output writes, index reconciliation, and copies. It does not establish physical DRAM bandwidth saturation, or a complete percentage of time waiting for memory.</p>')
        sections.append('<p>Local L2 supplies most selected demand refills during read/decode. The “L3 / peer L2” source combines two cache paths, so it cannot provide a separate L3 hit rate. Demand refills omit prefetch traffic and many stores. LS dispatch counts operations, not unique memory accesses. The selected L2 ratio uses data-request hit masks <code>0xF0</code> and miss mask <code>0x08</code> on event <code>0x64</code>; it is a request-outcome ratio, not a universal miss rate for every cache access.</p>')
        sections.append(table(["Associated function", "Local L2 fills / 1k retired", "L3 / peer L2 fills / 1k retired", "Local DRAM/IO fills / 1k retired"], [[parser_function_label(row),
            f'{row["metrics"]["demand_local_l2_per_kinst"]:.3f}', f'{row["metrics"]["demand_l3_peer_l2_per_kinst"]:.3f}', f'{row["metrics"]["demand_dram_io_per_kinst"]:.3f}']
            for row in cache["functions"][:10]]))
        sections.append('<p>Runtime memory routines have much higher refill density per retired instruction than the main decoders. The independent growth audit identifies actual copy payload in those paths. A low refill count in number parsing therefore does not imply the whole parser has little memory traffic. Function ratios are affected by sampling skid and instruction-attribution bias.</p>')

        sections.append('<h4>Translation, alignment, and instruction supply</h4>')
        translation_rows = []
        for scope, caption in (("read_decode_workers", "Read/decode workers"), ("merge_all", "Merge")):
            tlb = micro_group(data, "tlb", scope)["metrics"]
            front = micro_group(data, "frontend", scope)["metrics"]
            translation_rows.append([caption, f'{tlb["dtlb_refills_per_kinst"]:.4f}', f'{tlb["dtlb_refill_l2_miss_percent"]:.1f}%',
                f'{tlb["dtlb_l2_misses_per_kinst"]:.4f}', f'{tlb["split_cacheline_loads_per_kinst"]:.3f}', f'{tlb["split_page_loads_per_kinst"]:.3f}',
                f'{front["icache_l2_refills_per_kinst"] + front["icache_system_refills_per_kinst"]:.4f}', f'{front["op_queue_empty_cycle_percent"]:.2f}%'])
        sections.append(table(["Scope", "L1 DTLB refills / 1k retired", "L2 miss share of those refills", "L2 DTLB misses / 1k retired", "64-byte crossing load events / 1k", "4 KiB crossing load events / 1k", "Instruction-cache refills / 1k", "Op queue empty / cycles"], translation_rows))
        sections.append('<p>The large L2 TLB miss percentage uses the small population of L1 TLB refills as its denominator. It is not the percentage of all loads that need a page-table walk. Merge has more translation and crossing-load events per instruction. These counts do not measure their latency penalty. Large pages and stronger alignment are candidate experiments after measuring that penalty; no privilege, page-size, or allocator policy was changed.</p>')
        sections.append('<p>Instruction-cache refill events include lines supplied by L2 and by the system/other caches. Op queue empty events are about 4.6% of read/decode non-halted cycles in this capture. The selected frontend counters do not identify a large instruction-fetch limit. Queue and dispatch conditions overlap, and an empty queue can coexist with other constraints. These counters cannot provide a complete top-down breakdown of every stalled cycle.</p>')

        sections.append('<h4>Where the memory traffic comes from</h4>' + figures["parser_growth"])
        sections.append(table(["Buffer family", "Growth allocations", "Cumulative allocated GB", "Copy payload GB", "Largest single allocation MiB"], [[row["kind"], row["growths"],
            f'{row["allocated_bytes"] / 1e9:.3f}', f'{row["copy_payload_bytes"] / 1e9:.3f}', f'{row["largest_allocation_bytes"] / 1024**2:.1f}']
            for row in micro["allocation"]["allocation_audit"]["buffers"] if row["growths"]]))
        sections.append(f'<p><strong>Buffer growth copies {traffic["buffer_growth_copy_payload_bytes"] / 1e9:.2f} GB of payload.</strong> The parser emits {traffic["temporary_logical_bytes"] / 1e9:.2f} GB of logical temporary geometry and {traffic["output_logical_bytes"] / 1e9:.2f} GB of final parser arrays. Its growable buffers repeatedly allocate larger storage and copy the old elements. Index triplets account for most growth-copy payload. The {traffic["cumulative_buffer_allocated_bytes"] / 1e9:.2f} GB allocation total is cumulative; it is not peak live memory.</p>')
        sections.append(f'<p>A minimum logical traffic budget is about {traffic["minimum_logical_read_write_bytes"] / 1e9:.2f} GB: one input read, temporary writes, growth-copy reads and writes, and final merge reads/writes. It excludes extra token scans, allocator metadata, UV zero initialization, file-cache copying in Windows, and other work. Cache reuse changes physical transfers, so this number is not DRAM bytes or a measured bandwidth requirement. Only the growable <code>Buffer</code> allocations are counted directly.</p>')
        sections.append('<p>The diagnostic parser output matches the baseline ordered fingerprint. Counts execute only on growth operations and range completion. Its timing is excluded from performance medians. The byte-buffer category combines offset flags and face counts because both use the same underlying byte type.</p>')
        constant_fields = micro["constant_parser_fields_bytes"]
        index_copies = next(row["copy_payload_bytes"] for row in micro["allocation"]["allocation_audit"]["buffers"] if row["kind"] == "index-triplets")
        sections.append(table(["Uniform parser field on this model", "Explicit storage in final parser arrays", "Evidence"], [
            ["Missing normal IDs (-1 inside each index triplet)", f'{constant_fields["missing_normal_ids"] / 1e9:.3f} GB', "No normal records, a successful validated parse, and normal-index bounds checks imply every normal ID is missing."],
            ["One material ID for every face", f'{constant_fields["uniform_material_ids"] / 1e9:.3f} GB', "Whole-file metadata scan finds one usemtl command, at byte 45, before the first face; no backslash ambiguity."],
            ["Smoothing ID zero for every face", f'{constant_fields["zero_smoothing_ids"] / 1e9:.3f} GB', "Whole-file whitespace-aware scan finds no s command; the parser initializes smoothing IDs to zero."]]))
        sections.append(f'<p><strong>About {sum(constant_fields.values()) / 1e9:.2f} GB ({sum(constant_fields.values()) / traffic["output_logical_bytes"] * 100:.1f}%) of final parser storage holds uniform fields.</strong> The temporary index triplets also carry the missing normal IDs. One third of their {index_copies / 1e9:.2f} GB growth-copy payload represents that constant field. A compact index pair or an implicit-field view could reduce storage and writes, but must preserve the generic parser API, normal-bearing models, material changes, smoothing changes, and all logical values. This is a representation opportunity, not a measured production speedup.</p>')

        sections.append('<h4>Range balance and core placement</h4>' + figures["parser_ranges"] + figures["parser_affinity"])
        ranges = micro["allocation"]["allocation_audit"]["ranges"]
        walls = [row["wall_ms"] for row in ranges]
        sections.append(f'<p>The 16 ranges are about 444 MB each, but contain different mixes of positions, UVs, and faces. In the diagnostic, range wall time spans {min(walls) / 1000:.3f}–{max(walls) / 1000:.3f} s; the longest is {max(walls) / statistics.mean(walls):.2f} times the mean. This supports testing a bounded persistent worker pool with smaller tasks that balance decode cost. Its extra scheduling, boundary reconstruction, and allocation costs must be included. The wall/thread-CPU gap also includes overlapped IO and scheduling.</p>')
        sections.append(table(["Windows-reported cache", "Capacity", "Sharing"], [["L1 data", "32 KiB per physical core, 8-way", "Two SMT logical processors"],
            ["L1 instructions", "32 KiB per physical core, 8-way", "Two SMT logical processors"], ["L2", "512 KiB per physical core, 8-way", "Two SMT logical processors"],
            ["L3", "16 MiB, 16-way", "All 8 physical cores / 16 logical processors"], ["Cache line", "64 bytes", "All reported cache levels"]]))
        sections.append('<p>Each worker owns two 256 KiB native input buffers, plus edge storage and growing output buffers. Two workers on one physical core therefore allocate more input-buffer storage than its 512 KiB L2 capacity. Allocated storage is not the same as simultaneously resident cache lines. The measured eight-worker paired-SMT control does not show a higher demand-refill rate than one worker per physical core; cache-capacity arithmetic alone does not establish thrashing.</p>')
        sections.append(table(["Unprofiled placement control", "Median parser s", "Range s", "CPU seconds", "Allowed logical CPUs"], [[row["name"],
            f'{row["wall_ms"]["median"] / 1000:.3f}', f'{row["wall_ms"]["min"] / 1000:.3f}–{row["wall_ms"]["max"] / 1000:.3f}',
            f'{row["cpu_ms"] / 1000:.2f}', ','.join(str(cpu) for cpu in row["affinity"])] for row in micro["affinity"]]))
        timing_by_name = {row["name"]: row for row in micro["affinity"]}
        eight = timing_by_name["w8-physical"]; sixteen = timing_by_name["w16-all"]
        sections.append(f'<p>Sixteen workers use {(sixteen["cpu_ms"] / eight["cpu_ms"] - 1) * 100:.1f}% more process CPU time than eight pinned to separate physical cores, with a {(1 - sixteen["wall_ms"]["median"] / eight["wall_ms"]["median"]) * 100:.1f}% lower median wall time in this campaign. Their ranges overlap. Eight workers allowed on all CPUs have nearly the same median wall time as sixteen here. Eight workers confined to four physical cores with SMT are clearly slower. Placement, partitioning, and measurement variance matter; this campaign does not establish a universal change to the worker default.</p>')
        placement_rows = []
        for workers, placement in ((8, "physical"), (8, "four-core-smt"), (16, "all")):
            b = micro_group(data, "branch", "read_decode_workers", workers, placement)
            c = micro_group(data, "data", "read_decode_workers", workers, placement)
            placement_rows.append([f'{workers} / {placement}', f'{b["metrics"]["ipc"]:.2f}', f'{b["metrics"]["branch_miss_percent"]:.4f}%',
                f'{c["metrics"]["demand_fills_per_1000_ls_dispatch"]:.3f}', f'{c["metrics"]["demand_dram_io_per_kinst"]:.4f}'])
        sections.append(table(["Workers / placement", "Branch-capture IPC", "Branch miss ratio", "Data-capture fills / 1k LS dispatch", "DRAM/IO fills / 1k retired"], placement_rows))
        sections.append('<p>The eight-worker placement controls hold worker count and range partitioning constant. Each placement counter set has one capture; IPC differs across separately collected event sets, so small differences are uncertain. Native sample core IDs verify the requested affinity. The profiler resets its launched child affinity by default; the accepted pinned captures use its explicit <code>--affinity</code> option. Captures that failed affinity validation, or overlapped a build, are excluded. <a href="https://docs.amd.com/r/en-US/57368-uProf-user-guide/9.5.3.-Options">AMD: collection and affinity options</a>.</p>')

        sections.append('<h4>Parser trials supported by these measurements</h4>' + table(["Trial", "Measured reason", "Acceptance test"], [
            ["Avoid repeated face-index decoding; specialize common v/vt triangles", "Accurate prediction plus high instruction volume; the failed simple-face probe remains measurable.", "Full precision, ordered output, error lines, negative references, comments, continuation, and general fallback."],
            ["Reduce geometric buffer growth copies", "6.32 GB growth-copy payload; 3.73 GB in index-triplet buffers.", "Compare a sizing pass, bounded slabs, or informed reservation. Include the extra pass, metadata, peak memory, and final merge."],
            ["Use compact or implicit values for uniform parser fields", "1.52 GB of final parser storage holds missing normal IDs, one material ID, and zero smoothing IDs on this model.", "Preserve generic models, indexing, metadata access, exact output values, and the downstream loader contract. Compare complete parse time and peak memory."],
            ["Batch index reconciliation and improve output writes", "Scalar merge loop, low estimated IPC, larger DRAM/IO refill density, and store queue pressure.", "Preserve offsets, bounds checks, ordering, and diagnostics; measure complete parse wall time and CPU seconds."],
            ["Test a persistent bounded pool with balanced tasks", "Equal byte ranges have unequal record mixes and elapsed times; small async blocks were slower in the earlier controls.", "Measure task/boundary overhead, resource cost, cancellation, and stable collection order."],
            ["Keep TLB, alignment, and branchless changes as measured follow-ups", "Event counts locate possible costs but do not measure their latency; branch misses are already rare.", "Use precise load-latency/branch-site evidence when available, or isolated counterfactual kernels. Avoid changing memory policy from ratios alone."]]))
        sections.append('<details><summary>Measurement scope and remaining gaps</summary><ul>' + ''.join('<li>' + html.escape(item) + '</li>' for item in micro["limits"]) + '</ul><p>Three branch and two data-cache captures are pooled at 16 workers. Other counter sets use one capture per placement. Each set has six events and is collected sequentially. QPC metadata aligns scope windows; per-event sensitivity to a +16 ms shift is saved. The short allocation phase has greater boundary uncertainty. Actual accepted event definitions, intervals, native module offsets, and matching executable hashes are retained.</p><p>IBS collection returned “IBS counters are not available.” No virtualization or security settings were changed. Precise load latency, a cache-miss stall-time breakdown, physical DRAM-controller bandwidth, and false-sharing attribution remain unmeasured. <a href="https://docs.amd.com/r/en-US/57368-uProf-user-guide/IBS-Derived-Events">AMD: IBS derived events</a> describes the additional evidence that IBS can provide.</p><p><a href="parser-microarchitecture.json">Deep parser evidence JSON</a> · <a href="parser-hardware.csv">Hardware scope CSV</a> · <a href="parser-affinity.csv">Unprofiled affinity CSV</a> · <a href="parser-buffer-ranges.csv">Range allocation CSV</a>. Native databases, preserved binaries/PDBs, and rejected-attempt logs remain under <code>build/beartrap-loading-audit-20261014/parser-microarchitecture</code>.</p></details>')
    sections.append('</section>')

    sections.append('<section id="replays"><h2>6. The long annotation pass: validated replays</h2>' + figures.get("kernels", ""))
    sections.append(f'<p>The pass visits all 227,315,958 triangle corners. For each corner it hashes the vertex ID and three render-coordinate bit patterns, reads precise coordinates, and updates float block bounds and double group bounds. There is one model group, so simply parallelizing groups gives no benefit. The current pass calls hashing, generic position, and finite-coordinate helpers in the inner loop.</p><p>In the isolated replay, replacing helper calls with the same inline hash, direct OBJ source-coordinate access, and inline finite checks reduces the combined pass from {base_replay:.3f} s to {best_replay:.3f} s. This is a measured kernel result with matching outputs. It is not a measured end-to-end production speedup.</p>')
    sections.append('<p>The direct path needs validated immutable OBJ geometry and the correct source-position map. A production change must keep generic storage paths and cancellation behavior. The ordered fingerprint is part of saved annotation identity; a different parallel hash cannot silently replace it. Bounds can be parallelized independently, but compare extra passes, temporary memory, and overlap with point preparation.</p></section>')
    sections[-1] = sections[-1].replace('</section>', '') + f'<p>Inlining the hash alone changes the combined median from {base_replay:.3f} s to {data["kernel_summary"]["combined_inline_hash"]["median"] / 1000:.3f} s. Most of the combined gain comes from coordinate access and finite checks. The direct bounds-only pass takes {data["kernel_summary"]["bounds_direct_only"]["median"] / 1000:.3f} s on one thread and {data["kernel_summary"]["bounds_direct_parallel"]["median"] / 1000:.3f} s on eight threads. The ordered hash-only pass still takes {data["kernel_summary"]["hash_inline_only"]["median"] / 1000:.3f} s.</p></section>'

    sections.append('<section id="next"><h2>7. Next steps in order of expected value</h2>')
    sections.append(table(["Priority", "Action", "Evidence / acceptance check"], [[1, "Optimize annotation coordinate access and finite checks", "Select coordinate storage once; inline equivalent finite checks and hashing; preserve cached bounds and saved fingerprints. Validate a full load to Ready in matched runs."],
        [2, "Consider separating parallel bounds from ordered fingerprints", "The bounds probes show available parallelism. Compare total work and the Ready join, not only a bounds timer."],
        [3, "Trial fast_float 8.3.2 in the production loader", f'About {current_parser - modern_parser:.2f} s parser gain here. Require full-precision output, error-line, continuation, materials, freeform, and stress checks.'],
        [4, "Reduce parser peak memory / task overhead", "1–2 MiB blocks are slower and use more memory. Compare bounded pools and output allocation before changing defaults."],
        [5, "Evaluate a validated binary import/derived-data cache for repeat loads", "Can bypass repeated decode, ID mapping, normals, and prepared-data work. Include cache creation, input identity checks, invalidation, and GPU upload in its total cost."],
        [6, "Measure normals and ID resolution changes only after higher-value work", "These serial passes cost about a second each. Stable IDs, shared vertex writes, and deterministic reductions constrain parallelism."]]))
    sections.append('<div class="explorer"><h3>Explore a wall-time bound</h3><p>This model holds other stages constant and applies gains to the parser and the annotation block pass. The point branch sets the minimum post-load join time. It is a prediction, not a benchmark.</p><label for="parser-gain">Parser time reduction <output id="parser-gain-value">0%</output></label><input id="parser-gain" type="range" min="0" max="100" value="0"><label for="annotation-gain">Annotation block time reduction <output id="annotation-gain-value">0%</output></label><input id="annotation-gain" type="range" min="0" max="100" value="0"><div id="prediction" aria-live="polite"></div></div>')
    sections.append('<p>Do not prioritize more parser threads, larger staging buffers, or a blanket ISA flag. The measurements already show their limits. The largest remaining stages need fewer passes, less helper-call work, or a change in their dependency structure.</p></section>')

    sections.append('<section id="evidence"><h2>8. Evidence and reproducibility</h2>')
    sections.append(table(["Item", "Captured value"], [["Model", data["provenance"]["model"]], ["Model bytes", data["provenance"]["model_bytes"]], ["SHA-256", data["provenance"]["model_sha256"]],
        ["Revision", data["provenance"]["revision"]], ["CPU", data["cpu"]["brand"].strip() + "; 8 cores / 16 logical processors"], ["RAM", f'{data["provenance"]["ram_bytes"]:,} bytes'],
        ["GPU", "NVIDIA GeForce RTX 3070 Laptop GPU; 8 GiB; driver 616.92"], ["OS", data["provenance"]["os"]], ["Representative visible run", rep["name"]],
        ["Native full loads", data["validation"]["native_runs"]], ["Parser samples", data["validation"]["parser_runs"]], ["Read/copy samples", data["validation"]["control_runs"]],
        ["CPU diagnostics", "AMD uProf branch/cache/FP sets and independent VTune software hotspots"], ["GPU diagnostics", "Nsight Systems NVTX and WDDM capture; Windows per-process engine/memory counters"],
        ["Application change", "Production behavior is unchanged. Instrumentation and trial kernels are opt-in research targets."]]))
    sections.append('<p>The final Release build has no warnings. All 121 unit tests passed, including the slow and graphics groups. The original default selection also passed 14 additional research/control tests: 135 distinct checks in total. Parser input, Ready-toast, replay, accounting, capture-provenance, SIMD, allocation-output, and hardware-accounting checks are included. Logs and browser checks are recorded in <a href="validation.txt">validation.txt</a>. No tests were added for report text.</p>')
    sections.append('<p>Native and profiled timings are kept separate. Warm-cache loading is the primary scope; cold-cache and monitor scan-out latency remain unmeasured. Single diagnostic captures cannot establish small hardware-counter differences. No exact dynamic opcode counts, GPU shader occupancy, or physical DRAM-controller bandwidth are claimed.</p>')
    sections.append('<p>Download <a href="summary.json">the numerical summary</a>, <a href="phases.csv">phase CSV</a>, <a href="parser.csv">parser CSV</a>, <a href="measurements.zip">measurement records</a>, and <a href="REPRODUCE.txt">commands and paths</a>. Matching executables and symbols, native profiler databases, full logs, frame records, and screenshots remain under the recorded build output folder.</p></section>')

    frontend = {"ready": rep["endpoints_ms"]["ready_toast_drawn"] / 1000, "parser": phases_by_name["parser_total"]["wall_ms"] / 1000,
        "annotation": annotation, "blocks": blocks, "model": model, "phases": [{"name": label(r["name"]), "start": r["start_ms"] / 1000, "end": r["end_ms"] / 1000, "wall": r["wall_ms"] / 1000} for r in rep["phase_rows"]],
        "cpu": rep["series"]["cpu"], "memory": rep["series"]["memory"]}
    if "parser_microarchitecture" in data:
        branch = micro_group(data, "branch", "parser_total")
        frontend["branch"] = {"misses": branch["counts"]["0c3:00"]["estimated_events"] / branch["captures"],
                              "cycles": branch["counts"]["076:00"]["estimated_events"] / branch["captures"]}
    css = '''body{margin:0;background:#f5f4ef;color:#23313a;font:16px/1.6 system-ui,-apple-system,Segoe UI,sans-serif}main{max-width:1140px;margin:auto;padding:40px 32px 70px}h1{font-size:clamp(38px,5.5vw,66px);line-height:1.08;letter-spacing:-2px;max-width:980px;margin:12px 0 22px}h2{font-size:29px;line-height:1.25;margin:44px 0 22px}h3{font-size:20px;line-height:1.3}.eyebrow{color:#1f7283;text-transform:uppercase;letter-spacing:2px;font-size:12px;font-weight:750}.lede{font-size:21px;max-width:900px}.meta{font-size:14px;color:#53646d}nav{display:flex;gap:16px;flex-wrap:wrap;padding:18px 0;margin:20px 0;border-block:1px solid #d4d9d6}a{color:#176b7b;text-underline-offset:3px}.kpis{display:grid;grid-template-columns:repeat(4,1fr);gap:20px;margin:28px 0}.kpis>div{background:#fff;padding:22px}.kpis strong{display:block;font-size:32px;color:#1f7283}.kpis span{display:block;font-size:13px;line-height:1.4;margin-top:8px}aside{padding:22px 26px;background:#e5ece9;border-left:4px solid #1f7283;margin:25px 0}section{scroll-margin-top:20px}figure{margin:28px 0;background:#fff;padding:20px 22px}figure svg{display:block;width:100%;height:auto;max-height:880px}figcaption{font-size:12px;color:#52646d;margin-top:15px}.table-wrap{overflow:auto;margin:22px 0;background:#fff}table{border-collapse:collapse;width:100%;font-size:13px;text-align:left}th{background:#e6ece9;font-weight:700;white-space:nowrap}td,th{padding:11px 13px;border-bottom:1px solid #e2e7e4;vertical-align:top}td:first-child{font-weight:600}details{padding:16px 20px;background:#edf0eb;margin:20px 0}summary{font-weight:650;cursor:pointer}code{font-size:.88em;overflow-wrap:anywhere}.explorer{padding:22px 26px;background:#fff;margin:25px 0}.explorer label{display:flex;justify-content:space-between;font-size:14px;font-weight:650}.explorer input{width:100%;accent-color:#1f7283;margin:12px 0 22px}.explorer output{color:#1f7283}#time-detail,#prediction{font-size:18px;min-height:62px}footer{font-size:12px;color:#65737b;padding-top:30px}li{margin:10px 0}@media(max-width:760px){main{padding:24px 16px}h1{letter-spacing:-1px}.kpis{grid-template-columns:repeat(2,1fr);gap:12px}.kpis>div{padding:16px}figure{padding:12px 8px}nav{gap:10px;font-size:13px}}'''
    script = '''const DATA=__DATA__;
const byId=id=>document.getElementById(id);
const nearest=(rows,t)=>rows.reduce((best,row)=>Math.abs(row.time_s-t)<Math.abs(best.time_s-t)?row:best,rows[0]);
const time=byId('time');time.max=(DATA.ready+.2).toFixed(1);time.value=0;
function inspect(){const t=Number(time.value);byId('time-value').textContent=t.toFixed(1)+' s';const active=DATA.phases.filter(p=>p.start<=t&&t<p.end).sort((a,b)=>a.wall-b.wall).slice(0,3);const cpu=nearest(DATA.cpu,t);const mem=nearest(DATA.memory,t);byId('time-detail').textContent=(active.length?active.map(p=>p.name).join(' · '):t>=DATA.ready?'Ready':'Request / handoff')+' — nearest samples: '+cpu.logical_cpus.toFixed(1)+' busy logical CPUs; '+(mem.private_bytes/1073741824).toFixed(2)+' GiB private commit.';}
function predict(){const pg=Number(byId('parser-gain').value)/100;const ag=Number(byId('annotation-gain').value)/100;byId('parser-gain-value').textContent=Math.round(pg*100)+'%';byId('annotation-gain-value').textContent=Math.round(ag*100)+'%';const newJoin=Math.max(DATA.model,DATA.annotation-DATA.blocks*ag);const saved=DATA.parser*pg+Math.max(DATA.model,DATA.annotation)-newJoin;byId('prediction').textContent='Predicted Ready toast: '+(DATA.ready-saved).toFixed(2)+' s; '+saved.toFixed(2)+' s saved ('+(saved/DATA.ready*100).toFixed(1)+'%).';}
function branchBudget(){const penalty=Number(byId('branch-penalty').value);byId('branch-penalty-value').textContent=penalty;const nominal=DATA.branch.misses*penalty;byId('branch-budget').textContent='Nominal recovery budget: '+(nominal/1e9).toFixed(3)+' billion cycles per parse; '+(nominal/DATA.branch.cycles*100).toFixed(3)+'% of the sampled cycle total.';}
time.addEventListener('input',inspect);byId('parser-gain').addEventListener('input',predict);byId('annotation-gain').addEventListener('input',predict);if(DATA.branch){byId('branch-penalty').addEventListener('input',branchBudget);branchBudget();}inspect();predict();'''.replace("__DATA__", json.dumps(frontend, separators=(",", ":")).replace("<", "\\u003c"))
    document = '<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>BearTrap loading performance · current revision</title><style>' + css + '</style></head><body><main>' + ''.join(sections) + '<footer>Initial campaign 14 October 2026; parser hardware extension 15 October 2026. Decimal GB is 10⁹ bytes; GiB is 2³⁰ bytes. Report assets and controls work offline.</footer></main><script>' + script + '</script></body></html>'
    (output / "report.html").write_text(document, encoding="utf-8")
    (output / "summary.json").write_text(json.dumps(data, indent=2), encoding="utf-8")
    if "parser_simd" in data:
        (output / "parser-simd.json").write_text(json.dumps(data["parser_simd"], indent=2), encoding="utf-8")
        comparison_rows = read(root / "parser-simd/isa-comparison.json")["records"]
        with (output / "parser-isa.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, ["isa", "workers", "round", "wall_ms", "cpu_ms", "average_logical_cpus", "fingerprint"])
            writer.writeheader()
            for row in comparison_rows:
                phase = next(p for p in row["events"] if p["name"] == "parser_total")
                writer.writerow({"isa": row["compiler_isa"], "workers": row["workers_requested"], "round": row["round"],
                    "wall_ms": phase["wall_ms"], "cpu_ms": phase["cpu_ms"], "average_logical_cpus": phase["average_logical_cpus"],
                    "fingerprint": row["geometry"]["fingerprint"]})
    if "parser_microarchitecture" in data:
        micro = data["parser_microarchitecture"]
        (output / "parser-microarchitecture.json").write_text(json.dumps(micro, indent=2), encoding="utf-8")
        hardware_rows = []
        for capture in micro["captures"]:
            for scope in capture["scopes"]:
                row = {"capture": capture["name"], "kind": capture["kind"], "workers": capture["workers"],
                    "placement": capture.get("placement", "physical" if capture["workers"] == 8 else "all"), "scope": scope["name"],
                    "wall_ms": scope["wall_ms"], "cpu_ms": scope["cpu_ms"], "user_ms": scope["user_ms"], "kernel_ms": scope["kernel_ms"]}
                row.update({key: value for key, value in scope["metrics"].items() if not isinstance(value, dict)})
                hardware_rows.append(row)
        affinity_rows = []
        for record in read(root / "parser-microarchitecture/affinity-timings.json")["records"]:
            phase = next(p for p in record["events"] if p["name"] == "parser_total")
            affinity_rows.append({"configuration": record["configuration"], "round": record["round"], "workers": record["workers_requested"],
                "affinity": ','.join(str(cpu) for cpu in record["affinity"]), "wall_ms": phase["wall_ms"], "cpu_ms": phase["cpu_ms"],
                "fingerprint": record["geometry"]["fingerprint"]})
        for filename, rows in (("parser-hardware.csv", hardware_rows), ("parser-affinity.csv", affinity_rows),
                               ("parser-buffer-ranges.csv", micro["allocation"]["allocation_audit"]["ranges"])):
            keys = sorted({key for row in rows for key, value in row.items() if not isinstance(value, (dict, list))})
            with (output / filename).open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, keys, extrasaction="ignore"); writer.writeheader()
                writer.writerows({key: row.get(key) for key in keys} for row in rows)
    for filename, rows in (("phases.csv", rep["phase_rows"]), ("parser.csv", data["parser_records"])):
        keys = sorted({key for row in rows for key, value in row.items() if not isinstance(value, (dict, list))})
        with (output / filename).open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, keys, extrasaction="ignore"); writer.writeheader()
            writer.writerows({key: row.get(key) for key in keys} for row in rows)
    reproduce = f'''Model: {data['provenance']['model']}
Model SHA256: {data['provenance']['model_sha256']}
Base revision: {data['provenance']['revision']}
Capture folder: {root}
Build: cmake --preset vs2026-vcpkg -DWOBY_BUILD_OBJ_PROFILE=ON -DWOBY_BUILD_RAPIDOBJ_PROTOTYPE_BENCHMARK=ON -DWOBY_PARSER_FASTFLOAT_HEADER={root.as_posix()}/fast_float-v8.3.2.h
cmake --build --preset vs2026-vcpkg --config Release
ctest --preset vs2026-vcpkg -C Release
uv run experiments/loading-audit/run.py build/vs2026-vcpkg/bin/Release "{data['provenance']['model']}" --output NEW_UNIQUE_CAPTURE_FOLDER --rounds 3 --fast-float-header "{root / 'fast_float-v8.3.2.h'}"
build/vs2026-vcpkg/bin/Release/woby_loading_audit_kernels.exe "{data['provenance']['model']}" 3 > NEW_UNIQUE_CAPTURE_FOLDER/kernels.json
python experiments/loading-audit/census.py "{data['provenance']['model']}" NEW_UNIQUE_CAPTURE_FOLDER/census.json
uv run experiments/obj-profile/deep_tools.py "{data['provenance']['model']}" --executable build/vs2026-vcpkg/bin/Release/woby_obj_profile_sse2.exe --output NEW_UNIQUE_CAPTURE_FOLDER/calibration --mode flop-control --sets fp
uv run experiments/obj-profile/run.py build/vs2026-vcpkg/bin/Release/woby_obj_profile_sse2.exe "{data['provenance']['model']}" --modes parser --workers 1,16 --rounds 3 --compare-executable build/vs2026-vcpkg/bin/Release/woby_obj_profile_avx2.exe --output NEW_UNIQUE_CAPTURE_FOLDER/parser-simd/isa-comparison.json
Preserve both compiler variants and their main.asm listings before any rebuild. Collect separate --instructions runs into parser-simd/context-samples.json; keep them out of medians.
uv run experiments/loading-audit/parser_simd.py CAPTURE_FOLDER
uv run experiments/loading-audit/micro_run.py PRESERVED_AVX2_PROFILE_EXE "{data['provenance']['model']}" NEW_UNIQUE_CAPTURE_FOLDER/parser-microarchitecture --steps topology,timing,collect
Run woby_parser_allocation_audit.exe prototype "{data['provenance']['model']}" 0 16 separately; save its JSON as parser-microarchitecture/allocation-audit.json and preserve its binary/PDB hash.
python experiments/loading-audit/metadata_scan.py "{data['provenance']['model']}" NEW_UNIQUE_CAPTURE_FOLDER/parser-microarchitecture/metadata-scan.json
uv run experiments/loading-audit/micro_analyze.py CAPTURE_FOLDER
uv run experiments/loading-audit/report.py CAPTURE_FOLDER REPORT_FOLDER
No cache flush or power-plan change is performed. Do not run builds, tests, or competing benchmarks during captures.
The current campaign uses a downloaded research-only fast_float 8.3.2 header. Download it from the official release and preserve its hash.
The report file embeds every SVG plot and all interactive data. Nearby PNG/SVG/CSV/JSON files are optional export/evidence links. Report QA uses the T3 collaborative browser.
'''
    (output / "REPRODUCE.txt").write_text(reproduce, encoding="utf-8")
    shutil.copy2(rep["screenshot"], output / "beartrap.png")
    with zipfile.ZipFile(output / "measurements.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in (".json", ".jsonl", ".csv", ".txt", ".log") and "matching-bin" not in path.parts:
                if path.stat().st_size < 100 * 1024**2:
                    archive.write(path, path.relative_to(root))
    print(json.dumps({"report": str(output / "report.html"), "html_bytes": (output / "report.html").stat().st_size,
                      "ready_rpc_s": ready, "ready_toast_s": toast, "first_frame_s": first, "current_parser_s": current_parser,
                      "modern_parser_s": modern_parser, "annotation_replay_base_s": base_replay, "annotation_replay_direct_s": best_replay}))


if __name__ == "__main__":
    capture = Path(sys.argv[1]).resolve(strict=True)
    destination = Path(sys.argv[2]).resolve()
    make(summarize(capture), capture, destination)
