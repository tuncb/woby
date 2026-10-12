"""Render the second measurement pass from preserved native records."""
from collections import defaultdict
import csv
import html
import json
from pathlib import Path
import re
import shutil
import sqlite3
import statistics
import zipfile
from svg_embed import namespace_svg

import matplotlib.pyplot as plt
import numpy as np

COLORS = ["#216e76", "#ba673e", "#729b6b", "#8066a6", "#b69b4c", "#71849b", "#a55f7b"]
LEAVES = ["parser_scan_collect", "parser_merge_execute", "coordinate_origin", "coordinate_rebase",
          "triangulation", "corner_mapping", "normal_face_accumulation", "normal_vertex_normalize",
          "render_bounds", "original_bounds", "group_render_bounds", "group_original_bounds",
          "gpu_index_validation", "point_membership_build", "point_range_bounds", "point_morton_keys",
          "point_radix_sort", "point_sorted_gather", "point_tree_build"]
LABELS = ["Read + decode", "Merge chunks", "Find origin", "Rebase coordinates", "Triangulation",
          "Corner mapping", "Face normals + accumulate", "Normalize vertex normals", "Render bounds",
          "Original bounds", "Group render bounds", "Group original bounds", "Validate GPU indices",
          "Point membership", "Point range bounds", "Morton keys", "Radix sort", "Gather sorted points", "Point tree"]


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def stage_map(record):
    return {row["name"]: row for row in record["stages"]}


def event_value(record, name, key="wall_ms"):
    return sum(row[key] for row in record["events"] if row["name"] == name)


def ready(record):
    return sum(event_value(record, name) for name in ("cpu_import", "group_state", "scene_cpu_preparation")) / 1000


def table(headers, rows):
    def cells(tag, row):
        return ''.join(f'<{tag}>{html.escape(str(value))}</{tag}>' for value in row)
    return '<table><thead><tr>' + cells('th', headers) + '</tr></thead><tbody>' + ''.join(
        '<tr>' + cells('td', row) + '</tr>' for row in rows) + '</tbody></table>'


def figure(fig, destination, name, caption):
    fig.tight_layout(pad=1.8)
    for suffix in ("svg", "png"):
        fig.savefig(destination / f"{name}.{suffix}", dpi=170, facecolor="white", bbox_inches="tight")
    plt.close(fig)
    svg = (destination / f"{name}.svg").read_text(encoding="utf-8")
    return '<figure>' + namespace_svg('<svg' + svg.split('<svg', 1)[1], name + '-') + f'<figcaption>{caption} '
    # The links are added by chart() to keep every export usable by itself.


def chart(fig, destination, name, caption):
    return figure(fig, destination, name, caption) + f'<a href="{name}.png">PNG</a> · <a href="{name}.svg">SVG</a></figcaption></figure>'


def save_csv(path, rows):
    if not rows:
        return
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def counter(stage, event):
    return stage.get("counts", {}).get(event, {}).get("estimated_events", 0)


def paired_summary(source):
    records = source["records"]
    groups = defaultdict(list)
    for record in records:
        groups[(record["executable"], record["reserve_percent"])].append(record)
    result = []
    for (exe, reserve), runs in groups.items():
        result.append({"executable": exe, "reserve_percent": reserve, "runs": len(runs),
            "ready_median_s": statistics.median(ready(run) for run in runs),
            "ready_min_s": min(ready(run) for run in runs), "ready_max_s": max(ready(run) for run in runs),
            "mapping_median_s": statistics.median(event_value(run, "corner_mapping") / 1000 for run in runs),
            "uv_cache_median_s": statistics.median(event_value(run, "uv_validation_cache") / 1000 for run in runs),
            "mesh_capacity_bytes": runs[0]["geometry"]["retained_buffer_capacity_bytes"],
            "peak_import_GiB": statistics.median(next(row for row in run["events"] if row["name"] == "cpu_import")
                                                  ["process_peak_working_set"] / 2**30 for run in runs),
            "all_geometry_hashes_equal": len({run["geometry"]["geometry_fingerprint"] for run in runs}) == 1,
            "all_normal_hashes_equal": len({run["geometry"]["normal_fingerprint"] for run in runs}) == 1})
    return result


def hardware_section(root, destination, summary):
    profiles = {isa: {kind: load(root / f"uprof-prepare-{isa}-{kind}-analysis.json")
                      for kind in ("branch", "cache", "fp")} for isa in ("sse2", "avx2")}
    stages = {isa: {kind: stage_map(value) for kind, value in sets.items()} for isa, sets in profiles.items()}
    rows = []
    for isa, sets in stages.items():
        for name in LEAVES:
            branch, cache, fp = (sets[kind].get(name, {}) for kind in ("branch", "cache", "fp"))
            rows.append({"isa": isa, "phase": name, "profile_wall_s": branch.get("wall_ms", 0) / 1000,
                "average_logical_cpus": branch.get("average_logical_cpus", 0),
                "estimated_user_cycles": counter(branch, "076:00"), "estimated_retired_instructions": counter(branch, "0c0:00"),
                "ipc_estimate": branch.get("ipc_estimate"), "branch_mpki_estimate": branch.get("branch_mpki_estimate"),
                "branch_mispredict_percent_estimate": branch.get("branch_mispredict_percent_estimate"),
                "local_DRAM_demand_fills_estimate": counter(cache, "043:08"),
                "local_L2_demand_fills_estimate": counter(cache, "043:01"),
                "L3_or_sibling_L2_demand_fills_estimate": counter(cache, "043:02"),
                "L2_DTLB_miss_estimate": counter(branch, "045:f0"),
                "FLOPs_estimate": counter(fp, "003:0f"),
                "DRAM_event_samples": cache.get("counts", {}).get("043:08", {}).get("samples", 0)})
    save_csv(destination / "deep-hardware.csv", rows)
    summary["hardware"] = rows
    fig, axes = plt.subplots(1, 3, figsize=(14, 9), sharey=True)
    y = np.arange(len(LEAVES))
    for i, isa in enumerate(("sse2", "avx2")):
        profile = stages[isa]["branch"]
        for column, (key, scale) in enumerate((("wall_ms", .001), ("ipc_estimate", 1), ("average_logical_cpus", 1))):
            axes[column].barh(y + (i - .5) * .34, [(profile.get(name, {}).get(key) or 0) * scale for name in LEAVES],
                              height=.32, color=COLORS[i], label=isa.upper())
    axes[0].set_yticks(y, LABELS)
    axes[0].invert_yaxis()
    for axis, title in zip(axes, ("Profile wall time (s)", "Estimated user IPC", "Average logical CPUs busy")):
        axis.set_title(title, fontsize=12)
        axis.grid(axis="x", alpha=.17)
        axis.set_axisbelow(True)
    axes[2].set_xlim(0, 16)
    axes[0].legend(loc="lower right")
    fig.suptitle("Where time, instructions and CPU concurrency differ", fontsize=17, y=1.01)
    output = chart(fig, destination, "deep-hardware", "Separate uProf captures. Nested parent phases are omitted. These profiled times are diagnostic, not native benchmark medians.")
    names = ["parser_scan_collect", "corner_mapping", "normal_face_accumulation", "group_original_bounds",
             "point_morton_keys", "point_radix_sort", "point_sorted_gather"]
    labels = [LABELS[LEAVES.index(name)] for name in names]
    fig, axes = plt.subplots(1, 3, figsize=(14, 4.7))
    metrics = [([stages['sse2']['branch'][name]['branch_mpki_estimate'] or 0 for name in names], "Branch misses / 1,000 retired instructions"),
               ([counter(stages['sse2']['cache'][name], '043:08') / max(1, counter(stages['sse2']['cache'][name], '0c0:00')) * 1000 for name in names], "Local DRAM/IO demand fills / 1,000 instructions"),
               ([counter(stages['sse2']['fp'][name], '003:0f') / 1e9 for name in names], "Estimated retired FP operations (billions)")]
    for axis, (values, title) in zip(axes, metrics):
        axis.barh(labels, values, color=COLORS[0])
        axis.invert_yaxis()
        axis.set_title(title, fontsize=10)
        axis.grid(axis="x", alpha=.17)
    output += chart(fig, destination, "deep-counter-pressure", "Demand cache-fill events exclude prefetch and writes. A zero means no samples at the selected interval. The panels use different event-set captures.")
    compact = [[row['phase'], f"{row['profile_wall_s']:.3f}", f"{row['ipc_estimate'] or 0:.2f}",
                f"{row['branch_mispredict_percent_estimate'] or 0:.4f}",
                f"{row['local_DRAM_demand_fills_estimate']/1e6:.3f} / {row['DRAM_event_samples']:g}",
                f"{row['FLOPs_estimate']/1e9:.3f}"] for row in rows if row['isa'] == 'sse2']
    output += '<details><summary>Every measured SSE2 leaf phase: counter values and sample support</summary>' + table(
        ['Phase', 'Profile wall s', 'IPC estimate', 'Branch misses %', 'DRAM demand fills M / samples', 'FLOPs B'], compact) + '</details>'
    corner = stages['sse2']['branch']['corner_mapping']
    corners = profiles['sse2']['branch']['probe']['geometry']['triangles'] * 3
    bounds = stages['sse2']['branch']['group_original_bounds']
    avx_bounds = stages['avx2']['branch']['group_original_bounds']
    control = load(root / 'uprof-flop-control-sse2-fp-analysis.json')
    expected = control['probe']['stats']['expected_flops']
    measured = control['counts']['003:0f']['estimated_events']
    summary['fp_calibration'] = {'expected_flops': expected, 'estimated_flops': measured,
                                  'error_percent': (measured / expected - 1) * 100}
    output += f'''<p>Corner mapping retires about <strong>{counter(corner, '0c0:00')/corners:.1f} instructions per corner</strong> in the SSE2 branch capture. Its estimated branch miss rate is {corner['branch_mispredict_percent_estimate']:.3f}%, or {corner['branch_mpki_estimate']:.2f} misses per 1,000 instructions. Branch prediction is effective here. The secondary seam hash also covers only 0.58% of corners. These findings focus attention on repeated attribute access and instruction count.</p>
<p>Group original bounds has an IPC estimate of {bounds['ipc_estimate']:.2f} with SSE2 and {avx_bounds['ipc_estimate']:.2f} with AVX2. Retired instruction estimates fall from {counter(bounds, '0c0:00')/1e9:.2f} B to {counter(avx_bounds, '0c0:00')/1e9:.2f} B. This supports a compiler/code-path explanation for the large bounds gain. It does not show that 256-bit arithmetic processes eight vertices at a time.</p>
<p>The FP event was checked with a known SSE2 loop: {expected:,} expected FP operations and {measured:,.0f} estimated operations, an error of {(measured/expected-1)*100:+.2f}%. AMD's event requires a merged counter; uProf handles that configuration. This calibration supports the FP estimates but does not turn sampled data into exact totals.</p>'''
    placement = []
    fig, axis = plt.subplots(figsize=(13, 7))
    matrix = np.zeros((len(LEAVES), 16))
    for i, name in enumerate(LEAVES):
        stage = stages['sse2']['branch'].get(name, {})
        for row in stage.get('cycle_samples_by_logical_cpu', []):
            matrix[i, row['id']] = row['percent']
            placement.append({'phase': name, 'logical_cpu': row['id'], 'cycle_samples': row['samples'], 'phase_cycle_share_percent': row['percent']})
    from matplotlib.colors import PowerNorm
    picture = axis.imshow(matrix, aspect='auto', cmap='YlGnBu', norm=PowerNorm(.45, vmin=0, vmax=100))
    axis.set_yticks(range(len(LABELS)), LABELS)
    axis.set_xticks(range(16))
    axis.set_xlabel('Logical processor ID from uProf')
    axis.set_title('Where the phase cycle samples execute')
    bar = fig.colorbar(picture, ax=axis, pad=.02)
    bar.set_label('Share of phase cycle samples (%)')
    bar.set_ticks([0, 2, 5, 10, 25, 50, 100])
    output += chart(fig, destination, 'deep-logical-cpus', 'Each row sums to 100%. Colour shows sample placement, not machine utilization. A serial thread can migrate between processors; placement on several CPUs does not prove parallel execution.')
    save_csv(destination / 'deep-logical-cpus.csv', placement)
    dominant = [[name, f"{max((row['percent'] for row in stages['sse2']['branch'][name].get('cycle_samples_by_thread', [])), default=0):.1f}%"] for name in LEAVES]
    output += '<details><summary>Largest single-thread share of cycle samples per phase</summary>' + table(['Phase', 'Dominant thread share'], dominant) + '</details>'
    sensitivity = [[name, f"{stages['sse2']['branch'][name]['alignment_sensitivity_plus_16ms']['076:00']['change_percent']:+.2f}%",
                    f"{stages['sse2']['branch'][name]['alignment_sensitivity_plus_16ms']['0c0:00']['change_percent']:+.2f}%"] for name in LEAVES]
    output += '<p>The 16 ms sensitivity check changes corner-mapping cycles by 0.04% and group original-bounds retired instructions by 0.15% in this capture. The longer-phase conclusions are stable. Subsecond phases have larger boundary sensitivity.</p>'
    output += '<details><summary>Timestamp sensitivity: shift each phase window forward by 16 ms</summary>' + table(['Phase', 'Cycle estimate change', 'Retired instruction estimate change'], sensitivity) + '</details>'
    summary['logical_cpu_placement'] = placement
    summary['phase_alignment_sensitivity'] = sensitivity
    return output, profiles


