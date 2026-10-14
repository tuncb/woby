# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib>=3.10", "numpy>=2"]
# ///
"""Build the end-to-end measurement report from preserved records."""
import argparse
import csv
import html
import json
from pathlib import Path
import shutil
import statistics
import zipfile

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from e2e_analyze import summarize_native, summarize_run, phases, first_marks, timechart_series, distribution
from e2e_evidence import all_evidence
from e2e_sections import detailed_sections


COLORS = ["#176f76", "#ba653b", "#617caa", "#91734d", "#58744e"]
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "svg.fonttype": "none",
                     "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.alpha": .18})


def save(fig, output, name):
    fig.savefig(output / (name + ".png"), dpi=160, bbox_inches="tight")
    fig.savefig(output / (name + ".svg"), bbox_inches="tight")
    plt.close(fig)
    return f'<figure><img src="{name}.svg" alt="{name.replace("-", " ")}"><figcaption><a href="{name}.png">PNG</a> · <a href="{name}.svg">SVG</a></figcaption></figure>'


def table(headers, rows):
    return '<div class="scroll"><table><thead><tr>' + ''.join('<th>'+html.escape(str(h))+'</th>' for h in headers) + '</tr></thead><tbody>' + ''.join(
        '<tr>'+''.join('<td>'+html.escape(str(c) if c is not None and str(c).strip() else '—')+'</td>' for c in row)+'</tr>' for row in rows) + '</tbody></table></div>'


def figure_native(native, output):
    names = list(native["groups"])
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.1))
    for i, name in enumerate(names):
        rows = [r for r in native["runs"] if r["configuration"] == name]
        axes[0].scatter([i]*len(rows), [r["load_rpc_ms"]/1000 for r in rows], color=COLORS[i], s=45)
        axes[0].plot([i-.15,i+.15], [statistics.median(r["load_rpc_ms"]/1000 for r in rows)]*2, color=COLORS[i], lw=3)
        axes[1].scatter([i]*len(rows), [r["initial_upload_ms"]/1000 for r in rows], color=COLORS[i], s=45)
    for axis in axes:
        axis.set_xticks(range(len(names)), [n.replace('-', '\n') for n in names])
    axes[0].set(ylabel="Load RPC completion, seconds", title="Full readiness · three fresh processes per control")
    axes[1].set(ylabel="Initial upload worker, seconds", title="2.139 GB initial upload · allocation and completion included")
    fig.tight_layout()
    return save(fig, output, "native-controls")


def figure_timeline(record, output):
    rows = phases(record)
    marks = first_marks(record["trace"]); start = marks["model_add_requested"]
    selected = [("CPU import", "cpu_import"), ("Initial index validation", "gpu_index_validation"),
                ("Initial GPU upload", "gpu_upload_worker_total"), ("Source indices", "post_source_indices"),
                ("Original bounds", "post_original_bounds"), ("Point preparation", "post_point_preparation"),
                ("Annotations", "post_annotation_cache"), ("Point GPU upload", "gpu_upload_worker_total")]
    fig, ax = plt.subplots(figsize=(12, 5))
    for i, (label, name) in enumerate(selected):
        matching=[r for r in rows if r['name']==name]
        row = matching[-1] if label=='Point GPU upload' else matching[0]
        ax.barh(i, row["wall_ms"]/1000, left=(row["start_ms"]-start)/1000, color=COLORS[i % len(COLORS)], height=.65)
        ax.text((row["start_ms"]-start)/1000+.05, i, f'{row["wall_ms"]/1000:.3f}s', va="center", fontsize=9)
    for name, label, color in (("first_scene_frame_gpu_complete", "First scene GPU completion", "#176f76"),
                                ("all_tools_ready", "All tools ready", "#ba653b")):
        value = (marks[name]-start)/1000
        ax.axvline(value, color=color, ls="--")
        ax.text(value+(.1 if 'first' in name else -.1),.98,f'{label}\n{value:.3f}s',
                transform=ax.get_xaxis_transform(),ha='left' if 'first' in name else 'right',va='top',color=color,fontsize=9)
    ax.set(yticks=range(len(selected)), yticklabels=[l for l,_ in selected], xlabel="Seconds after model.add starts", title="One instrumented hidden run · CPU and upload phases")
    ax.invert_yaxis(); fig.tight_layout()
    return save(fig, output, "pipeline-timeline")


