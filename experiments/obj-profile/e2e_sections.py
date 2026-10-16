"""Detailed report sections built from measured data, with explicit scope labels."""
import html
import statistics

import matplotlib.pyplot as plt
from e2e_analyze import phases, first_marks, distribution


def fmt(value, digits=3):
    return '—' if value is None else f'{value:,.{digits}f}'


def count(stage, event, caller=False):
    return stage.get('caller_counts' if caller else 'counts',{}).get(event,{}).get('estimated_events',0)


def short_function(name):
    name=name.split('(')[0].strip().rsplit(' ',1)[-1]
    return name if len(name)<=115 else name[:112]+'…'


def detailed_sections(evidence, native, hardware, example, output, table, save):
    sections={}; controls=evidence['controls']; pacing=evidence['pacing']
    paired=[]
    for round_number in (1,2,3):
        a=next(r for r in pacing if r['round']==round_number and r['configuration']=='production-hidden')
        b=next(r for r in pacing if r['round']==round_number and r['configuration']=='cleanfast-hidden')
        paired.append([round_number,fmt(a['load_rpc_ms']/1000),fmt(b['load_rpc_ms']/1000),
                       fmt((b['load_rpc_ms']-a['load_rpc_ms'])/1000),fmt(a['initial_upload_ms']/1000),fmt(b['initial_upload_ms']/1000)])
    sections['pacing']='<h3>Inactive pacing · separate paired batch</h3>'+table(
        ['Round','Production ready s','1 ms ready s','1 ms minus production s','Production upload s','1 ms upload s'],paired)+'''<p>The 1 ms copy uses the production main loop and has no upload-poll logging. It is slower in each pair. This supports keeping the present inactive pacing for this pipeline. The old result tested a different upload path.</p><p>This later batch is slower for both configurations than the first native batch. Its production median is 44.025 s; its 1 ms median is 47.258 s. The cause of the between-batch change is not established. Do not pool the batches or assign that difference to code changes.</p>'''
    endpoint_rows=[]; render_rows=[]
    for name in native['groups']:
        runs=[r for r in native['runs'] if r['configuration']==name]
        bounds=[r['native_scene_frame_cpu_complete_bounds_ms'] for r in runs if r['native_scene_frame_cpu_complete_bounds_ms']]
        endpoint_rows.append([name,fmt(statistics.median(b[0] for b in bounds)/1000) if bounds else '—',
                              fmt(statistics.median(b[1] for b in bounds)/1000) if bounds else '—',
                              fmt(native['groups'][name]['first_frame_gpu_ms'].get('median',0)/1000) if 'traced' in name else 'unmeasured',
                              fmt(native['groups'][name]['load_to_verified_screenshot_ms']['median']/1000)])
        render_rows.append([name,fmt(statistics.median(r['render']['frames_per_second'] for r in runs),1),
                            fmt(statistics.median(r['render']['gpu_frame_ms']['median'] for r in runs)),
                            fmt(statistics.median(r['render']['gpu_frame_ms']['p95'] for r in runs)),
                            sum(r['render']['dropped_frames'] for r in runs),
                            fmt(statistics.median(r['process']['peak_working_set']['max']/2**30 for r in runs)),
                            fmt(statistics.median(r['process']['peak_commit']['max']/2**30 for r in runs))])
    sections['endpoints']=table(['Configuration','Scene CPU frame lower s','Scene CPU frame upper s','Exact first scene GPU s','Verified screenshot s'],endpoint_rows)+'''<p>The native CPU-frame time is bracketed by the performance.begin RPC. A scene revision change selects the first scene frame. This is CPU frame completion, not GPU completion. The generated viewer adds a first-scene token and checks its GPU completion. Screenshot completion is a later verification endpoint and uses a separate 1920×1800 capture.</p>'''
    sections['native_render']=table(['Configuration','Settled frames/s','Median GPU ms','P95 GPU ms','Dropped records','Peak CPU WS GiB','Peak private commit GiB'],render_rows)+'''<p>Each settled interval lasts eight seconds. Hidden production pacing is 20 frames/s. Visible controls request 240 frames/s on a 120 Hz display but deliver about 60 frames/s with this scene. Render timing includes the current backend and presentation behavior; it is not a pure geometry kernel measurement.</p>'''
    diag=controls['mapping_diagnostic']; corners=controls['geometry']['triangles']*3
    sections['commits']='''<h2 id="commits">What the latest commits changed</h2>'''+table(['Commit','Implemented work','Evidence in this measurement'],[
        ['c68de54','Deferred preparation; whole-group bounds reuse','First model frame completes before point and annotation work. One whole group is reused; zero partial vertices are scanned.'],
        ['b83bd71','Bounded background upload ring','16 MiB staging; about 256 initial submissions. Initial upload takes less than one second in native standard controls.'],
        ['084f4dd','AVX2/NEON geometry preparation','x64 samples and disassembly show AVX2 rebase and bounds code. Deferred whole-mesh bounds still use the generic accessor.'],
        ['71c7e37','Automatic AVX-512 rebase dispatch','This Ryzen supports AVX2. The AVX-512 path is not exercised here.'],
        ['7ee6133','Staged ID resolution; exact vertex allocation','Render vertex capacity equals 38,414,391. Eight gather workers follow the serial resolution pass.']])+'''<p>These are observations of the current implementation. The prior report used an earlier revision and a different timing batch. This investigation does not claim a before/after speedup for each commit.</p><h3>Staged mapping operation counts</h3>'''+table(['Operation','Count','Share of triangle corners'],[[key,f'{value:,}',fmt(value/corners*100,3)+'%' if key in ('first_positions','primary_hits','secondary_lookups') else '—'] for key,value in diag.items()])+'''<p>First positions, primary hits, and secondary lookups partition all 227,315,958 corners. Secondary lookups are only 0.580% of corners. Most resolution work uses the primary path. The diagnostic build records exact operation counts; its extra increments make its wall time unsuitable as a native control.</p>'''
    worker_rows=[p for p in example['trace'] if p.get('name') in ('parser_worker_read_decode','mapping_primary_worker','mapping_seam_worker')]
    origin=first_marks(example['trace'])['model_add_requested']; fig,ax=plt.subplots(figsize=(12,max(8,len(worker_rows)*.24)))
    for i,p in enumerate(worker_rows):
        ax.barh(i,p['wall_ms']/1000,left=(p['start_ms']-origin)/1000,color='#176f76' if p['name'].startswith('parser') else '#ba653b',height=.7)
    ax.set(yticks=range(len(worker_rows)),yticklabels=[p['name'].replace('parser_worker_read_decode','parse').replace('mapping_primary_worker','primary').replace('mapping_seam_worker','seam')+' · T'+str(p['thread_id']) for p in worker_rows],xlabel='Seconds after model.add starts',title='Worker intervals · elapsed time overlaps across threads')
    ax.invert_yaxis(); fig.tight_layout(); sections['workers']=save(fig,output,'worker-timeline')
    work=[]
    for name in ('parser_scan_collect','parser_merge_execute','mapping_id_resolution','mapping_primary_gather','normal_face_accumulation','post_source_indices','post_original_bounds','point_morton_keys','point_radix_sort','annotation_morton_keys','annotation_radix_sort','annotation_tree_build'):
        values=[p for p in phases(example) if p['name']==name]
        if values: work.append([name,fmt(sum(p['wall_ms'] for p in values)/1000),fmt(sum(p['caller_cpu_ms'] for p in values)/1000),fmt(statistics.mean(p['average_logical_cpus'] for p in values),2)])
    sections['workers']+=table(['Phase in example','Wall s','Caller CPU s','Process CPU / wall'],work)+'''<p>Caller CPU time uses GetThreadTimes. Process CPU/wall includes concurrent renderer and worker activity. It can exceed one even for a serial phase. A parser parent mostly waits for its children; parent-thread IPC is not a parser throughput metric. Thread IDs and raw per-thread samples are preserved.</p>'''
    sections['workers']+=table(['Parser worker in branch profile','Wall s','CPU s','Read submit/wait s','Cycle samples','Estimated IPC'],[[
        w['thread_id'],fmt(w['wall_ms']/1000),fmt(w['caller_cpu_ms']/1000),fmt(w['io_submit_wait_ms']/1000),
        int(w.get('counts',{}).get('076:00',{}).get('samples',0)),fmt(w.get('ipc_estimate'),2)] for w in hardware['branch'].get('parser_worker_counters',[])])+'''<p>Read submit/wait time is measured around the worker's file API calls. It does not include decode time. Parser worker intervals are separate from the waiting parent. Their CPU times and counter samples must be evaluated per thread.</p>'''
    selected=['parser_scan_collect','parser_merge_execute','mapping_id_resolution','mapping_primary_gather','normal_face_accumulation','normal_vertex_normalize','post_source_indices','post_original_bounds','point_range_bounds','point_morton_keys','point_radix_sort','point_tree_build','annotation_blocks_fingerprints','annotation_bounds','annotation_fingerprints','annotation_morton_keys','annotation_radix_sort','annotation_tree_build']
    metrics=[]
    for name in selected:
        cache=next((s for s in hardware['cache']['stages'] if s['name']==name),None)
        branch=next((s for s in hardware['branch']['stages'] if s['name']==name),None)
        fp=next((s for s in hardware['fp']['stages'] if s['name']==name),None)
        if cache and fp and branch:
            dram=count(cache,'043:08'); retired=count(cache,'0c0:00')
            metrics.append([name,fmt(cache['wall_ms']/1000),fmt(count(cache,'043:01')/1e6),fmt(count(cache,'043:02')/1e6),
                            fmt(dram/1e6),int(cache['counts']['043:08']['samples']),fmt(dram/retired*1000) if retired else '—',
                            fmt(count(branch,'045:f0')/1e6),fmt(count(fp,'003:0f')/1e9)])
    sections['counters']='''<h3>Cache, TLB and FP estimates</h3>'''+table(['Phase','Cache profile s','Local L2 fills M','L3/sibling fills M','DRAM/IO demand fills M','DRAM samples','DRAM fills/1k instr.','L2 DTLB misses M','FP arithmetic B'],metrics)+'''<p>Each event column comes from its own event-set capture. Demand-fill events omit prefetch, stores, and other traffic. FP events include arithmetic in unused lanes and do not count useful geometry operations. Comparisons and blends are not FP arithmetic. Sparse zero samples do not prove that an event never occurs. Ratios are sample estimates, not exact dynamic counts.</p>'''
    calib=controls['fp_calibration']; observed=count(calib,'003:0f'); retired=count(calib,'0c0:00')
    sections['counters']+='<h3>Known-loop calibration</h3>'+table(['Quantity','Known loop','Sample × period','Relative error'],[
        ['FP arithmetic',f"{calib['expected_flops']:,}",f'{observed:,.0f}',fmt((observed/calib['expected_flops']-1)*100,2)+'%'],
        ['Retired loop instructions',f"{calib['expected_retired_loop_instructions']:,}",f'{retired:,.0f}',fmt((retired/calib['expected_retired_loop_instructions']-1)*100,2)+'%']])+f'''<p>The calibration interval is {calib['wall_ms']/1000:.3f} s. It verifies that the FP event responds to known arithmetic; the short interval exposes sampling and alignment error. It does not establish exact accuracy for every load phase. The full +16 ms alignment sensitivity is in summary.json.</p>'''
    shifted=calib['alignment_sensitivity_plus_16ms']
    sections['counters']+=table(['Calibration with +16 ms selection shift','Estimated events','Error against known loop'],[
        ['FP arithmetic',f"{shifted['003:0f']['shifted_estimated_events']:,.0f}",fmt((shifted['003:0f']['shifted_estimated_events']/calib['expected_flops']-1)*100,2)+'%'],
        ['Retired loop instructions',f"{shifted['0c0:00']['shifted_estimated_events']:,.0f}",fmt((shifted['0c0:00']['shifted_estimated_events']/calib['expected_retired_loop_instructions']-1)*100,2)+'%']])+'''<p>The shift brings the known loop closer to its expected counts. The main tables retain the QPC-metadata alignment and report the shifted selection as a sensitivity check. They do not assume that this short-loop correction is exact for every capture.</p>'''
    native_functions=evidence['production_functions']['functions']
    sections['functions']='''<h3>Production executable · load-window CPU samples</h3>'''+table(['Function resolved from matching linker map','Cycle samples','Share of all cycle samples %','Retired samples'],[[short_function(f['aliases'][0]),int(f['cycle_samples']),fmt(f['cycle_sample_percent'],2),int(f['retired_samples'])] for f in native_functions[:22]])+'''<p>This is the uninstrumented production executable. Function names use the matching linker map and the captured executable hash. Shares use the load window only. Map aliases and inlining limit attribution to an individual source expression.</p><h3>Independent VTune software profile</h3>'''+table(['Function','Sampled CPU s','Module','Source'],[[short_function(r['Function']),fmt(float(r['CPU Time'])),r['Module'],r['Source File']] for r in evidence['vtune_functions'][:22]])+table(['Thread','Sampled CPU s','Effective s','Spin s','Tasks'],[[r['Thread'],fmt(float(r['CPU Time'])),fmt(float(r['CPU Time:Effective Time'])),fmt(float(r['CPU Time:Spin Time'])),r.get('Task Count','')] for r in evidence['vtune_threads'][:22]])+'''<p>VTune uses user-mode software sampling and task markers. Its 62.249 s collection includes startup, load, verification, and settled rendering. Total sampled CPU time is 114.422 s over 81 threads. Its function shares cover a different window from the production uProf table. Thread CPU seconds can exceed wall seconds when workers overlap.</p>'''
    isa=controls['instructions']['sample_categories']
    sections['instructions']='''<h3>Instruction forms and SIMD limits</h3>'''+table(['Context-sampling phase','Decoded categories and sample counts'],[[phase,', '.join(f'{k}: {v}' for k,v in counts.items())] for phase,counts in isa.items() if phase!='geometry_validation'])+'''<p>The separate context sampler decodes 248 recorded instruction locations. It sees waiting threads as well as running threads and adds suspension overhead. Its load time is diagnostic. The hardware retired-event samples provide another biased location distribution. Both show AVX2 code where expected; neither reports exact lane utilization or dynamic SIMD counts.</p>'''
    observer=controls['instruction_observer']
    sections['instructions']+=table(['Context observer','Recorded value'],[[key,fmt(value) if isinstance(value,float) else value] for key,value in observer.items()])+'''<p>The SSE2 calibration listing has two packed multiply/add pairs, then subtract and branch, for each of 67,108,864 iterations. It has 402,653,184 loop instructions and 536,870,912 arithmetic operations. The matching assembly is in the source archive.</p>'''
    power=evidence['timechart_phase_power']
    sections['power']=table(['Timechart phase','Samples','Median mean-LP MHz','Range mean-LP MHz','Median CPU package W','Max CPU package W'],[[p['name'],p['samples'],fmt(p['mean_lp_frequency_MHz'].get('median'),1),fmt(p['mean_lp_frequency_MHz'].get('min'),1)+'–'+fmt(p['mean_lp_frequency_MHz'].get('max'),1),fmt(p['package_power_W'].get('median'),2),fmt(p['package_power_W'].get('max'),2)] for p in power])+'''<p>The frequency mean includes idle logical CPUs. It is not the frequency of the active model-loader core. Phase association uses the QPC epochs of the observer and trace. Windows CPU frequency stays at 1990 MHz in these records and cannot explain phase speed.</p>'''
    ns=[]
    for s in evidence['nsys']:
        ns.append([s['case'],s['nvtx_count'],s.get('etw_events_total','—'),s['target_events'],s['queue']['matched'],s['dma']['matched'],fmt(s.get('phase_alignment_max_error_ms'),4)])
    expanded=next(s for s in evidence['nsys'] if 'full' in s['case'])
    sections['wddm']='''<h2 id="wddm">Nsight and WDDM ownership</h2>'''+table(['Capture','NVTX ranges','System ETW events','Associated target events','Matched queue pairs','Matched DMA pairs','Max NVTX/phase start error ms'],ns)+table(['Driver transfer type · target-emitting process','Events','Bytes'],[[r['label'],r['count'],f"{r['bytes']:,}"] for r in expanded['transfer_rows']])+'''<p>Expanded WDDM tracing records more than one million system events. Target association uses either the emitting process or an explicit ProcessId/hProcessId payload. Memory ownership requires the payload ID. The GPU inventory contains a misleading duplicate adapter name; bus location and adapter LUID identify the physical NVIDIA adapter.</p>'''+table(['Queue evidence','Matched','Unmatched starts','Unmatched stops','Median ms','P95 ms','Max ms'],[[kind.upper(),expanded[kind]['matched'],expanded[kind]['unmatched_starts'],expanded[kind]['unmatched_stops'],fmt(expanded[kind]['duration_ms'].get('median')),fmt(expanded[kind]['duration_ms'].get('p95')),fmt(expanded[kind]['duration_ms'].get('max'))] for kind in ('queue','dma')])+'''<p>Queue pairs use GPU, context, queue-packet pointer, and submit sequence. Synchronization starts use an unknown packet-type value; the stop supplies the real type. Queue elapsed time includes scheduling and synchronization waits. It is not shader execution time. DMA pairs are a limited subset of copy-engine activity. Matched stops report no timeout or preemption.</p>'''
    transferphases=[p for p in expanded['phases'] if p['transfers'] or p['name'] in ('cpu_import','post_load_cpu_total')]
    sections['wddm']+=table(['Expanded trace phase','Profile s','Driver transfer bytes by type','Queue starts matched','Median queue duration ms'],[[p['name'],fmt(p['wall_ms']/1000),', '.join(k+': '+f"{v['bytes']:,}" for k,v in p['transfers'].items()),p['queue_packets'],fmt(p['queue_duration_ms'].get('median'))] for p in transferphases])+'''<p>The 3,139,715,072 driver-transfer bytes cover the full process, including setup, load, derived resources, and screenshot activity. They are not the initial mesh-upload payload or a direct PCIe measurement. Parent phase rows overlap children.</p><p>The expanded runner reached the requested endpoints and saved its trace, then hit the old 45 s profiler-export timeout. Offline export succeeded. Native wall-time conclusions exclude this attempt. The runner now allows 300 s for profiler exit and export.</p>'''
    sections['wddm']+=table(['Driver memory event · owning PID','Segment','Peak GiB','Final bytes after process exit'],[[m['event'],m['segment'],fmt(m['peak_bytes']/2**30),m['final_bytes']] for m in expanded['driver_memory']])
    fig,ax=plt.subplots(figsize=(11,4))
    for m in expanded['driver_memory']:
        if m['segment']=='Local': ax.step([v['time_ms']/1000 for v in m['samples']],[v['bytes']/2**30 for v in m['samples']],where='post',label=m['event'])
    ax.set(xlabel='Seconds after model.add starts',ylabel='Target local-segment GiB',title='WDDM driver memory · payload ProcessId matches the target');ax.legend();fig.tight_layout()
    sections['wddm']+=save(fig,output,'driver-memory')+'''<p>Local-segment usage peaks at 2.924 GiB. Local commitment peaks at 2.933 GiB. Both return to zero at process exit. This is a separate profiled run and a separate ownership measure from CPU working-set pages.</p>'''
    inventories=evidence['memory']; after=next(m for m in inventories if m['name']=='after_ready') if any(m['name']=='after_ready' for m in inventories) else inventories[1]
    sections['memory']=table(['Checkpoint','Scan ms','Write-combined commit GiB','Valid WC pages GiB','WC allocations'],[[m['name'],fmt(m['scan_ms'],1),fmt(m['write_combined_committed_bytes']/2**30),fmt(m['write_combined_valid_page_bytes']/2**30),m['write_combined_allocations']] for m in inventories])+table(['Largest allocation at first ready','Committed MiB','Valid page MiB','Protection'],[[hex(a['base']),fmt(a['committed_bytes']/2**20,2),fmt(a.get('valid_pages',0)*4096/2**20,2),', '.join(hex(p) for p in a['protections'])] for a in after['largest_allocations']])+'''<p>PAGE_WRITECOMBINE ranges account for much of the gap between private commit and CPU working set. Page validity does not measure GPU residency. Repeated 16/32 MiB ranges can be driver allocations; protection and size alone do not prove an owner.</p>'''
    sections['memory']+=table(['Known retained/temporary structure','Payload or recorded capacity MiB','Scope'],[
        ['Render vertices',fmt(controls['geometry']['vertex_bytes']/2**20,2),'38,414,391 × 32 bytes; exact capacity'],
        ['Render indices',fmt(controls['geometry']['index_bytes']/2**20,2),'227,315,958 × 4 bytes'],
        ['Source triangle indices',fmt(controls['geometry']['index_bytes']/2**20,2),'Deferred materialization'],
        ['Double source positions',fmt(37890645*24/2**20,2),'37,890,645 × 24 bytes'],
        ['Point data',fmt(614630256/2**20,2),'Measured capacity; ordered render vertices'],
        ['Point GPU upload',fmt(684707424/2**20,2),'Recorded payload; full points plus 4,379,823 proxy points'],
        ['Annotation triangle offsets',fmt(75771986*4/2**20,2),'One uint32 per triangle'],
        ['Annotation tree',fmt(170078688/2**20,2),'Recorded vector capacity; 2,097,151 active nodes'],
        ['Mapping primary table',fmt(454687740/2**20,2),'Temporary; released after mapping'],
        ['Parser triangle records',fmt(3409739370/2**20,2),'Released before vertex gather'],
        ['Decoded chunk data',fmt(controls['parser_stats']['parsed_chunk_bytes']/2**20,2),'Parser chunk inventory'],
        ['In-flight parser text',fmt(controls['parser_stats']['peak_inflight_text_bytes']/2**20,2),'Bounded 16 × 4 MiB text buffers']])+'''<p>These structures have different lifetimes. Do not add temporary and retained payloads to predict a process peak. Capacity includes allocator reservation in some rows. The parser's peak working set remains about 8.84 GiB with 16 workers.</p>'''
    uploads=controls['upload']; fig,ax=plt.subplots(figsize=(11,4))
    for mode,color in (('burst','#ba653b'),('overlap','#176f76')):
        rows=[r for r in uploads if r['mode']==mode]
        ax.plot([r['capacity_mib'] for r in rows],[r['upload_ms']['median'] for r in rows],marker='o',label=mode,color=color)
    ax.set(xscale='log',xlabel='Staging capacity MiB · log scale',ylabel='Median upload ms',title='512 MiB isolated upload · five measured rounds after warmup'); ax.legend(); fig.tight_layout()
    sections['controls']=save(fig,output,'upload-capacity')+table(['Mode','Capacity MiB','Median allocation ms','Median upload ms','Upload range ms','Submissions','Median waits'],[[r['mode'],r['capacity_mib'],fmt(r['allocation_ms']['median']),fmt(r['upload_ms']['median']),fmt(r['upload_ms']['min'])+'–'+fmt(r['upload_ms']['max']),int(r['submissions']['median']),fmt(r['waits']['median'],0)] for r in uploads])+f'''<p>All {controls['upload_verified_samples']} of {controls['upload_total_samples']} upload samples passed readback verification. Round zero is warmup; five rounds remain per cell. Overlap usually reduces waits. Large staging capacities reduce submissions but do not give the shortest upload time. This is an isolated 512 MiB control, not an end-to-end ring-capacity comparison.</p>'''
    copy=controls['copy'][0]
    sections['controls']+='<h3>Memory copy control</h3>'+table(['Control','Median ms','Payload GB/s','Minimum read+write GB/s'],[['512 MiB × 16 copies',fmt(copy['wall_ms']['median']),fmt(copy['payload_GB_per_second']['median']),fmt(copy['minimum_read_plus_write_GB_per_second']['median'])]])+'''<p>The buffers are allocated and initialized before the timed copy loop. Read+write is the minimum traffic implied by the copies; it is not a DRAM-controller measurement. Mapping and bounds have different locality and dependency costs.</p>'''
    sections['environment']='''<h3>Environment and run coverage</h3>'''+table(['Item','Recorded setup'],[
        ['CPU','AMD Ryzen 7 5800H; 8 cores, 16 logical CPUs; AVX2'],['Memory','68,564,348,928 bytes physical memory'],
        ['GPU','NVIDIA RTX 3070 Laptop; 8 GiB; driver 616.92'],['OS','Windows 11 build 26300; Hyper-V root partition; standard user'],
        ['Power','AC connected; battery 96%; Legion Balance mode'],['Tools','AMD uProf 5.3.521; VTune 2025.3; Nsight Systems 2026.5.1; Nsight Graphics 2026.3.1'],
        ['Controls','15 native full loads; 6 separate pacing full loads; 15 parser runs; 18 read runs; 3 copy runs; 84 verified upload samples'],
        ['Deep captures','Three CPU event sets; production CPU profile; VTune; NVTX; two WDDM levels; CPU power/frequency; three load/remove cycles; feature and camera recordings']])+'''<p>The process observer requests 100 ms intervals. Work and API calls make the observed intervals longer; actual cadence is retained for each run. Windows counters are sampled at about one-second intervals. GPU counters are device-wide unless the table names process dedicated/shared memory.</p>'''
    rows=[]
    for key,telemetry in list(evidence['native_telemetry'].items())[:5]:
        load=telemetry['load']; settled=telemetry['settled_render']; nv=settled['nvml']
        rows.append([key.rsplit(':',1)[-1],fmt(load.get('disk_read_bytes_per_second',{}).get('max',0)/1e6,1),fmt(load.get('memory_page_reads_per_second',{}).get('max',0),1),fmt(settled.get('gpu_process_dedicated_bytes',{}).get('median',0)/2**30),fmt(nv['power_W'].get('median'),1),fmt(nv['temperature_C'].get('median'),1),fmt(nv['sm_clock_MHz'].get('median'),0)])
    sections['telemetry']=table(['PID · example native runs','Max system disk read MB/s during load','Max system page reads/s','Settled process GPU dedicated GiB','Settled device W','Device °C','SM MHz'],rows)+'''<p>Disk and page-read counters cover the system and cannot prove per-process coldness. Soft page faults are not disk reads. The GPU Engine wildcard counter returns 0xc0000bba and is unavailable in these records; no per-engine utilization is claimed. Process GPU memory counters succeed. Device clock-event reason masks, PCIe RX/TX samples, and negotiated link generation/width are preserved in summary.json.</p>'''
    return sections