def instruction_section(profiles, destination, summary):
    categories = ["integer/other", "control flow", "SSE scalar", "SSE128 packed/move",
                  "AVX scalar", "AVX128 packed/move", "AVX256", "unresolved"]
    names = ["parser_scan_collect", "corner_mapping", "normal_face_accumulation", "group_original_bounds", "point_radix_sort"]
    fig, axes = plt.subplots(1, 2, figsize=(14, 4.8), sharey=True)
    mix_rows = []
    for isa, axis in zip(("sse2", "avx2"), axes):
        phases = stage_map(profiles[isa]['branch'])
        left = np.zeros(len(names))
        for category, color in zip(categories, COLORS + ["#bbbbbb"]):
            values = []
            for name in names:
                mix = phases[name]['instruction_sample_categories']
                value = mix.get(category, 0) / max(1, sum(mix.values())) * 100
                values.append(value)
                mix_rows.append({'isa': isa, 'phase': name, 'category': category, 'samples': mix.get(category, 0), 'percent': value})
            axis.barh(np.arange(len(names)), values, left=left, color=color, label=category)
            left += values
        axis.set_title(isa.upper() + ' sampled instruction locations')
        axis.set_xlim(0, 100)
        axis.set_xlabel('Share of retired-event IP samples (%)')
    axes[0].set_yticks(np.arange(len(names)), [LABELS[LEAVES.index(name)] for name in names])
    axes[0].invert_yaxis()
    axes[1].legend(loc='upper left', bbox_to_anchor=(1.01, 1), fontsize=9)
    save_csv(destination / 'deep-instruction-locations.csv', mix_rows)
    summary['instruction_location_mix'] = mix_rows
    return chart(fig, destination, 'deep-instruction-mix', 'Event-sampled instruction pointers, not exact dynamic opcode counts. Packed/move categories include copies. Samples can skid away from the instruction that caused an event.') + '''<p>AVX2 mainly replaces SSE encodings with VEX scalar or 128-bit instructions in these kernels. Wide AVX is present in memory routines and some generated code. The mapping and sorting work remains dominated by integer operations and control flow. Intel SDE is still absent, so exact per-phase dynamic SSE/AVX instruction counts remain open.</p>'''


def calibration_section(root, destination, summary):
    data = load(root / 'uprof-flop-control-sse2-fp-analysis.json')
    loop = data['control_loop']
    total, arithmetic, expected = loop['retired_samples'], loop['arithmetic_samples'], loop['expected_instructions']
    summary['retired_control'] = {'expected_loop_instructions': expected, 'loop_retired_samples': total,
                                  'estimated_loop_instructions': total * 1_000_000,
                                  'sampled_arithmetic_percent': arithmetic/total*100, 'actual_loop_arithmetic_percent': 4/6*100,
                                  'loop_rva': [hex(loop['start_rva']), hex(loop['end_rva'])], 'loop_instructions': loop['instructions']}
    fig, axis = plt.subplots(figsize=(12, 3))
    values = [4/6*100, arithmetic/total*100]
    labels = ['Known dynamic loop stream', 'Retired-event sampled instruction pointers']
    axis.barh(labels, values, color=COLORS[0], label='Packed FP arithmetic')
    axis.barh(labels, [100-value for value in values], left=values, color=COLORS[1], label='Integer + control')
    axis.set_xlim(0, 100)
    axis.invert_yaxis()
    axis.set_xlabel('Instruction share (%)')
    axis.set_title('Calibration: good event totals do not imply unbiased opcode samples')
    axis.legend(loc='upper center', bbox_to_anchor=(.5, -.28), ncol=2)
    output = chart(fig, destination, 'deep-sample-calibration', 'The loop has two mulpd, two addpd, sub and jne per unrolled iteration. No FP math follows it in this address range. Exact matching executable and loop bytes are preserved.')
    output += f'''<p>The loop executes {expected:,} x86 instructions. Its {total:g} retired-event samples estimate {total*1_000_000:,.0f} instructions, {(total*1_000_000/expected-1)*100:+.2f}% from the known count. Yet {arithmetic/total*100:.1f}% of sampled loop IPs point at FP arithmetic, compared with the known 66.7% dynamic fraction. Sampling period effects and skid can strongly bias opcode-location shares. This is why the ISA plots describe observed locations rather than exact instruction utilization.</p>
<p>The control's loop sample activity spans about 32–484 ms in the native database. The metadata/QPC phase window predicts about 16.5–469.2 ms. That exposes an approximately 16 ms residual origin difference. Whole-loop calibration counts do not require phase-window selection. The phase tables include a +16 ms sensitivity check; long-phase conclusions are more robust than subsecond ratios.</p>'''
    return output