def figure_phase_budget(group, output):
    names = ["mapping_table_allocate", "mapping_index_initialize", "mapping_id_resolution", "mapping_parser_records_release", "mapping_vertex_allocate", "mapping_position_map", "mapping_primary_gather", "mapping_seam_gather", "mapping_lookup_release"]
    post = ["post_source_indices", "post_original_bounds", "post_point_preparation", "post_group_bounds", "post_annotation_cache"]
    fig, axes = plt.subplots(1,2,figsize=(12,5))
    for ax, items, title in ((axes[0],names,"Mapping passes"),(axes[1],post,"Deferred preparation")):
        values = [group["phases_ms"].get(n,{}).get("median",0)/1000 for n in items]
        ax.barh(range(len(items)), values, color=COLORS[0 if ax is axes[0] else 1])
        ax.set(yticks=range(len(items)),yticklabels=[n.replace('mapping_','').replace('post_','').replace('_',' ') for n in items],xlabel="Median seconds",title=title)
        ax.invert_yaxis()
        for i,v in enumerate(values): ax.text(v+.03,i,f'{v*1000:.3f} ms' if 0<v<.001 else f'{v:.3f}',va='center',fontsize=9)
    fig.tight_layout(); return save(fig,output,"phase-budgets")


def figure_resources(record, output, name="resources"):
    start = record["load_request_ms"]; samples=record["samples"]; gpu=record["gpu"]["samples"]
    fig, axes=plt.subplots(3,1,figsize=(12,8),sharex=True)
    for field,label,color in (("working_set","CPU working set",COLORS[0]),("private_bytes","Private commit",COLORS[1])):
        axes[0].plot([(s['time_ms']-start)/1000 for s in samples],[s[field]/2**30 for s in samples],label=label,color=color)
    axes[0].plot([(s['time_ms']-start)/1000 for s in gpu],[s['memory_used']/2**30 for s in gpu],label="Device-wide GPU memory",color=COLORS[2])
    axes[0].set_ylabel('GiB'); axes[0].legend(loc='upper right',ncol=3)
    axes[1].plot([(s['time_ms']-start)/1000 for s in gpu],[s['gpu_percent'] for s in gpu],label='Device-wide GPU busy',color=COLORS[0])
    axes[1].set_ylabel('GPU busy %')
    axes[2].plot([(s['time_ms']-start)/1000 for s in gpu],[s['sm_clock_MHz'] for s in gpu],label='SM clock MHz',color=COLORS[2])
    power=axes[2].twinx(); power.plot([(s['time_ms']-start)/1000 for s in gpu],[s['power_W'] for s in gpu],color=COLORS[1],label='GPU power W')
    axes[2].set(ylabel='SM clock MHz',xlabel='Seconds after load request'); power.set_ylabel('GPU power W')
    axes[0].set_title('One instrumented hidden run · process memory and NVML device samples')
    fig.tight_layout(); return save(fig,output,name)


def counter_table(hardware):
    rows=[]
    for stage in hardware.get('stages',[]):
        counts=stage['counts']; samples=counts.get('076:00',{}).get('samples',0)
        retired=counts.get('0c0:00',{}).get('estimated_events',0)
        rows.append([stage['name'],f"{stage['wall_ms']/1000:.3f}",samples,f"{retired/1e9:.3f}",
                     f"{stage['ipc_estimate']:.2f}" if stage['ipc_estimate'] is not None else '—',
                     f"{stage.get('caller_ipc_estimate'):.2f}" if stage.get('caller_ipc_estimate') is not None else '—',
                     f"{stage['branch_mispredict_percent_estimate']:.3f}" if stage['branch_mispredict_percent_estimate'] is not None else '—'])
    return table(['Profile phase','Profile wall s','Cycle samples','Estimated retired instructions B','All-thread IPC','Caller IPC','Branch misses %'],rows)