def worker_section(root, destination, summary):
    database = sqlite3.connect(str(root / 'nsys-cpu.sqlite'))
    ranges = database.execute('select text,start,end,globalTid from NVTX_EVENTS where end is not null order by start').fetchall()
    database.close()
    start = min(row[1] for row in ranges)
    workers = [row for row in ranges if row[0] == 'parser_worker_read_decode']
    phases = [row for row in ranges if row[0] in ('parser_scan_collect', 'parser_merge', 'corner_mapping', 'group_state', 'scene_cpu_preparation')]
    fig, (top, bottom) = plt.subplots(2, 1, figsize=(14, 7.5), gridspec_kw={'height_ratios': [3, 1]})
    for index, row in enumerate(workers):
        top.broken_barh([((row[1] - start) / 1e9, (row[2] - row[1]) / 1e9)], (index - .36, .72), facecolors=COLORS[0])
    top.set_yticks(range(len(workers)), [f'Worker {index+1} · thread {row[3] & 0xffffffff}' for index, row in enumerate(workers)], fontsize=8)
    top.invert_yaxis()
    top.set_title('Actual read/decode worker ranges from Nsight Systems NVTX')
    top.set_xlabel('Seconds from CPU import marker')
    top.grid(axis='x', alpha=.2)
    for index, row in enumerate(phases):
        bottom.broken_barh([((row[1] - start) / 1e9, (row[2] - row[1]) / 1e9)], (index - .35, .7), facecolors=COLORS[index % len(COLORS)])
    bottom.set_yticks(range(len(phases)), [row[0].replace('_', ' ') for row in phases], fontsize=8)
    bottom.invert_yaxis()
    bottom.set_xlabel('Seconds from CPU import marker · wider scale')
    bottom.grid(axis='x', alpha=.2)
    summary['nvtx_ranges'] = [{'name': text, 'start_s': (begin-start)/1e9, 'end_s': (end-start)/1e9, 'thread': tid & 0xffffffff}
                              for text, begin, end, tid in ranges]
    save_csv(destination / 'deep-nvtx-ranges.csv', summary['nvtx_ranges'])
    return chart(fig, destination, 'deep-worker-timeline', 'Each bar is an elapsed worker range. It includes decoding, file waits and allocation; it is not a core-running interval.') + '''<p>All 16 read/decode workers overlap. Their completion times are uneven. The subsequent corner map, group bounds and most point preparation execute serially. This timeline was captured with NVTX only; CPU sampling and context switches were disabled because those Nsight modes require more access in this session. The stage CPU counters and uProf samples provide separate evidence of CPU use.</p>'''


def experiments_section(root, destination, summary):
    experiments = [("reservation", "Reserve 2% more source IDs", lambda record: record['reserve_percent'] == 2),
                   ("uv-validation", "Precompute valid-UV flags", lambda record: 'uvcheck' in record['executable']),
                   ("uv-pointer", "Capture the UV data pointer", lambda record: 'uvptr' in record['executable'])]
    summaries, export = {}, []
    fig, axes = plt.subplots(1, len(experiments), figsize=(14, 4.5), sharey=False)
    rows = []
    for axis, (name, label, changed) in zip(axes, experiments):
        data = load(root / f'{name}.json')
        summaries[name] = paired_summary(data)
        baseline = [record for record in data['records'] if not changed(record)]
        variant = [record for record in data['records'] if changed(record)]
        pair = []
        for original, modified in zip(sorted(baseline, key=lambda r: r['round']), sorted(variant, key=lambda r: r['round'])):
            pair.append((ready(original), ready(modified)))
            axis.plot([0, 1], [event_value(original, 'corner_mapping')/1000, event_value(modified, 'corner_mapping')/1000],
                      color=COLORS[original['round'] - 1], marker='o', linewidth=1.3, label=f"Round {original['round']}")
            for record in (original, modified):
                export.append({'experiment': name, 'round': record['round'], 'variant': changed(record),
                               'ready_s': ready(record), 'mapping_s': event_value(record, 'corner_mapping') / 1000,
                               'normal_fingerprint': record['geometry']['normal_fingerprint'],
                               'geometry_fingerprint': record['geometry']['geometry_fingerprint']})
        med_original, med_modified = (statistics.median(value[i] for value in pair) for i in (0, 1))
        rows.append([label, f'{med_original:.3f} → {med_modified:.3f}', f'{(1-med_modified/med_original)*100:+.2f}%',
                     f'{min((1-b/a)*100 for a,b in pair):+.2f} to {max((1-b/a)*100 for a,b in pair):+.2f}%'])
        axis.set_xticks([0, 1], ['Baseline', 'Experiment'])
        axis.set_title(label, fontsize=11)
        axis.set_ylabel('Corner mapping (s)')
        axis.grid(axis='y', alpha=.18)
    axes[-1].legend(fontsize=8)
    output = chart(fig, destination, 'deep-paired-experiments', 'Four rotating paired rounds per experiment, without a profiler. Each coloured line joins a baseline and variant from the same round. Different panels are separate batches.')
    output += table(['Research change', 'CPU-ready median s', 'Median time reduction', 'Paired round time reductions'], rows)
    summary['native_experiments'] = summaries
    summary['native_geometry_identical'] = len({row['geometry_fingerprint'] for row in export}) == 1
    summary['native_normals_identical'] = len({row['normal_fingerprint'] for row in export}) == 1
    save_csv(destination / 'deep-native-pairs.csv', export)
    reservation = summaries['reservation']
    fig, axes = plt.subplots(1, 2, figsize=(12, 3.5))
    for axis, key, title in zip(axes, ('mesh_capacity_bytes', 'peak_import_GiB'), ('Retained mesh buffer capacity (GiB)', 'Peak import working set (GiB)')):
        values = [row[key] / 2**30 if key.endswith('bytes') else row[key] for row in reservation]
        axis.barh(['Baseline', '+2% reserve'], values, color=COLORS[:2])
        axis.invert_yaxis()
        axis.set_title(title)
        for y, value in enumerate(values):
            axis.text(value, y, f'  {value:.3f}', va='center')
        axis.set_xlim(0, max(values) * 1.18)
    output += chart(fig, destination, 'deep-allocation', 'The extra initial reserve avoids a later vector-capacity jump. Populated geometry stays the same. Working set excludes the Windows file cache and GPU allocations.')
    saved = reservation[0]['mesh_capacity_bytes'] - reservation[1]['mesh_capacity_bytes']
    uv = summaries['uv-validation']
    output += f'''<p>The 2% reserve saves <strong>{saved:,} bytes ({saved/2**20:.1f} MiB)</strong> of retained mesh capacity. Its timing gain is small and varies by round. The memory reduction is the stronger result. This is a model-specific estimate: models with more UV/normal seams can need a different reserve strategy.</p>
<p>The valid-UV cache adds one byte per UV, about 38.4 MB, and a {uv[1]['uv_cache_median_s']:.3f} s prepass. All four paired import times are slower. The pointer experiment changes only the getter capture; the paired chart shows its actual effect. A hotspot in sampled data does not guarantee a useful wall-time change.</p>
<p>All three experiments produce identical complete geometry and normal fingerprints on this model. Small fixture tests also cover UV seams, unused invalid UV values, and values that overflow float conversion. These are research targets. The production loader remains unchanged.</p>'''
    original = stage_map(load(root / 'uprof-prepare-sse2-branch-analysis.json'))['corner_mapping']
    modified = stage_map(load(root / 'uprof-prepare-uvptr-branch-analysis.json'))['corner_mapping']
    output += '<p>A separate hardware capture supports the pointer result. Estimated corner-loop retired instructions fall from '
    output += f"{counter(original,'0c0:00')/1e9:.3f} B to {counter(modified,'0c0:00')/1e9:.3f} B; cycles fall from {counter(original,'076:00')/1e9:.3f} B to {counter(modified,'076:00')/1e9:.3f} B. IPC rises from {original['ipc_estimate']:.2f} to {modified['ipc_estimate']:.2f}. DRAM/IO demand-fill estimates stay close: {counter(original,'043:08')/1e6:.3f} M versus {counter(modified,'043:08')/1e6:.3f} M. This supports less instruction/dependency work as the reason for the gain. Separate profiler captures are diagnostic; the native paired runs establish wall time.</p>"
    return output


def replay_section(root, destination, summary):
    records = load(root / 'bounds-replay.json')['records']
    record = records[0]
    replay = record['replay']
    grouped = defaultdict(list)
    for row in replay['bounds']:
        grouped[(row['name'], row['workers'])].append(row['seconds'])
    medians = {key: statistics.median(value) for key, value in grouped.items()}
    summary['bounds_replay'] = [{'case': name, 'workers': workers, 'median_s': value, 'min_s': min(grouped[(name, workers)]),
                                 'max_s': max(grouped[(name, workers)])} for (name, workers), value in medians.items()]
    save_csv(destination / 'deep-bounds-replay.csv', summary['bounds_replay'])
    names = ['replay_original_api', 'replay_unchecked_chain', 'replay_source_indices', 'replay_expanded_indexed', 'replay_unique_vertices']
    labels = ['P[M[I]] with mesh API', 'P[M[I]] direct unchecked', 'P[S] source corner IDs', 'Expanded doubles [I]', 'Expanded doubles: unique scan']
    fig, axes = plt.subplots(1, 2, figsize=(14, 4.7))
    values = [medians[(name, 1)] for name in names]
    axes[0].barh(labels, values, color=COLORS[:5])
    axes[0].invert_yaxis()
    axes[0].set_title('Same exact bounding box · single thread')
    axes[0].set_xlabel('Median seconds, 3 repetitions')
    for y, value in enumerate(values):
        axes[0].text(value, y, f'  {value:.3f}', va='center', fontsize=9)
    axes[0].set_xlim(0, max(values)*1.17)
    for parallel, serial, label, color in [('replay_chain_parallel', 'replay_unchecked_chain', 'P[M[I]]', COLORS[0]),
                                          ('replay_source_parallel', 'replay_source_indices', 'P[S]', COLORS[1])]:
        workers = [1, 2, 4, 8, 16]
        times = [medians[(serial, 1)]] + [medians[(parallel, n)] for n in workers[1:]]
        axes[1].plot(workers, [times[0]/time for time in times], marker='o', label=label, color=color)
    axes[1].set_title('Independent partial bounds, then merge')
    axes[1].set_xlabel('Worker threads')
    axes[1].set_ylabel('Speedup from the same serial kernel')
    axes[1].set_xticks([1, 2, 4, 8, 16])
    axes[1].grid(alpha=.2)
    axes[1].legend()
    output = chart(fig, destination, 'deep-bounds-kernels', 'Actual model indices and coordinates. Every one of the 48 reductions must match the production group bounding box exactly. These kernels run after import in one process; they are not end-to-end loader changes.')
    output += '''<pre>Production original bounds:
corner i → render index I[i] → source index M[I[i]] → double3 P[...] → origin addition → min/max

Alternative using retained OBJ source IDs:
corner i → source index S[i] → double3 P[...] → origin addition → min/max

Whole-group reuse:
already computed complete-mesh bounds → group bounds, if coverage and coordinate rules match</pre>'''
    rows = []
    for key in ('source_index_locality', 'render_index_locality', 'render_to_source_locality'):
        data = replay[key]
        rows.append([key.replace('_locality', ''), f"{data['comparisons']:,}",
                     f"{data['same_start_cache_line']/max(1,data['comparisons'])*100:.2f}%",
                     f"{data['same_start_4KiB_page']/max(1,data['comparisons'])*100:.2f}%",
                     f"{data['within_16_ids']/max(1,data['comparisons'])*100:.2f}%",
                     f"{data['at_least_1MiB_apart']/max(1,data['comparisons'])*100:.2f}%"])
    summary['index_locality'] = {key: value for key, value in replay.items() if key.endswith('locality')}
    output += table(['Index stream', 'Adjacent comparisons', 'Same start cache line', 'Same start 4 KiB page', 'Within 16 IDs', '≥1 MiB apart'], rows)
    output += f'''<p>The expanded double-coordinate alternative adds {replay['expanded_position_bytes']:,} bytes ({replay['expanded_position_bytes']/2**20:.1f} MiB). Include its allocation and fill cost before using it in a loader. The unique-vertex scan visits 38.4 M entries instead of 227.3 M triangle corners. It is valid for this single complete group; partial groups require membership information. Reusing existing whole-mesh bounds can avoid the scan entirely, but the group's bounding-box radius must keep its current definition.</p>'''
    prefetch = [['No prefetch', f"{medians[('replay_source_indices',1)]:.3f}"]] + [[f'{ahead} corners ahead', f"{medians[(f'replay_source_prefetch_{ahead}',1)]:.3f}"] for ahead in (16, 64, 256)]
    output += table(['Software prefetch control', 'Median seconds'], prefetch)
    sweep = replay['access_sweep']
    summary['access_sweep'] = sweep
    save_csv(destination / 'deep-access-sweep.csv', sweep)
    fig, axis = plt.subplots(figsize=(12, 4.6))
    for pattern, label, color in [('sequential', 'Sequential IDs', COLORS[0]), ('actual_prefix_wrapped', 'Actual first 8 M corners, modulo range', COLORS[1]), ('random', 'Uniform pseudorandom IDs', COLORS[3])]:
        values = defaultdict(list)
        for row in sweep:
            if row['pattern'] == pattern:
                values[row['working_set_bytes']].append(row['ns_per_read'])
        axis.plot([size/2**20 for size in values], [statistics.median(value) for value in values.values()], marker='o', label=label, color=color)
    axis.set_xscale('log', base=2)
    axis.set_xlabel('Coordinate address range (MiB, logarithmic)')
    axis.set_ylabel('Nanoseconds per useful double3 read')
    axis.set_title('Access order and address range: the same source data, 8 M reads per case')
    axis.grid(alpha=.18)
    axis.legend()
    output += chart(fig, destination, 'deep-access-sweep', '54 controls: 6 address ranges × 3 patterns × 3 repetitions. Eight independent sum accumulators reduce a single arithmetic dependency. This is useful-load throughput, not measured physical DRAM bandwidth.')
    output += '''<p>The configured range is not always the touched working set. At the largest range, a sequential run touches only the first 8 M positions; the actual-prefix control follows only the first 8 M model corners. The random control spans the range. These controls explain sensitivity to locality; they do not replace a measured full-model cache trace.</p>'''
    if len(records) > 1:
        avx = defaultdict(list)
        for row in records[1]['replay']['bounds']:
            avx[(row['name'], row['workers'])].append(row['seconds'])
        comparisons = [[label, f'{medians[(name,1)]:.3f}', f'{statistics.median(avx[(name,1)]):.3f}',
                        f'{medians[(name,1)]/statistics.median(avx[(name,1)]):.2f}×'] for name, label in zip(names, labels)]
        summary['avx_bounds_replay'] = [{'case': name, 'workers': workers, 'median_s': statistics.median(values)}
                                        for (name, workers), values in avx.items()]
        output += '<details><summary>SSE2 and AVX2 replay kernels</summary>' + table(['Reduction', 'SSE2 median s', 'AVX2 median s', 'Throughput ratio'], comparisons) + '''<p>One fresh process per compiler build, three repetitions per kernel. The same boxes match exactly. This is a kernel comparison with fixed test order; it is not a paired end-to-end benchmark.</p></details>'''
    return output


def short_function(name):
    if 'buildObjMesh' in name and 'lambda_2' in name:
        return 'UV getter (out-of-line)'
    for text in ('ParseFace', 'from_chars_advanced', 'PrototypePosition', 'originalMeshBounds', 'buildObjMesh',
                 'ProcessLine', 'PrototypeSimpleFace', 'CopyIndices::Execute', 'PrototypeLine', 'vertexIndex',
                 'generateNormals', 'radixSort', 'buildPointCloud'):
        if text in name:
            return text
    return Path(name).name if '.dll' in name else name[:72]


def production_section(root, destination, summary):
    data = load(root / 'production-analysis.json')
    code = load(root / 'production-code-check.json')
    top = data['functions'][:10]
    fig, axis = plt.subplots(figsize=(12, 4.6))
    axis.barh([short_function(row['aliases'][0]) for row in top], [row['cycle_sample_percent'] for row in top], color=COLORS[0])
    axis.invert_yaxis()
    axis.set_xlabel('Share of sampled user cycles inside model.add (%)')
    axis.set_title('Unmodified production code: top sampled CPU locations')
    axis.grid(axis='x', alpha=.18)
    output = chart(fig, destination, 'deep-production-hotspots', 'Linker-map resolution on the actual viewer executable. The .text SHA-256 before and after adding the map is identical. Identical-code aliases remain in the raw JSON.')
    getter = next(row for row in data['functions'] if row['instructions'])
    summary['production'] = {'counts': data['counts'], 'UV_getter_cycle_sample_percent': getter['cycle_sample_percent'],
                              'text_identical': code['text_identical'], 'text_sha256': data['exact_code_sha256'],
                              'getter_instructions': getter['instructions'], 'top_functions': top}
    output += f'''<p>The production UV getter accounts for <strong>{getter['cycle_sample_percent']:.2f}% of sampled user cycles across the complete load</strong>. Its 23-byte body follows four dependent pointers before reading and converting the double UV value. The probe attributes about 49% of corner-mapping cycle samples to the same getter. The production capture confirms that the code path is real; native paired runs measure the useful gain from changing it.</p>
<pre>{html.escape(chr(10).join(getter['instructions']))}</pre>
<p>These loads usually hit small metadata in cache. Their serial dependencies can still cost time. Capturing the immutable UV data pointer removes repeated metadata loads. The remaining corner work and memory access limit the total gain. Event-based sampling can skid, so the sample share is not a direct prediction of removable wall time.</p>
<p>VTune's independent user-mode capture also identifies this getter: 4.499 s of sampled CPU time, after ParseFace at 9.047 s and fast_float numeric conversion at 7.485 s. VTune reports 96.566 s of aggregate CPU time over 43.364 s, with 4.624 s classified as spin. That capture includes the probe's separate geometry fingerprint validation. It therefore measures more than loading alone; its spin label does not establish a kernel wait cause.</p>'''
    with (root / 'vtune-threads.csv').open(encoding='utf-8-sig', newline='') as stream:
        threads = list(csv.DictReader(stream))
    workers = [row for row in threads if row['Thread'].startswith('Thread ')]
    summary['vtune_threads'] = threads
    output += f"<p>The VTune main thread accounts for 37.055 s of sampled CPU time, including fingerprint validation. Sixteen worker threads each account for {min(float(row['CPU Time']) for row in workers):.3f}–{max(float(row['CPU Time']) for row in workers):.3f} s. Their work is reasonably balanced by aggregate CPU time. These are different threads and a different process from the NVTX capture; thread IDs must not be joined across the captures.</p>"
    details = [[short_function(row['aliases'][0]), f"{row['cycle_sample_percent']:.2f}%", f"{row['cycle_samples']:g}",
                '; '.join(row['aliases'])] for row in top]
    output += '<details><summary>Production function names and sample support</summary>' + table(['Location', 'Cycle sample share', 'Samples', 'Linker aliases'], details) + '</details>'
    return output