def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('root',type=Path); parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); root=args.root.resolve(); output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    native=summarize_native(root)
    deep={p.parent.name:summarize_run(p) for p in (root/'deep').glob('*/result.json')}
    hardware={case:json.loads((root/'deep'/case/'cpu-analysis.json').read_text()) for case in ('branch','cache','fp') if (root/'deep'/case/'cpu-analysis.json').exists()}
    example_path=next(Path(r['path']) for r in native['runs'] if r['configuration']=='traced-hidden')
    example=json.loads(example_path.read_text())
    evidence=all_evidence(root)
    detailed=detailed_sections(evidence,native,hardware,example,output,table,save)
    figures=[figure_native(native,output),figure_timeline(example,output),figure_phase_budget(native['groups']['traced-hidden'],output),figure_resources(example,output)]
    timechart=[]
    timechart_path=root/'deep/timechart/timechart/timechart.csv'
    if timechart_path.exists():
        record=json.loads((root/'deep/timechart/result.json').read_text()); timechart=timechart_series(timechart_path,record)
        fig,axes=plt.subplots(2,1,figsize=(12,5.5),sharex=True)
        axes[0].plot([x['time_ms']/1000 for x in timechart],[x['mean_effective_frequency_MHz'] for x in timechart],color=COLORS[0]); axes[0].set(ylabel='Mean across 16 logical CPUs, MHz',title='AMD Timechart · effective CPU frequency and package power')
        axes[1].plot([x['time_ms']/1000 for x in timechart],[x['package_power_W'] for x in timechart],color=COLORS[1]); axes[1].set(ylabel='CPU package power W',xlabel='Seconds after observer launch')
        fig.tight_layout(); figures.append(save(fig,output,'cpu-frequency-power'))
    memory=deep.get('memory',{}).get('memory_maps',[])
    memory_rows=[[m['name'],f"{m['private_bytes']/2**30:.3f}",f"{m['working_set_bytes']/2**30:.3f}"] for m in memory]
    features=deep.get('production-features',deep.get('features',{})).get('feature_captures',[])
    feature_rows=[[c['name'],c['render']['frames'],f"{c['render'].get('frames_per_second',0):.1f}",f"{c['render']['gpu_frame_ms'].get('median',0):.3f}",f"{c['render']['gpu_frame_ms'].get('p95',0):.3f}",c['render']['points']['submittedCount'].get('max',0)] for c in features]
    native_rows=[]
    for name,g in native['groups'].items():
        native_rows.append([name,3,f"{g['initial_cpu_preparation_ms']['median']/1000:.3f}",f"{g['initial_upload_ms']['median']/1000:.3f}",f"{g['load_rpc_ms']['median']/1000:.3f}",f"{g['load_rpc_ms']['min']/1000:.3f}–{g['load_rpc_ms']['max']/1000:.3f}"])
    phase_rows=[[name,f"{d['median']/1000:.6f}",f"{d['min']/1000:.6f}–{d['max']/1000:.6f}"] for name,d in native['groups']['traced-hidden']['phases_ms'].items() if 'worker' not in name]
    initial=native['groups']['production-hidden']; traced=native['groups']['traced-hidden']
    feature_transitions=table(['View transition','Frames','GPU median ms','GPU P95 ms','Point submissions summed over frames','Image colors','Non-background %'],[
        [c['name'],c['transition']['frames'],f"{c['transition']['gpu_frame_ms'].get('median',0):.3f}",
         f"{c['transition']['gpu_frame_ms'].get('p95',0):.3f}",f"{c['transition']['point_submission_total']:,}",
         c['image']['unique_colors'],f"{c['image']['non_background_fraction']*100:.3f}"] for c in features])
    feature_images=[]
    for name in ('solid','solid_edges','solid_points','points','edges'):
        source=root/'deep/production-features'/('feature-'+name+'.png')
        if source.exists():
            shutil.copy2(source,output/source.name)
            feature_images.append(f'<a href="{source.name}">{name.replace("_"," ")}</a>')
    navigation=deep.get('production-features',{}).get('navigation',{})
    navigation_table=table(['Camera orbit frames','Frames/s','Median GPU ms','P95 GPU ms','CPU frame P99 ms','Dropped records'],[[
        navigation.get('frames',0),f"{navigation.get('frames_per_second',0):.1f}",
        f"{navigation.get('gpu_frame_ms',{}).get('median',0):.3f}",f"{navigation.get('gpu_frame_ms',{}).get('p95',0):.3f}",
        f"{navigation.get('cpu_frame_ms',{}).get('p99',0):.3f}",navigation.get('dropped_frames',0)]])
    summary={'provenance':json.loads((root/'provenance.json').read_text()),'native':native,'deep':deep,'hardware':hardware,'timechart':timechart,'evidence':evidence}
    for name in ('parser','reads','copy'):
        path=root/'controls'/(name+'.json')
        if path.exists(): summary[name]=json.loads(path.read_text())
    summary_path=output/'summary.json'; summary_path.write_text(json.dumps(summary,indent=2),encoding='utf-8')
    extra_sections=[]
    if 'parser' in summary:
        rows=[]
        for workers in (1,2,4,8,16):
            records=[r for r in summary['parser']['records'] if r['workers_requested']==workers]
            values=[next(p['wall_ms'] for p in r['events'] if p['name']=='parser_total')/1000 for r in records]
            peak=[max(p['process_peak_working_set'] for p in r['events'])/2**30 for r in records]
            rows.append([workers,f'{statistics.median(values):.3f}',f'{min(values):.3f}–{max(values):.3f}',f'{statistics.median(peak):.3f}'])
        extra_sections.append('<h2>Parser scaling</h2>'+table(['Workers','Median parser s','Range s','Peak working set GiB'],rows)+'<p>Each process reads the warmed file. Parser output fingerprints must match. Validation hashing follows the timed parser interval.</p>')
    if 'reads' in summary:
        rows=[]
        for mode in ('read-cached','read-native'):
            for workers in (1,8,16):
                records=[r for r in summary['reads']['records'] if r['mode']==mode and r['workers_requested']==workers]
                values=[next(p['wall_ms'] for p in r['events'] if p['name']=='read_only')/1000 for r in records]
                rows.append([mode,workers,f'{statistics.median(values):.3f}',f'{7108991827/statistics.median(values)/1e9:.3f}'])
        extra_sections.append('<h2>Read controls</h2>'+table(['Control','Workers','Median s','Payload GB/s'],rows)+'<p>Native reads bypass the Windows file cache. They are a device-read control. They do not measure a cold production load. No system cache was flushed.</p>')
    sources='''<p class="small">Methods: <a href="https://learn.microsoft.com/en-us/windows/win32/perfctrs/about-performance-counters">Windows performance counters</a> · <a href="https://docs.amd.com/r/en-US/57368-uProf-user-guide/Hardware-Sources">AMD PMCs</a> · <a href="https://docs.amd.com/r/en-US/57368-uProf-user-guide/Using-CLI-to-Profile">AMD Timechart</a> · <a href="https://docs.nvidia.com/nsight-systems/UserGuide/index.html">Nsight Systems</a> · <a href="https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlDeviceQueries.html">NVML queries</a>.</p>'''
    content=f'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>BearTrap end-to-end · 14 October 2026</title>