def viewer_metrics(root):
    viewer = load(root / 'viewer.json')
    log = (root / 'viewer.log').read_text(encoding='utf-8')
    cpu = re.search(r'perf model_cpu_load .*?parse_ms=([\d.]+) total_ms=([\d.]+)', log)
    gpu = re.search(r'perf model_gpu_finalize .*?bytes=(\d+) upload_ms=([\d.]+) longest_step_ms=([\d.]+)', log)
    if not cpu or not gpu:
        raise ValueError(f'Missing CPU/upload completion log: {root}')
    frames = viewer['frames'].get('events', []) if isinstance(viewer['frames'], dict) else viewer['frames']
    frame_rows = [row for row in frames if 'gpuFrameMilliseconds' in row]
    return {'viewer': viewer, 'log': log, 'cpu_load_s': float(cpu[2])/1000, 'import_s': float(cpu[1])/1000,
            'upload_bytes': int(gpu[1]), 'upload_submission_s': float(gpu[2])/1000,
            'longest_upload_step_ms': float(gpu[3]), 'frame_rows': frame_rows}


def gpu_section(root, destination, summary):
    baseline = viewer_metrics(root / 'upload-baseline-viewer')
    fast = viewer_metrics(root / 'upload-fast-viewer')
    upload_rows = []
    for match in re.finditer(r'\[([^]]+)\].*research upload_frame bytes=(\d+) wall_ms=([\d.]+)', fast['log']):
        from datetime import datetime
        upload_rows.append({'timestamp': match[1], 'seconds': datetime.fromisoformat(match[1]).timestamp(),
                            'bytes': int(match[2]), 'work_ms': float(match[3])})
    if not upload_rows:
        raise ValueError('No upload frame records')
    origin = upload_rows[0]['seconds']
    for row in upload_rows:
        row['relative_s'] = row['seconds'] - origin
    elapsed = upload_rows[-1]['relative_s']
    rows = []
    for name, data in [('Production: inactive 50 ms', baseline), ('Research: inactive 1 ms', fast)]:
        viewer = data['viewer']
        rows.append([name, f"{viewer['load_rpc_ms']/1000:.3f}", f"{data['cpu_load_s']:.3f}",
                     f"{(viewer['load_rpc_ms']/1000-data['cpu_load_s']):.3f}", f"{data['upload_submission_s']:.3f}"])
    output = table(['Viewer', 'RPC load s', 'Logged CPU s', 'RPC minus CPU s', 'Summed upload work s'], rows)
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.5))
    axes[0].step([row['relative_s'] for row in upload_rows], np.cumsum([row['bytes'] for row in upload_rows])/1e9, where='post', color=COLORS[0])
    axes[0].set_title('Fast viewer: actual upload progress by frame')
    axes[0].set_xlabel('Seconds from first upload-frame log')
    axes[0].set_ylabel('Cumulative payload (GB)')
    axes[0].grid(alpha=.18)
    for name, data, color in [('Production 50 ms', baseline, COLORS[0]), ('Research 1 ms', fast, COLORS[1])]:
        viewer = data['viewer']
        begin = viewer['render_started_ms']
        finish = begin + viewer['render_duration_ms']
        samples = [row for row in viewer.get('gpu', {}).get('samples', []) if begin <= row['time_ms'] < finish]
        if samples:
            axes[1].plot([(row['time_ms']-begin)/1000 for row in samples], [row['gpu_percent'] for row in samples], color=color, label=name)
    axes[1].set_title('Device-wide GPU busy: settled fixed-camera interval')
    axes[1].set_xlabel('Seconds from steady recording start')
    axes[1].set_ylabel('NVML busy (%)')
    axes[1].set_ylim(0, 105)
    axes[1].legend(fontsize=9)
    output += chart(fig, destination, 'deep-upload-pacing', 'Only the research viewer changes inactive-frame pacing. The 4 MiB upload steps, 16 MiB/frame cap and 4 ms work target remain in place. GPU busy includes other applications.')
    minimum_frames = (fast['upload_bytes'] + 16*1024*1024 - 1) // (16*1024*1024)
    output += f'''<p>The production hidden viewer permits at most 16 MiB per 50 ms frame. This payload needs at least <strong>{minimum_frames} frames</strong>, or about {(minimum_frames-1)*.05:.2f} s between first and last frame even if the copy cost were negligible. The native research viewer logs {len(upload_rows)} upload frames over {elapsed:.3f} s. Their payload sum is {sum(row['bytes'] for row in upload_rows):,} bytes. Pacing and the per-frame byte cap are part of observed upload latency.</p>
<p>Summed step time includes staging copies, queue operations and waits. It is not PCIe transfer time. The elapsed frame interval includes pacing and other work. Neither measure alone establishes PCIe bandwidth or copy-engine occupancy. A Vulkan GPU timeline is still required to separate these costs.</p>'''
    resource_rows = []
    for name, data in [('Production', baseline), ('Research 1 ms', fast)]:
        viewer = data['viewer']
        begin, finish = viewer['render_started_ms'], viewer['render_started_ms'] + viewer['render_duration_ms']
        readings = [row for row in viewer.get('gpu', {}).get('samples', []) if begin <= row['time_ms'] < finish]
        frames = data['frame_rows']
        numeric = [row.get('data', row) for row in frames]
        gpu_times = [row['gpuFrameMilliseconds'] for row in numeric if 'gpuFrameMilliseconds' in row]
        resource_rows.append([name, f'{len(frames)}', f'{len(frames)/(viewer["render_duration_ms"]/1000):.1f}',
                              f'{statistics.median(gpu_times):.2f}' if gpu_times else 'see frame CSV',
                              f'{statistics.median([row["gpu_percent"] for row in readings]):.0f}%' if readings else 'no samples',
                              f'{statistics.median([row["sm_clock_MHz"] for row in readings]):.0f}' if readings else 'no samples',
                              f'{statistics.median([row["power_W"] for row in readings]):.1f}' if readings else 'no samples',
                              f'{min(row["temperature_C"] for row in readings)}–{max(row["temperature_C"] for row in readings)}' if readings else 'no samples'])
    output += table(['Settled fixed camera', 'Frames', 'Frames/s', 'Median GPU ms', 'Median GPU busy', 'Median SM MHz', 'Median GPU W', 'GPU temperature °C'], resource_rows)
    output += '''<p>Both recordings wait 25 s after screenshot capture before measuring eight seconds of ordinary frames. This reduces overlap with the annotation worker; the process CPU trace can be checked for residual work. The 1 ms experiment measures a hidden-window workload with different pacing. It is not a visible-window FPS result. NVML power, clock and temperature are coarse device samples. Its constant 100% memory-utilization field is excluded from bandwidth conclusions.</p>'''
    summary['upload_frames'] = upload_rows
    summary['viewer_comparison'] = [{**{key: value for key, value in data.items() if key not in ('viewer', 'log', 'frame_rows')},
                                    'rpc_load_s': data['viewer']['load_rpc_ms']/1000} for data in (baseline, fast)]
    summary['viewer_resource_rows'] = resource_rows
    save_csv(destination / 'deep-upload-frames.csv', upload_rows)
    fig, axes = plt.subplots(3, 1, figsize=(13, 8.5), sharex=True)
    for name, data, color in [('Production 50 ms', baseline, COLORS[0]), ('Research 1 ms', fast, COLORS[1])]:
        viewer = data['viewer']
        origin_ms = viewer['load_request_ms']
        samples = viewer['samples']
        times, busy = [], []
        for i in range(4, len(samples), 4):
            before, after = samples[i-4], samples[i]
            interval = after['time_ms'] - before['time_ms']
            times.append((after['time_ms']-origin_ms)/1000)
            busy.append((after['cpu_ms']-before['cpu_ms'])/interval if interval else 0)
        axes[0].plot(times, busy, color=color, label=name, linewidth=1)
        axes[1].plot([(row['time_ms']-origin_ms)/1000 for row in samples], [row['private_bytes']/2**30 for row in samples], color=color, label=name)
        gpu_samples = viewer.get('gpu', {}).get('samples', [])
        axes[2].plot([(row['time_ms']-origin_ms)/1000 for row in gpu_samples], [row['gpu_percent'] for row in gpu_samples], color=color, linewidth=1)
        save_csv(destination / ('deep-viewer-' + ('baseline' if data is baseline else 'fast') + '-frames.csv'),
                 [{'gpu_ms': row['gpuFrameMilliseconds'], 'frame_ms': row['frameMilliseconds']} for row in data['frame_rows']])
    axes[0].set_ylabel('Logical CPUs busy, process CPU delta')
    axes[0].set_ylim(bottom=0)
    axes[0].set_title('Native viewer resource history: import, upload, annotation and settled rendering')
    axes[0].legend(fontsize=9)
    axes[1].set_ylabel('Process private commit (GiB)')
    axes[2].set_ylabel('Device-wide GPU busy (%)')
    axes[2].set_ylim(0, 105)
    axes[2].set_xlabel('Seconds from model.add request')
    for axis in axes:
        axis.grid(alpha=.17)
    output += chart(fig, destination, 'deep-viewer-resources', 'CPU deltas aggregate all viewer threads over about 0.2 s. The later CPU activity includes annotation preparation and rendering. Private commit is allocated process memory, not resident RAM or GPU memory.')
    output += '<p>GPU busy before model upload covers the progress/UI workload and other applications. OBJ parsing and CPU mesh preparation remain CPU work. Device-wide GPU activity does not show that the parser runs on the GPU.</p>'
    return output


def memory_section(root, destination, summary):
    path = root / 'memory-resident-viewer' / 'viewer.json'
    if not path.exists():
        return ''
    viewer = load(path)
    snapshots = viewer['memory_maps']
    rows = []
    for snapshot in snapshots:
        totals = snapshot['totals']
        rows.append({'stage': snapshot['name'], 'time_ms': snapshot['time_ms'],
                     'private_bytes': snapshot['private_bytes'], 'working_set_bytes': snapshot['working_set_bytes'],
                     'virtual_private_commit_bytes': totals.get('private_committed_bytes', 0),
                     'virtual_mapped_commit_bytes': totals.get('mapped_committed_bytes', 0),
                     'virtual_image_commit_bytes': totals.get('image_committed_bytes', 0),
                     'virtual_reserved_bytes': sum(value for key, value in totals.items() if key.endswith('_reserved_bytes')),
                     'region_count': snapshot['region_count'], 'query_ms': snapshot['duration_ms']})
    summary['memory_map_snapshots'] = rows
    save_csv(destination / 'deep-memory-map.csv', rows)
    (destination / 'deep-memory-map.json').write_text(json.dumps(snapshots, indent=2), encoding='utf-8')
    labels = ['Before load', 'Load returned', 'After settling', 'Model removed', 'New empty scene', 'Empty +10 s']
    x = np.arange(len(rows))
    fig, axes = plt.subplots(2, 1, figsize=(13, 8), sharex=True)
    width = .35
    axes[0].bar(x-width/2, [row['private_bytes']/2**30 for row in rows], width, label='Process private commit', color=COLORS[0])
    axes[0].bar(x+width/2, [row['working_set_bytes']/2**30 for row in rows], width, label='Process working set', color=COLORS[1])
    axes[0].set_ylabel('Process counter (GiB)')
    axes[0].set_title('Native production viewer: memory retention and release')
    axes[0].legend(fontsize=9)
    bottom = np.zeros(len(rows))
    for key, label, color in [('virtual_private_commit_bytes', 'Private virtual pages', COLORS[0]),
                               ('virtual_mapped_commit_bytes', 'Mapped section pages', COLORS[1]),
                               ('virtual_image_commit_bytes', 'Executable image pages', COLORS[2])]:
        values = np.array([row[key]/2**30 for row in rows])
        axes[1].bar(x, values, bottom=bottom, label=label, color=color)
        bottom += values
    axes[1].set_ylabel('VirtualQueryEx committed pages (GiB)')
    axes[1].set_xticks(x, labels)
    axes[1].legend(fontsize=9)
    for axis in axes:
        axis.grid(axis='y', alpha=.18)
        axis.set_axisbelow(True)
    output = chart(fig, destination, 'deep-memory-retention', 'One fresh production viewer. Model removal is followed by scene.new to reset the scene and undo history. VirtualQueryEx classifications are separate from process commit accounting; per-page validity is measured below.')
    output += table(['Checkpoint', 'Private commit GiB', 'Working set GiB', 'VM private GiB', 'VM mapped GiB',
                     'VM image GiB', 'Reserved VA GiB', 'Query ms'],
                    [[row['stage'], *[f'{row[key]/2**30:.3f}' for key in ('private_bytes', 'working_set_bytes',
                      'virtual_private_commit_bytes', 'virtual_mapped_commit_bytes', 'virtual_image_commit_bytes', 'virtual_reserved_bytes')],
                      f'{row["query_ms"]:.1f}'] for row in rows])
    page_rows = []
    for snapshot in snapshots:
        query = snapshot['page_query']
        page_bytes = query['page_bytes']
        wc = query['by_protection'].get('1028', {})
        page_rows.append({'stage': snapshot['name'],
                          'queried_bytes': query['totals']['queried_pages']*page_bytes,
                          'valid_bytes': query['totals']['valid_pages']*page_bytes,
                          'writecombine_queried_bytes': wc.get('queried_pages', 0)*page_bytes,
                          'writecombine_valid_bytes': wc.get('valid_pages', 0)*page_bytes})
    summary['memory_page_validity'] = page_rows
    save_csv(destination / 'deep-page-validity.csv', page_rows)
    settled = next(snapshot for snapshot in snapshots if snapshot['name'] == 'after_settle')
    settled_pages = settled['page_query']
    protection = settled_pages['by_protection']
    keys = ('4', '1028')
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.5))
    positions = np.arange(2)
    axes[0].bar(positions-.18, [protection[key]['queried_pages']*4096/2**30 for key in keys], .36,
                color=COLORS[0], label='Queried committed virtual pages')
    axes[0].bar(positions+.18, [protection[key]['valid_pages']*4096/2**30 for key in keys], .36,
                color=COLORS[1], label='Pages with Valid=1')
    axes[0].set_xticks(positions, ['Cached read/write\n0x004', 'Write-combined read/write\n0x404'])
    axes[0].set_title('Settled viewer: committed pages versus page validity')
    axes[0].set_ylabel('GiB of CPU virtual pages')
    axes[0].legend(fontsize=8)
    axes[1].plot(x, [row['writecombine_queried_bytes']/2**30 for row in page_rows], 'o-',
                 label='Write-combined committed pages', color=COLORS[0])
    axes[1].plot(x, [row['writecombine_valid_bytes']/2**30 for row in page_rows], 'o-',
                 label='Write-combined Valid=1 pages', color=COLORS[1])
    axes[1].set_xticks(x, ['Before', 'Load', 'Settled', 'Remove', 'New', '+10 s'])
    axes[1].set_title('Write-combined allocation lifetime')
    axes[1].set_ylabel('GiB of CPU virtual pages')
    axes[1].legend(fontsize=8)
    for axis in axes:
        axis.grid(axis='y', alpha=.18)
        axis.set_axisbelow(True)
    output += chart(fig, destination, 'deep-page-validity', 'QueryWorkingSetEx queries every committed page in allocations of at least 1 MiB. It does not touch or lock target memory. Valid CPU virtual pages are separate from GPU VRAM residency and the ordinary process working-set counter.')
    settled_row = next(row for row in page_rows if row['stage'] == 'after_settle')
    largest = next(row for row in settled['allocations'] if row['types'] == ['private'] and row['protections'] == [4])
    output += f'''<p>The settled viewer has <strong>{settled_row['writecombine_queried_bytes']/2**30:.3f} GiB of queried write-combined committed pages</strong>, with only <strong>{settled_row['writecombine_valid_bytes']/2**20:.2f} MiB marked Valid</strong>. This accounts for much of the gap between private commit and the working set. Their page protection, size distribution and load/remove lifetime are consistent with graphics mappings. That attribution is an inference; the page APIs do not provide the driver allocation owner or VRAM placement.</p>
<p>The largest cached private allocation has {largest['committed_bytes']/1e9:.3f} GB committed and {largest['valid_pages']*4096/1e9:.3f} GB of valid pages. These sizes match the vertex-capacity and populated-vertex sizes; the identity is inferred from size rather than an allocation stack. Thus reducing spare vector capacity reduces commit without necessarily saving the same amount of steady resident RAM. Native reserve tests separately show a lower import RSS peak.</p>'''
    output += table(['Checkpoint', 'All queried pages GiB', 'All Valid pages GiB', 'Write-combined queried GiB', 'Write-combined Valid MiB'],
                    [[row['stage'], f'{row["queried_bytes"]/2**30:.3f}', f'{row["valid_bytes"]/2**30:.3f}',
                      f'{row["writecombine_queried_bytes"]/2**30:.3f}', f'{row["writecombine_valid_bytes"]/2**20:.2f}'] for row in page_rows])
    output += '<p>The queried-page inventory excludes allocations below 1 MiB. Valid pages can include large pages or special mappings outside the ordinary working set, and the scan spans multiple kernel calls. Its total need not equal the later process RSS reading. The loaded scans take about 2.2–2.4 s, so these captures are memory controls and are not used for native timing comparisons.</p>'
    output += '''<p>The source retains the five mesh arrays, point-cloud arrays and later annotation arrays shown in the CPU memory diagram. GPU geometry uses device-local allocations; its Buffer struct holds an owner and size/stride fields. The upload queue keeps one fixed 16 MiB CPU-visible ring. It copies each source range immediately and reuses ring space after GPU completion. It does not retain a full CPU shadow of all GPU geometry. Three frame arenas also retain reusable pages, normally at least 4 MiB per used arena.</p>
<p>VirtualQueryEx classifies virtual page ranges. It does not identify the C++ object that allocated them, and its committed-page totals are not identical to the process private-commit counter. Copy-on-write pages keep their mapped/image type. This API alone does not establish page residency. The scan is read-only and not atomic; annotation and rendering can run during a checkpoint. Heap allocation stacks and driver allocation records are still required to assign every byte.</p>
<p>Scene history copies file metadata, not the mesh arrays. Model removal and creating a new empty scene are measured separately. The latter also resets runtime state; the intervening time permits delayed graphics cleanup. A later decrease cannot be assigned only to undo-history clearing. The final empty checkpoint tests release after more GPU retirement and worker time. Two load/remove captures do not establish long-term leak behavior.</p>'''
    output += '<details><summary>Largest virtual allocations at the settled checkpoint</summary>' + table(
        ['Allocation base', 'Committed MiB', 'Valid pages MiB', 'Reserved MiB', 'VM type', 'Protection', 'Mapped file, if available'],
        [[f'0x{row["base"]:x}', f'{row["committed_bytes"]/2**20:,.2f}',
          f'{row["valid_pages"]*4096/2**20:,.2f}' if 'valid_pages' in row else 'Not queried',
          f'{row["reserved_bytes"]/2**20:,.2f}',
          ', '.join(row['types']), ', '.join(f'0x{value:x}' for value in row['protections']),
          row['mapped_file'] or 'Not identified by this API'] for row in settled['allocations'][:20]]) + '</details>'
    output += '<p><a href="deep-memory-map.json">Full virtual-region snapshots</a> · <a href="deep-memory-map.csv">Memory checkpoints CSV</a>. Methods: <a href="https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex">Microsoft VirtualQueryEx</a>, <a href="https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmappedfilenamew">GetMappedFileNameW</a> and <a href="https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-queryworkingsetex">QueryWorkingSetEx</a>.</p>'
    return output