<style>html{{scroll-behavior:smooth}}body{{margin:0;background:#f1f4f3;color:#203436;font:16px/1.6 Segoe UI,Arial,sans-serif}}main{{max-width:1160px;margin:32px auto;background:white;padding:36px 44px}}h1{{font-size:34px;line-height:1.2}}h2{{margin-top:36px;font-size:23px}}.lead{{font-size:21px}}.note{{border-left:4px solid #176f76;padding:12px 18px;background:#edf5f4}}.small,figcaption{{font-size:13px;color:#576d70}}table{{border-collapse:collapse;width:100%;font-size:14px;margin:16px 0 24px}}td,th{{padding:9px 12px;border-bottom:1px solid #dbe3e1;text-align:left;font-variant-numeric:tabular-nums}}th{{background:#eaf1ef}}img{{width:100%;height:auto}}figure{{margin:24px 0}}a{{color:#176f76}}code{{font:13px Consolas,monospace}}.scroll{{overflow:auto}}li{{margin:10px 0}}details{{margin:20px 0}}summary{{font-weight:600}}@media(max-width:700px){{main{{margin:0;padding:20px}}h1{{font-size:28px}}}}</style><main>
<p class="small">14 October 2026 · woby 0.29.0 · revision <code>7ee6133eb133b2102551ebd47a3ecca263e86cce</code></p>
<h1>BearTrap: first frame and full readiness are different endpoints</h1>
<p class="lead">The first model frame completes on the GPU at about {traced['first_frame_gpu_ms']['median']/1000:.2f} s. The production hidden viewer returns the completed load at {initial['load_rpc_ms']['median']/1000:.2f} s.</p>
<p>The remaining work is source topology, original-coordinate bounds, point preparation, and annotations. The scene can render while this worker runs. The load RPC waits for its completion.</p>
<p class="note">Native viewer controls establish end-to-end times. Instrumented copies provide phase boundaries. Hardware profiles are separate diagnostic runs. A completed GPU frame does not measure the time when a person sees pixels on a display.</p>
<p><a href="#native">Native controls</a> · <a href="#phases">Phases</a> · <a href="#hardware">Hardware</a> · <a href="#memory">Memory</a> · <a href="#render">Rendering</a> · <a href="#next">Next actions</a> · <a href="#evidence">Evidence</a></p>
{detailed['commits']}
<h2 id="native">Native controls and endpoint definitions</h2>
{table(['Configuration','Runs','Initial CPU s','Initial upload s','Load RPC s','Load RPC range s'],native_rows)}
{detailed['endpoints']}
<p>Three rotating rounds use fresh Release processes and a warmed 7,108,991,827-byte input. The fixed drawable is 1280×720. The model viewport is 805×687 with 4× MSAA. Visible controls stayed focused in their settled recordings.</p>
<p>The initial upload contains 2,138,524,344 bytes. Its timer includes buffer creation, staging, submission, and GPU completion. It is not a PCIe-only transfer timer. The old research fast-pacing viewer also logs upload polls. A separate control removes those logs.</p>
<p>The hidden trace median for full readiness is about 4.4% above its native control; the visible trace median is about 3.1% above its native control. Markers and generated code can affect timing. Phase times locate work; native controls establish end-to-end behavior.</p>
{figures[0]}
{detailed['pacing']}
<h2 id="phases">Phase boundaries and remaining work</h2>{figures[1]}{figures[2]}
<p>Whole-group bounds reuse is active. The three hidden traces reuse one complete group and scan zero partial-group vertices. Its median work is 0.015 ms. The separate original-coordinate whole-mesh pass still calls the mesh accessor for each render vertex.</p>
<p>Mapping resolution is serial. Allocation and index initialization have separate intervals. Eight gather workers write to disjoint resolved IDs. Worker intervals overlap; their elapsed times must not be added to the mapping total.</p>
<details><summary>Every measured phase · three instrumented hidden runs</summary>{table(['Phase','Median s','Range s'],phase_rows)}</details>
<details><summary>Worker timeline and CPU time</summary>{detailed['workers']}</details>
<h2 id="hardware">CPU hardware counters and independent profiles</h2>
<p>AMD uProf collects cycles, retired instructions, branch events, demand cache fills, DTLB events, and floating-point events. Each event set fits the available counters. FP events require a paired merge counter. User mode is enabled; OS events are excluded.</p>
<p>Phase selection uses absolute QPC timestamps and start-inclusive, end-exclusive windows. A +16 ms window shift measures alignment sensitivity. Ratios omit phases shorter than 100 ms. Parent phases overlap their children and must not be added.</p>
{counter_table(hardware.get('branch',{}))}
{detailed['counters']}
<p>Instruction-location samples can skid. They describe observed code locations, not exact dynamic opcode counts or SIMD lane use. Demand cache fills exclude traffic such as prefetch and writes. They do not measure physical DRAM bandwidth.</p>
<p>The production executable has its own uProf capture and linker-map function resolution. VTune supplies an independent software-sampling profile with call stacks and thread summaries. Nsight NVTX ranges show worker overlap. WDDM records add allocation, memory-transfer, and presentation evidence.</p>
<details><summary>Function and thread profiles</summary>{detailed['functions']}</details>
<details><summary>Instruction samples and SIMD</summary>{detailed['instructions']}</details>
{figures[4] if len(figures)>4 else ''}
<p>Timechart reports effective frequency for 16 logical CPUs and package/core power at 100 ms intervals. These are system measurements. Windows diagnostic counters run at about one-second intervals. CPU temperature is not provided by the supported Timechart categories.</p>
<details><summary>CPU frequency and power by phase</summary>{detailed['power']}</details>
{detailed['wddm']}
<h2 id="memory">Memory use and release</h2>{figures[3]}
{table(['Checkpoint','Private commit GiB','Working set GiB'],memory_rows)}
<p>Each checkpoint uses VirtualQueryEx and QueryWorkingSetEx. Page-validity queries cover committed allocations of at least 1 MiB. They do not touch or lock target memory. A scan is not atomic and is outside native timing controls.</p>
<p>Private commit, CPU working set, and GPU memory are separate quantities. Write-combined committed ranges can explain part of their difference. Region size and protection do not identify the allocator owner. WDDM allocation records provide separate driver evidence.</p>
<p>Three load/remove cycles return to approximately 0.38 GiB private commit. The small increase between cycles is retained in the data. This checks observed release; it does not prove long-term leak freedom.</p>
<details><summary>Allocation protection, page validity, and data sizes</summary>{detailed['memory']}</details>
<h2 id="render">Rendering after preparation</h2>
{detailed['native_render']}
{table(['View','Frames','Frames/s','Median GPU ms','P95 GPU ms','Maximum newly submitted points/frame'],feature_rows)}
{feature_transitions}
<p>Feature images: {' · '.join(feature_images)}. Each image uses the separate 1920×1800 capture. Feature timing uses the 805×687 viewport.</p>
<p>Point rendering accumulates results. Once full refinement is complete, a fixed camera can submit zero new points while displaying the stored result. Its settled FPS does not measure full-source point throughput. Transition captures record refinement separately.</p>
<p>The solid-to-points transition submits 42,414,381 point entries across frames. This includes progressive work and is not a count of distinct source points. The final refined count is 38,414,391. The points-only image contains visible geometry; the zero settled submission count does not mean a blank image.</p>
<p>Edges-only rendering uses a depth occluder before its edge draw. Its cost must be compared with the matching view. Screenshots verify solid, combined, point-only, and edge-only output. Camera navigation has a separate recording.</p>
{navigation_table}<p>The navigation control sends 24 orbit commands, each with 3° yaw. Pitch is +0.5° for the first 12 commands and −0.5° for the next 12. Each command is followed by a 100 ms delay. It records frame behavior during motion. It does not measure input-to-photon latency.</p>
<p>NVML samples GPU busy time, memory, clocks, power, temperature, PCIe RX/TX, and clock-event reasons. Values are device-wide. PCIe samples cover the API's short interval and cannot reconstruct an exact upload byte total. The constant memory-utilization field is excluded from bandwidth claims.</p>
{''.join(extra_sections)}
<h2 id="upload-controls">Isolated upload and copy controls</h2>{detailed['controls']}
<h2 id="next">Next actions supported by the measurements</h2>
<ol><li><strong>Separate feature readiness.</strong> Publish bounds and required point data before annotation work. Build optional caches when their tools need them. Keep saved-annotation and point-cloud requirements.</li>
<li><strong>Improve deferred bounds access.</strong> Select position storage once. Retain index and finite-coordinate validation. The current whole-mesh deferred pass takes about 1.50 s despite the earlier eager-path SIMD work. Preserve the referenced-vertex set; unreferenced source positions must not change the bounds.</li>
<li><strong>Reduce source-index work.</strong> Materialization adds 867.14 MiB and about 1.99 s. Test delayed construction for topology tools or a validated bulk mapping pass.</li>
<li><strong>Target serial resolution and parser memory.</strong> The ID loop takes about 4.50 s. Attribute gathering is already much shorter. The parser's simultaneous chunk and merged data remain the peak-memory target.</li>
<li><strong>Keep present upload pacing.</strong> Initial upload is below one second in the standard control. The log-free 1 ms control is slower in all three pairs. The isolated capacity sweep favors small overlap buffers; it does not establish an end-to-end capacity change.</li>
<li><strong>Test a prepared binary cache for repeat loads.</strong> Preserve double source coordinates, ordered IDs, groups, UV validity, and source-change detection. Measure creation separately from reuse.</li></ol>
<h2 id="evidence">Evidence, limits, and reproduction</h2>
{detailed['environment']}
<details><summary>Windows and GPU telemetry examples</summary>{detailed['telemetry']}</details>
<p>WPR rejects CPU tracing with 0xc5585011. xperf rejects the requested kernel flags. Nsight Vulkan tracing cannot register its extension without registry-writing permission. Nsight Graphics connects, then reports unavailable GPU performance counters. No profiling policy or counter-access setting was changed.</p>
<p>IBS, L3, DF, and UMC sources are unavailable in the uProf inventory. Physical DRAM bandwidth and exact dynamic instruction counts remain unmeasured. Intel SDE is not installed. GPU hardware pipeline counters remain unavailable.</p>
<p>The production source stays at the recorded revision. Generated copies add phase markers and diagnostic counters. A log-free pacing copy changes only inactive pacing. Builds use <code>vs2026-vcpkg</code> in Release mode. The application and measurement targets build without warnings.</p>
<p>Validation: 110 default Release tests, 8 slow tests, and 26 graphics tests pass. Eight measurement tests cover phase nesting, thread ownership, endpoint order, trace integrity, scheduling, counter thread-ID domains, WDDM ownership/pairing, and VTune export parsing. Report text has no tests. A later preset-only rerun selected Debug and was stopped; the corrected final rerun uses explicit Release.</p>
<p><a href="summary.json">Complete numerical summary</a> · <a href="measurements.zip">Portable raw records and source archive</a> · <a href="beartrap.png">Verified instrumented screenshot</a> · <a href="REPRODUCE.txt">Reproduction commands</a> · <a href="../../../experiments/obj-profile/e2e_run.py">Viewer runner</a> · <a href="../../../experiments/obj-profile/e2e_generate.py">Instrumentation generator</a> · <a href="../../../experiments/obj-profile/e2e_hardware.py">Hardware analysis</a> · <a href="../obj-beartrap-20261011/report.html">Original investigation</a></p>
<p class="small">Large native profiler sessions and matching executables remain in <code>build/e2e-beartrap-20261014</code>. The portable archive contains numeric records, commands, source snapshots, and diagnostics.</p>{sources}</main></html>'''
    (output/'report.html').write_text(content,encoding='utf-8')
    shutil.copy2(example_path.parent/'beartrap.png',output/'beartrap.png')
    with zipfile.ZipFile(output/'measurements.zip','w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for path in root.rglob('*'):
            if path.is_file() and path.suffix in ('.json','.jsonl','.csv','.log') and 'matching-bin' not in path.parts:
                archive.write(path,'records/'+str(path.relative_to(root)).replace('\\','/'))
        repo=Path(__file__).resolve().parents[2]
        for directory in (repo/'experiments/obj-profile',repo/'src'):
            for path in directory.rglob('*'):
                if path.is_file() and path.suffix in ('.py','.cpp','.h','.hpp','.txt','.md'):
                    archive.write(path,'sources/'+str(path.relative_to(repo)).replace('\\','/'))
        for name in ('CMakeLists.txt','CMakePresets.json','vcpkg.json','AGENTS.md'):
            archive.write(repo/name,'sources/'+name)
        for directory in ('e2e-viewer','upload-viewer'):
            generated=repo/'build/vs2026-vcpkg/experiments/obj-profile'/directory
            for path in generated.rglob('*'):
                if path.is_file(): archive.write(path,'generated/'+directory+'/'+str(path.relative_to(generated)).replace('\\','/'))
        asm=repo/'build/vs2026-vcpkg/experiments/obj-profile/woby_obj_profile_sse2.dir/Release/main.asm'
        if asm.exists(): archive.write(asm,'assembly/sse2-main.asm')
        if (output/'REPRODUCE.txt').exists(): archive.write(output/'REPRODUCE.txt','REPRODUCE.txt')
    print(json.dumps({'report':str(output/'report.html'),'summary_bytes':summary_path.stat().st_size,'archive_bytes':(output/'measurements.zip').stat().st_size}),flush=True)


if __name__=='__main__': main()