def simd_section(root, destination, summary):
    record = load(root / 'simd-replay.json')['records'][0]
    grouped = defaultdict(list)
    for row in record['simd_replay']['bounds']:
        grouped[(row['name'], row['workers'])].append(row['seconds'])
    rows = [{'kernel': name, 'workers': workers, 'median_s': statistics.median(values),
             'min_s': min(values), 'max_s': max(values)} for (name, workers), values in grouped.items()]
    summary['explicit_simd'] = rows
    save_csv(destination / 'deep-explicit-simd.csv', rows)
    fig, axes = plt.subplots(1, 2, figsize=(12, 4))
    for axis, workers in zip(axes, (1, 16)):
        values = [row['median_s'] for row in rows if row['workers'] == workers]
        axis.barh(['C++ unchecked chain', 'Explicit SSE2 mask/select', 'Explicit SSE4.1 blend'], values, color=COLORS[:3])
        axis.invert_yaxis()
        axis.set_title(f'{workers} worker' + ('s' if workers > 1 else ''))
        axis.set_xlabel('Median seconds, 3 repetitions')
        for y, value in enumerate(values):
            axis.text(value, y, f'  {value:.3f}', va='center')
        axis.set_xlim(0, max(values)*1.2)
    output = chart(fig, destination, 'deep-explicit-simd', 'The same P[M[I]] chain and exact bounding box. The explicit kernels keep XY min/max in 128-bit registers and use a scalar Z lane. This executable is compiled for the SSE2 baseline; the SSE4.1 call is guarded by CPUID.')
    output += '''<p>The compiler's SSE2 min/max uses floating comparison, an integer conditional move of a pointer, and a dependent value load. AVX code uses <code>vcmpltsd</code> and <code>vblendvpd</code>. The latter operates on XMM registers here. This is a value-selection change; it does not require 256-bit vectors or AVX2 integer instructions.</p>
<pre>SSE2 compiler sequence for one minimum:
movsd   xmm0, [box]
comisd  xmm0, [point]
mov     rax, box_address
cmova   rax, point_address
mov     rax, [rax]             ; chosen value after pointer selection
mov     [box], rax

AVX compiler sequence:
vmovsd    xmm2, [box]
vmovsd    xmm1, [point]
vcmpltsd  xmm0, xmm1, xmm2
vblendvpd xmm0, xmm2, xmm1, xmm0
vmovsd   [box], xmm0</pre>
<p>The explicit SSE2 mask kernel uses compare, AND, AND-NOT and OR to select values. SSE4.1 replaces that selection with a blend. Both keep reduction values in registers across iterations. This test separates register-based reduction from the compiler ISA switch. It is a specialized kernel for validated finite OBJ coordinates; general mesh access and empty/partial group handling still need the production contract.</p>'''
    return output


def write_deep_report(root, destination):
    if not root.exists():
        return ''
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10, 'axes.spines.top': False,
                         'axes.spines.right': False, 'svg.fonttype': 'path'})
    summary = {'date': '2026-10-12', 'scope': 'Deep second pass; separate from the 11 October timings'}
    hardware, profiles = hardware_section(root, destination, summary)
    production = production_section(root, destination, summary)
    experiments = experiments_section(root, destination, summary)
    replay = replay_section(root, destination, summary)
    simd = simd_section(root, destination, summary)
    workers = worker_section(root, destination, summary)
    instructions = instruction_section(profiles, destination, summary)
    calibration = calibration_section(root, destination, summary)
    gpu = gpu_section(root, destination, summary)
    memory = memory_section(root, destination, summary)
    (destination / 'deep-summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    archive_files = [path for path in root.iterdir() if path.is_file() and path.suffix in ('.json', '.csv', '.sqlite', '.nsys-rep', '.log')]
    archive_files += [path for path in destination.glob('deep-*.csv')]
    if memory:
        archive_files += [destination / 'deep-memory-map.json']
    with zipfile.ZipFile(destination / 'deep-measurements.zip', 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in archive_files:
            archive.write(path, 'records/' + path.name)
        if memory:
            for name in ('memory-map-viewer', 'memory-resident-viewer'):
                for filename in ('viewer.json', 'viewer.log'):
                    path = root / name / filename
                    archive.write(path, name + '/' + filename)
        for session in root.glob('uprof-*'):
            if session.is_dir() and (session / 'session.uprof').exists():
                for name in ('session.uprof', 'report.csv'):
                    if (session / name).exists():
                        archive.write(session / name, session.name + '/' + name)
        archive.writestr('README.txt', 'Native .prd/.db traces, VTune results and matching executables/PDBs remain in build/obj-profile-measurements/deep.\nThis archive contains numerical records, decoded sample locations, reports, the NVTX trace and exported tables.\nHardware values are sampled estimates, not exact event totals. Native timing batches exclude profiler overhead.\n')
    reserve = summary['native_experiments']['reservation']
    uv = summary['native_experiments']['uv-pointer']
    saved = reserve[0]['mesh_capacity_bytes'] - reserve[1]['mesh_capacity_bytes']
    mapping_gain = (1-uv[1]['mapping_median_s']/uv[0]['mapping_median_s'])*100
    cpu_gain = (1-uv[1]['ready_median_s']/uv[0]['ready_median_s'])*100
    gpu_baseline, gpu_fast = summary['viewer_comparison']
    gpu_rows = summary['viewer_resource_rows']
    return f'''<section id="deep-analysis">
<h2>12 October: hardware counters, kernel controls and native viewer tests</h2>
<p class="lead">The new tools provide real AMD core-counter samples and two independent CPU profiles. Native tests identify useful changes in memory capacity, attribute access, bounds reduction and upload pacing.</p>
<ul>
<li><strong>Reserve growth:</strong> {saved/2**20:.1f} MiB less retained mesh capacity, with identical geometry.</li>
<li><strong>UV pointer capture:</strong> {mapping_gain:.1f}% less corner-mapping time; {cpu_gain:.1f}% less CPU-ready time in four native paired rounds.</li>
<li><strong>Bounds:</strong> direct validated access, register reductions and independent workers are larger opportunities than removing one index level.</li>
<li><strong>Hidden upload:</strong> shorter inactive-frame pacing cuts the native load endpoint from {gpu_baseline['rpc_load_s']:.2f} s to {gpu_fast['rpc_load_s']:.2f} s, with almost unchanged CPU preparation.</li>
<li><strong>GPU use:</strong> settled hidden rendering reaches {gpu_rows[1][2]} frames/s and {gpu_rows[1][4]} device-wide busy time under the research pacing. Its higher GPU clock and power are part of that result.</li>
</ul>
<p class="note">The 11 October baseline below is a separate batch. It took 43.31 s for SSE2 CPU preparation; current native baselines are about 35 s. The production source is unchanged. The difference between days is not assigned to an optimization. Use the same-batch paired results for claims about changes. CPU frequency and temperature were not recorded per stage.</p>
<p><a href="#deep-hardware">Core counters</a> · <a href="#deep-production">Production code</a> · <a href="#deep-experiments">Native experiments</a> · <a href="#deep-bounds">Bounds + locality</a> · <a href="#deep-simd">Explicit SIMD</a> · <a href="#deep-workers">Worker timeline</a> · <a href="#deep-isa">Instruction forms</a> · <a href="#deep-gpu">GPU + upload</a> · <a href="#deep-memory">Memory retention</a> · <a href="#deep-evidence">Evidence and open measurements</a></p>
<h3 id="deep-hardware">Hardware measurements by phase</h3>
<p>AMD uProf 5.3.521.0 collected core PMCs on the Ryzen 7 5800H. Counter sets fit the six reported core counters: cycles + retired instructions plus branch/DTLB, demand-fill/dispatch, or FP events. The FP event uses a paired merge counter. Counts below are sample counts multiplied by each event's period. User mode is enabled; OS events are excluded.</p>
<p>Phase markers have absolute QPC timestamps and are aligned to native collection metadata; those offsets are about 16–21 ms. The short calibration below exposes a further approximately 16 ms residual difference in the database time origin. Each phase includes a +16 ms sensitivity check. Phase selection uses start-inclusive, end-exclusive windows. Short phases below 100 ms are omitted from counter ratios. Parent phases overlap their children and must not be added to them.</p>
{hardware}
<p>Local-DRAM/IO demand-fill counts exclude prefetch fills, stores and other traffic. Multiplying them by 64 bytes does not measure physical DRAM bandwidth. DTLB miss counts do not give page-walk duration. FP counts include arithmetic in unused vector lanes; they are not a count of useful geometry operations. Comparisons and blend instructions are not included as FP arithmetic. No zero counter is interpreted as proof that the event never occurred. The raw event definitions, sample counts and time ranges are preserved.</p>
<h3 id="deep-production">Validate the hotspot in production code</h3>{production}
<h3 id="deep-experiments">Native experiments: gains and a negative result</h3>{experiments}
<h3 id="deep-bounds">Bounds, parallel reduction and address locality</h3>{replay}
<h3 id="deep-simd">Isolate value selection and 128-bit SIMD</h3>{simd}
<h3 id="deep-workers">Read/decode concurrency and serial work</h3>{workers}
<h3 id="deep-isa">Observed instruction forms and calibration</h3>{instructions}{calibration}
<h3 id="deep-gpu">Upload pacing and settled GPU use</h3>{gpu}
<p>The faster GPU frame time includes a higher SM clock: about 1,875 MHz versus 1,575 MHz in the production hidden run. This is a workload/power-state correlation, not a geometry or shader optimization. The same camera, 1280×720 drawable, 805×687 model viewport and 4× MSAA remain in use. Each viewer has one native capture; these results do not establish a statistical FPS distribution across machines.</p>
<h3 id="deep-memory">Address maps, retained memory and model removal</h3>{memory}
<h3 id="deep-evidence">What the installed tools can and cannot measure here</h3>
{table(['Tool / source', 'Observed result', 'Remaining measurement'], [
['AMD uProf core PMCs', 'Nonzero cycle, instruction, branch, DTLB, cache-fill and FP samples; FP control within 0.4%', 'IBS, L3, DF and UMC sources reported unavailable; no DRAM controller bandwidth or full stall breakdown'],
['AMD uProf PCM', 'Rejected this processor: unsupported processor model / missing configuration file', 'Continuous CPU frequency, energy and memory-controller metrics'],
['VTune 2025.3', 'User-mode Hotspots with call stacks worked', 'Thread wait causes and kernel scheduling need another trace source'],
['Nsight Systems 2026.5.1', 'Full CPU NVTX trace worked', 'Vulkan injection failed because registry-write permission is unavailable; GPU metrics also require more access'],
['Nsight Graphics 2026.3.1', 'Target injection/connection worked in a smoke test', 'GPU Trace reported unavailable GPU performance counters; no pipeline/cache/copy-engine capture obtained'],
['WPR / WPA / xperf', 'WPR still rejected CPU recording with 0xc5585011', 'ETW context switches, ready-thread delays, hard faults, heap allocation stacks and physical I/O latency'],
['Intel SDE', 'Not installed, as reported by the user', 'Exact dynamic opcode/ISA counts; SDE timings must not be used as native wall time'],
['NVML + process counters', 'CPU/memory histories; device-wide GPU busy, clock, power and temperature', 'GPU engine-level occupancy and CPU per-phase clock/temperature']])}
<p>Tool installation does not guarantee access to every source. This session is not elevated. Hyper-V is active, and uProf reports several sources unavailable; that inventory does not prove the cause of each unavailable source. Failed attempts and their messages are in the archive. No system profiling policy, virtualization setting or NVIDIA counter-access setting was changed.</p>
<p>The next most useful capture is an elevated ETW CPU/context-switch/I/O trace plus a Vulkan GPU timeline with counter access. That would distinguish runnable delay, allocation, file waits, staging copy, fence waits and GPU copy work. After access is available, use the same model, camera and native paired controls. Keep exact opcode counting in a separate SDE run.</p>
<p>Further loader candidates now have evidence: reuse complete-group bounds, add validated specialized reductions, parallelize independent bounds, and keep the UV pointer stable. Reserve growth should be bounded for models with many seams. Defer point preparation only after checking picking and marker behavior. Test cancellation and partial groups before promoting a research kernel. A binary cache or a different file format can avoid ASCII conversion on repeated loads, but needs its own versioning, invalidation, storage-size and cold-load tests.</p>
<details><summary>Reproduce the deeper measurements</summary><pre>uv run experiments/obj-profile/deep_tools.py "D:\\temp\\obj_tests\\20190810_BearTrap_Ground_Model.obj" --executable build/vs2026-vcpkg/bin/Release/woby_obj_profile_sse2.exe --output build/obj-profile-measurements/deep --sets branch,cache,fp
uv run experiments/obj-profile/run.py build/vs2026-vcpkg/bin/Release/woby_obj_profile_sse2.exe "D:\\temp\\obj_tests\\20190810_BearTrap_Ground_Model.obj" --modes prepare --workers 16 --rounds 4 --reserve-percent 0,2 --output build/reservation.json
uv run experiments/obj-profile/run.py build/vs2026-vcpkg/bin/Release/woby_obj_profile_sse2.exe "D:\\temp\\obj_tests\\20190810_BearTrap_Ground_Model.obj" --modes replay --workers 16 --rounds 1 --output build/bounds.json
uv run experiments/obj-profile/run.py build/vs2026-vcpkg/bin/Release/woby_obj_profile_sse2.exe "D:\\temp\\obj_tests\\20190810_BearTrap_Ground_Model.obj" --modes simd-replay --workers 16 --rounds 1 --output build/simd.json
uv run experiments/obj-profile/viewer.py build/vs2026-vcpkg/bin/Release/woby.exe "D:\\temp\\obj_tests\\20190810_BearTrap_Ground_Model.obj" --memory-map --settle-seconds 25 --render-seconds 2 --output build/memory-viewer</pre><p>Use the compare-executable option for UV variants or AVX2. Preserve matching executable/PDB files before rebuilding. Native measurement records include executable and source hashes. Profiler captures include separate geometry validation; exclude that phase from loader timing and instruction summaries.</p></details>
<p><a href="deep-summary.json">Deep summary JSON</a> · <a href="deep-measurements.zip">Deep numerical data archive</a> · <a href="deep-hardware.csv">Phase counters CSV</a> · <a href="deep-native-pairs.csv">Native paired runs CSV</a> · <a href="deep-bounds-replay.csv">Bounds kernels CSV</a> · <a href="deep-access-sweep.csv">Access controls CSV</a></p>
<p>Native tool results remain in <code>build/obj-profile-measurements/deep</code>: <a href="../../../build/obj-profile-measurements/deep/uprof-prepare-sse2-branch/session.uprof">uProf session</a>, <a href="../../../build/obj-profile-measurements/deep/nsys-cpu.nsys-rep">Nsight NVTX trace</a>, <a href="../../../build/obj-profile-measurements/deep/vtune-prepare-valid/">VTune result</a>. Matching symbols are stored beside the captures. These local links require the original workspace; the numerical archive is portable.</p>
<p class="small">Primary references: <a href="https://docs.amd.com/r/en-US/57368-uProf-user-guide/Profiling-Concepts">AMD sampling concepts</a>; installed Family 19h Model 50h event definitions <code>AMDuProf/bin/Data/Events/0x19_0x5.xml</code>; <a href="https://docs.nvidia.com/nsight-systems/UserGuide/index.html">Nsight Systems requirements and CLI</a>; <a href="https://developer.nvidia.com/ERR_NVGPUCTRPERM">NVIDIA counter access</a>; <a href="https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html">Nsight Graphics GPU Trace</a>; <a href="https://learn.microsoft.com/en-us/troubleshoot/windows-server/support-tools/support-tools-xperf-wpa-wpr">Windows performance tools</a>.</p>
</section><h2 id="first-pass">11 October: first-pass timings and complete data flow</h2>'''
