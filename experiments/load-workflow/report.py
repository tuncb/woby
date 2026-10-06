# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib>=3.9", "numpy>=2"]
# ///
"""Create a standalone HTML report and exportable figures from validated results."""
import argparse
import csv
import html
import io
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter
import numpy as np

import analyze

NAMES = {'legacy':'Legacy', 'prototype_copy':'Prototype + copy', 'prototype':'Direct prototype'}
COLORS = {'legacy':'#697585', 'prototype_copy':'#d59736', 'prototype':'#1479b8'}
SHORT = {'small_batch':'200 small files','mixed':'Weighted + curve','quads':'250k quads','lines':'Lines + points',
         'trimmed':'Trimmed surfaces','bus':'BusGameMap','cloud':'10M points','bennu':'Bennu','powerplant':'Powerplant'}
READ = ['capacity_preflight','parse','legacy_freeform_reparse','freeform_resolve']
MESH = ['mesh_validate','source_adoption','coordinate_localization','triangulation','mesh_mapping','normals',
        'mesh_bounds','freeform_tessellation_append']
PREP = ['gpu_input_validation','point_membership','point_hierarchy']
ANNOTATION = ['annotation_blocks_fingerprints','annotation_spatial_index','annotation_snapshot_copy']


def esc(value): return html.escape(str(value))
def med(row, group, key): return row[group][key]['median']
def mib(value): return value/1048576
def number(value): return f'{value:,.1f}'
def integer(value): return f'{value:,}'
def duration(value):
    if 0 < value < .1: return f'{value*1000:,.1f} µs'
    return f'{value:,.1f} ms' if value<1000 else f'{value/1000:,.3f} s'
def estimate(value): return f'{mib(value):,.1f} MiB'
def interval(value, scale=1): return f"{value['median']/scale:,.2f} <small>({value['minimum']/scale:,.2f}–{value['maximum']/scale:,.2f})</small>"


def table(headers, rows):
    return '<div class="table-wrap"><table><thead><tr>'+''.join('<th>'+esc(x)+'</th>' for x in headers)+\
        '</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+str(x)+'</td>' for x in row)+'</tr>' for row in rows)+'</tbody></table></div>'


def findings(models):
    by_id={m['workload']['id']:m for m in models}
    if not all(name in by_id for name in ('mixed','cloud','small_batch','quads','bennu','powerplant')):
        return ''
    def value(model, variant, group, key):
        return med(by_id[model]['variants'][variant],group,key)
    def timing(model,variant,key): return value(model,variant,'timings',key)
    mixed_ratio=timing('mixed','legacy','fully_prepared_ms')/timing('mixed','prototype','fully_prepared_ms')
    cloud_copy=value('cloud','prototype_copy','memory','copied_position_bytes')
    cloud_peak_legacy=value('cloud','legacy','memory','import_peak_commit_bytes')
    cloud_peak_direct=value('cloud','prototype','memory','import_peak_commit_bytes')
    quad_regression=(timing('quads','prototype','first_submitted_ms')/timing('quads','legacy','first_submitted_ms')-1)*100
    ordinary=', '.join(f"{SHORT[name]} {timing(name,'legacy','first_submitted_ms')/timing(name,'prototype','first_submitted_ms'):.2f}×"
                       for name in ('cloud','bennu','powerplant'))
    return f'''<section><h2>The measured answer</h2>
<p><strong>Direct ownership is feasible and removes real work, but it does not by itself make the complete workflow substantially faster for ordinary large models.</strong> The strong exception is mixed weighted/freeform input, where the unified grammar removes the legacy reread/reparse fallback. The larger remaining opportunities are in Woby’s derived representations and preparation schedule.</p>
<ul><li><strong>Mixed grammar is the clear win.</strong> Scene-and-annotation preparation falls from {duration(timing('mixed','legacy','fully_prepared_ms'))} to {duration(timing('mixed','prototype','fully_prepared_ms'))}: {mixed_ratio:.2f}× faster. This primarily comes from avoiding the legacy fallback, rather than just moving a coordinate vector.</li>
<li><strong>The large ordinary models remain close overall.</strong> First-scene legacy/direct ratios are {ordinary}. These medians and their ranges do not establish a broad speedup. The quad-grid first scene is {quad_regression:.1f}% slower in the direct-path median; its parse stage is {duration(value('quads','prototype','phases','parse'))} versus {duration(value('quads','legacy','phases','parse'))} for legacy. The prototype is therefore not an across-the-board replacement on performance grounds.</li>
<li><strong>The position copy is measurable; its end-to-end effect is small.</strong> Restoring the copy on the point cloud adds {estimate(cloud_copy)} of position payload and takes {duration(value('cloud','prototype_copy','phases','source_adoption'))} in the adoption stage. The move takes {duration(value('cloud','prototype','phases','source_adoption'))}. Nevertheless, import peak commitment is {estimate(cloud_peak_legacy)} for legacy and {estimate(cloud_peak_direct)} for direct ownership. Later preparation establishes the whole-process peak, so the earlier CPU-only benchmark’s roughly 229 MiB saving does not survive as a workflow peak reduction.</li>
<li><strong>Small-file readiness is dominated by scheduling after the first view.</strong> The direct path submits the 200-file scene at {duration(timing('small_batch','prototype','first_submitted_ms'))}, but finishes annotation publication at {duration(timing('small_batch','prototype','fully_prepared_ms'))}. The annotation tail after commit is {duration(timing('small_batch','prototype','annotation_publish_after_commit_ms'))}; the measured annotation CPU stages total only {duration(sum(value('small_batch','prototype','phases',p) for p in ANNOTATION))}. One background job is published at a time on successive UI frames.</li>
<li><strong>Preparation deserves its own optimization budget.</strong> The point-cloud hierarchy takes {duration(value('cloud','prototype','phases','point_hierarchy'))}. Bennu’s first scene arrives at {duration(timing('bennu','prototype','first_submitted_ms'))}, while scene-and-annotation preparation ends at {duration(timing('bennu','prototype','fully_prepared_ms'))}. Faster parsing cannot directly remove these later stages.</li></ul>
<p>All variants produce identical geometry counts, major retained capacities, upload payloads, and decoded verification-image pixels for every workload and repeat. That is strong evidence for this corpus, not an exhaustive proof of OBJ semantic equivalence. The separate parser tests and ordered CPU geometry fingerprints provide additional coverage.</p></section>'''


def figure(fig, output, name, caption):
    fig.savefig(output/(name+'.svg'),bbox_inches='tight')
    fig.savefig(output/(name+'.png'),bbox_inches='tight',dpi=180)
    stream=io.StringIO()
    fig.savefig(stream,format='svg',bbox_inches='tight')
    svg=stream.getvalue()
    plt.close(fig)
    return '<figure>'+svg[svg.index('<svg'):]+f'<figcaption>{esc(caption)}</figcaption></figure>'


def endpoint_plot(models, output):
    fig,axes=plt.subplots(1,2,figsize=(12,7.2),sharey=True)
    for ax,key,title in zip(axes,['first_submitted_ms','fully_prepared_ms'],['First scene frame submitted','Scene and annotation preparation finished']):
        for offset,variant in zip([-.23,0,.23],NAMES):
            rows=[m['variants'][variant]['timings'][key] for m in models]
            x=np.array([r['median']/1000 for r in rows])
            lo=np.array([r['minimum']/1000 for r in rows])
            hi=np.array([r['maximum']/1000 for r in rows])
            ax.errorbar(x,np.arange(len(models))+offset,xerr=[x-lo,hi-x],fmt='o',markersize=5,
                        capsize=3,color=COLORS[variant],label=NAMES[variant],linewidth=1.4)
        ax.set_xscale('log')
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v,_:f'{v:g} s'))
        ax.set_title(title,loc='left',pad=16,fontweight='bold',fontsize=11)
        ax.grid(axis='x',alpha=.18)
        ax.set_xlabel('Elapsed wall time · logarithmic axis')
    axes[0].set_yticks(np.arange(len(models)),[SHORT[m['workload']['id']] for m in models])
    axes[0].invert_yaxis()
    axes[0].legend(loc='upper left',bbox_to_anchor=(0,-.13),ncol=3,frameon=False)
    fig.tight_layout(w_pad=3)
    return figure(fig,output,'workflow-times','Dots are medians of three independent processes; whiskers are min–max, not confidence intervals. The right endpoint includes background annotation publication. Point refinement and verified export are reported separately.')


def phase_plot(models, output):
    groups=[('Read / parse',READ,'#1479b8'),('Mesh construction',MESH,'#42a9a2'),
            ('UI group bounds',['ui_group_preparation'],'#85b85b'),('Point / upload preparation',PREP,'#d59736'),
            ('Annotation preparation',ANNOTATION,'#aa7aad')]
    fig,axes=plt.subplots(1,2,figsize=(12,7.4),sharey=True)
    for ax,variant in zip(axes,['legacy','prototype']):
        totals=np.array([sum(med(m['variants'][variant],'phases',p) for _,phases,_ in groups for p in phases) for m in models])
        left=np.zeros(len(models))
        for label,phases,color in groups:
            part=np.array([sum(med(m['variants'][variant],'phases',p) for p in phases) for m in models])
            width=part/np.maximum(totals,.00001)*100
            ax.barh(np.arange(len(models)),width,left=left,color=color,height=.65,label=label)
            left+=width
        for y,value in enumerate(totals): ax.text(102,y,f'{value/1000:.2f}s',va='center',fontsize=8)
        ax.set_xlim(0,116)
        ax.set_xticks([0,25,50,75,100],['0%','25%','50%','75%','100%'])
        ax.set_title(NAMES[variant],loc='left',fontweight='bold')
        ax.set_xlabel('Share of measured CPU-stage elapsed time')
        ax.grid(axis='x',alpha=.12)
    axes[0].set_yticks(np.arange(len(models)),[SHORT[m['workload']['id']] for m in models])
    axes[0].invert_yaxis()
    handles,labels=axes[0].get_legend_handles_labels()
    fig.legend(handles,labels,loc='lower center',ncol=3,frameon=False,bbox_to_anchor=(.53,.01))
    fig.tight_layout(w_pad=2,rect=(0,.12,1,1))
    return figure(fig,output,'cpu-phases','Phase timers measure elapsed time on the executing CPU thread, including scheduling. They are not hardware CPU-cycle counters. Work can overlap, particularly small-file prefetch; sums are not the request’s critical-path time. Median phase values are summarized independently.')


def memory_plot(models,output):
    fig,ax=plt.subplots(figsize=(11,6.5))
    for offset,variant in zip([-.23,0,.23],NAMES):
        rows=[m['variants'][variant]['memory']['import_peak_commit_bytes'] for m in models]
        x=np.array([mib(r['median']) for r in rows])
        lo=np.array([mib(r['minimum']) for r in rows])
        hi=np.array([mib(r['maximum']) for r in rows])
        ax.errorbar(x,np.arange(len(models))+offset,xerr=[x-lo,hi-x],fmt='o',capsize=3,color=COLORS[variant],label=NAMES[variant])
    ax.set_yticks(np.arange(len(models)),[SHORT[m['workload']['id']] for m in models])
    ax.invert_yaxis()
    ax.set_xlabel('Peak process commitment through import / background preparation (MiB)')
    ax.grid(axis='x',alpha=.2)
    ax.legend(frameon=False,ncol=3,loc='upper left',bbox_to_anchor=(0,-.12))
    fig.tight_layout()
    return figure(fig,output,'workflow-memory','Windows process peak commitment includes graphics initialization, CPU geometry, caches, and driver allocations. It is cumulative from process start, sampled before verification export. It is neither GPU residency nor just the parser’s allocation peak.')


def cloud_timeline(data, output):
    fig,ax=plt.subplots(figsize=(11,4.8))
    for variant in NAMES:
        selected=next(r for r in data['runs'] if r['workload']=='cloud' and r['variant']==variant)
        raw=json.loads((Path(selected['source'])/'run.json').read_text(encoding='utf-8'))
        rows=[r for r in raw['memory_samples'] if r['stage'] in ('load','background_preparation')]
        ax.plot([(r['ms']-raw['load_started_ms'])/1000 for r in rows],
                [mib(r['private_bytes']) for r in rows],color=COLORS[variant],label=NAMES[variant],linewidth=1.6)
    ax.set_xlabel('Seconds since the import request')
    ax.set_ylabel('Live private memory (MiB)')
    ax.grid(alpha=.18)
    ax.legend(frameon=False)
    fig.tight_layout()
    return figure(fig,output,'cloud-memory-timeline','Representative first-round traces, sampled approximately every 20 ms. These show allocation lifetimes; they are not medians, and very brief peaks can fall between samples. The separate process peak counters retain the OS-reported high-water mark.')


def report(data, output):
    output.mkdir(parents=True,exist_ok=True)
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'svg.fonttype':'none',
                         'axes.spines.top':False,'axes.spines.right':False,'axes.edgecolor':'#ccd3dc',
                         'text.color':'#24334b','axes.labelcolor':'#43536c','xtick.color':'#43536c','ytick.color':'#43536c'})
    models=data['comparisons']
    first_ratios=[med(m['variants']['legacy'],'timings','first_submitted_ms')/med(m['variants']['prototype'],'timings','first_submitted_ms') for m in models]
    slowest=max(models,key=lambda m:med(m['variants']['prototype'],'timings','fully_prepared_ms'))
    largest=max(models,key=lambda m:med(m['variants']['prototype'],'memory','import_peak_commit_bytes'))
    parts=[]
    parts.append(f'''<header><div class="eyebrow">WOBY · ISSUE 112 · MEASURED ON 2026-10-05</div>
<h1>From OBJ bytes<br>to a usable scene</h1><p class="deck">A complete workflow comparison of the legacy loader, the fixed RapidOBJ prototype, and a controlled extra-copy variant.</p>
<div class="badges"><span>{len(data['runs'])} matched viewer runs</span><span>{len(models)} model workloads</span><span>Release · Vulkan · RTX 3070 Laptop</span><span>3 runs per comparison</span></div></header>
<nav><a href="#results">Results</a><a href="#stages">Preparation stages</a><a href="#ownership">Direct ownership</a><a href="#memory">Memory</a><a href="#models">Model details</a><a href="#next">Next steps</a><a href="#method">Method &amp; limits</a></nav>
<main><section><h2>What this comparison answers</h2>
<p class="note">This report describes its recorded campaign. See the <a href="../parser-throughput-performance/README.md">parser regression investigation</a> for subsequent fixes and measurements.</p>
<p>The earlier CPU benchmark stopped after constructing a <code>Mesh</code>. This campaign runs the actual desktop viewer, with its background loader, UI group construction, point hierarchy, staged uploads, scene publication, annotation preparation, adaptive point refinement, screenshot readback, and steady rendering. The production renderer and geometry algorithms are used in every variant.</p>
<p>The direct path reaches <code>SourceMeshData::points</code>: RapidOBJ merges into a caller-owned <code>vector&lt;array&lt;double,3&gt;&gt;</code>, Woby localizes that allocation, and transfers its ownership. Woby subsequently constructs render vertices, mapped precise positions, primitive indices, group bounds, point data and annotation caches. Their costs remain visible in this report.</p>
<div class="callout"><strong>Keep the endpoints separate.</strong> “First scene” means a submitted scene frame, not photons on a display. “Preparation finished” includes background annotation publication. Full point detail is polled separately. A full-detail PNG verifies actual GPU rendering after preparation; its completion time includes readback and encoding.</div></section>''')
    parts.append(findings(models))
    parts.append('<section id="results"><h2>Workflow results</h2>')
    parts.append(endpoint_plot(models,output))
    rows=[]
    for m in models:
        legacy=m['variants']['legacy']; direct=m['variants']['prototype']
        ratio=med(legacy,'timings','first_submitted_ms')/med(direct,'timings','first_submitted_ms')
        rows.append([esc(m['workload']['label']),interval(legacy['timings']['first_submitted_ms']),
                     interval(direct['timings']['first_submitted_ms']),f'{ratio:.2f}×',
                     interval(legacy['timings']['fully_prepared_ms']),interval(direct['timings']['fully_prepared_ms'])])
    parts.append(table(['Workload','Legacy first scene, ms','Direct first scene, ms','Legacy/direct','Legacy prepared, ms','Direct prepared, ms'],rows))
    parts.append(f'<p>First-scene ratios span {min(first_ratios):.2f}× to {max(first_ratios):.2f}× across these workloads. A ratio above 1 favors the prototype; below 1 favors legacy. Small differences with overlapping run ranges are not evidence of a reliable improvement. The longest direct-path preparation in this corpus is {esc(slowest["workload"]["label"])} at {duration(med(slowest["variants"]["prototype"],"timings","fully_prepared_ms"))}.</p></section>')
    parts.append('<section id="stages"><h2>Where the preparation time goes</h2>'+phase_plot(models,output))
    parts.append('''<ol><li><strong>Read, decode and merge.</strong> The parser reads OBJ/MTL data and reconciles worker output. Legacy may fail on weighted/freeform syntax, reread the file, reconstruct polygon text and parse it again. The prototype emits numeric freeform records in the initial pass.</li>
<li><strong>Build Woby geometry.</strong> Positions are adopted or copied, recentered, and triangulated. Stable first-use vertex IDs are assigned to position/normal/UV tuples. Woby builds packed render vertices and mapped double positions, expands lines and points, checks/generates normals, computes bounds, and tessellates freeform patches.</li>
<li><strong>Prepare the scene.</strong> UI groups obtain their local and original bounds. Render preparation validates indices, constructs per-group point membership, and builds a point hierarchy for eligible large meshes. The hierarchy condition is size-based and also applies to ordinary triangle meshes.</li>
<li><strong>Upload and publish.</strong> The UI thread stages at most 4 MiB per upload step and 16 MiB per frame, with a roughly 4 ms per-frame CPU budget. The scene is published when the batch’s buffers are staged. These budgets make elapsed upload time dependent on frame cadence.</li>
<li><strong>Finish background work.</strong> Annotation preparation builds block fingerprints, a spatial index and an owned snapshot. Jobs publish on subsequent UI frames. Point rendering can additionally refine a coarse initial view to all source points.</li></ol>''')
    rows=[]
    for m in models:
        r=m['variants']['prototype']
        rows.append([esc(SHORT[m['workload']['id']]),estimate(m['preparation_capacities'].get('upload_bytes',0)),
                     duration(med(r,'timings','upload_cpu_ms')),duration(med(r,'timings','gpu_finalize_wall_ms')),
                     duration(med(r,'timings','annotation_publish_after_commit_ms')),
                     duration(med(r,'timings','full_detail_observed_ms'))])
    parts.append(table(['Workload','GPU upload payload','CPU upload calls','Upload/finalize wall','Annotation tail after commit','Full point detail observed*'],rows))
    parts.append('<p class="note">*For scenes without active adaptive points this records the first post-load observation, not a point-refinement cost. CPU upload-call time includes allocation/copy/driver waits. Upload/finalize wall time also includes frame scheduling and publication; neither is a standalone DMA bandwidth measurement.</p></section>')
    if all(name in {m['workload']['id'] for m in models} for name in ('cloud','bennu','powerplant','lines')):
        by_id={m['workload']['id']:m for m in models}
        parts.append('<section><h2>Why the bottleneck changes with the model</h2>')
        rows=[]
        for name in ('lines','cloud','bennu','powerplant'):
            m=by_id[name]; l=m['variants']['legacy']; d=m['variants']['prototype']
            rows.append([esc(SHORT[name]),duration(med(l,'phases','parse')),duration(med(d,'phases','parse')),
                         duration(med(d,'phases','mesh_mapping')),duration(med(d,'phases','ui_group_preparation')),
                         duration(med(d,'phases','point_hierarchy')),duration(med(d,'phases','annotation_spatial_index'))])
        parts.append(table(['Workload','Legacy parse','Direct parse','Direct mapping','Direct group bounds','Direct point hierarchy','Direct annotation index'],rows))
        def phase(name,variant,key): return duration(med(by_id[name]['variants'][variant],'phases',key))
        def snapshot(name):
            a=by_id[name]['annotation_capacities']
            return estimate(a['snapshot_vertex_bytes']+a['snapshot_index_bytes'])
        line_change=(med(by_id['lines']['variants']['prototype'],'timings','first_submitted_ms')/med(by_id['lines']['variants']['legacy'],'timings','first_submitted_ms')-1)*100
        plant=by_id['powerplant']
        parts.append(f'''<p>The line fixture exposes a remaining parser regression: {phase('lines','legacy','parse')} becomes {phase('lines','prototype','parse')}, and first-scene time rises by {line_change:.1f}%. The point-cloud parser also remains slower ({phase('cloud','legacy','parse')} → {phase('cloud','prototype','parse')}), while avoiding the legacy position copy and coordinate writeback recovers much of that time later. The exact low-level cause of these remaining decoder differences needs a separate parser profile; these phase timers locate the regression but do not prove its instruction-level cause.</p>
<p>Powerplant expands {plant['mesh_capacities']['source_positions']/1e6:.2f} million source positions into {plant['geometry']['vertexCount']/1e6:.2f} million render vertices at attribute seams. Its mapping phase takes {phase('powerplant','prototype','mesh_mapping')}. Bennu spends {phase('bennu','prototype','ui_group_preparation')} constructing UI group bounds and {phase('bennu','prototype','annotation_spatial_index')} constructing the annotation spatial index. Both ordinary triangle models also build a substantial point hierarchy before the first solid view.</p>
<p>Snapshot vertex/index payloads are {snapshot('bennu')} for Bennu and {snapshot('powerplant')} for Powerplant; copying those snapshots takes {phase('bennu','prototype','annotation_snapshot_copy')} and {phase('powerplant','prototype','annotation_snapshot_copy')} respectively. Sharing ownership targets those allocations and copies, but it does not eliminate the much larger spatial-index computation. Each model family therefore needs a different optimization.</p></section>''')
    parts.append('<section id="ownership"><h2>What direct parsing into Woby structures saves</h2><p>The extra-copy control uses the same prototype decoder, double UV/normal precision, worker pipeline, mesh builder and renderer as the direct prototype. It replaces the position-vector move with a copy and retains the original vector until mesh construction finishes. This isolates one ownership decision. It does not emulate every part of the legacy implementation.</p>')
    rows=[]
    for m in models:
        c=m['variants']['prototype_copy']; d=m['variants']['prototype']
        rows.append([esc(SHORT[m['workload']['id']]),estimate(med(c,'memory','copied_position_bytes')),
                     interval(c['phases']['source_adoption']),interval(d['phases']['source_adoption']),
                     duration(med(c,'timings','first_submitted_ms')),duration(med(d,'timings','first_submitted_ms')),
                     estimate(med(c,'memory','source_adoption_private_bytes')-med(d,'memory','source_adoption_private_bytes'))])
    parts.append(table(['Workload','Position copy restored','Copy adoption, ms','Direct adoption, ms','Copy first scene','Direct first scene','Live private delta at adoption*'],rows))
    parts.append('''<p class="note">*Difference of per-variant medians. This includes other live process allocations and is not an exact allocation counter. The restored payload is exact: 24 bytes per parsed position. Whole-workflow differences can be much smaller than this temporary allocation, and timing noise can reverse small differences.</p>
<div class="flow"><div><b>Worker records</b><span>Parallel decode<br>Temporary geometry chunks</span></div><i>→</i><div><b>Caller-owned coordinates</b><span>Typed merge destination<br>24 bytes / source position</span></div><i>→</i><div class="accent"><b>SourceMeshData</b><span>Localize in place<br>Move ownership</span></div><i>→</i><div><b>Derived representations</b><span>Render &amp; precise vertices<br>Hierarchy, snapshot, upload</span></div></div>
<p>Parallel worker chunks still have to be merged. Final GPU vertices require additional information: referenced positions, tuple deduplication at UV/normal seams, stable vertex ordering, a coordinate origin, triangulation, normal generation and sometimes freeform tessellation. Parsing into final packed vertices in one step would require preserving these semantics and coordinating global IDs across workers.</p>
<p>The legacy path additionally writes localized coordinates back into its parser position pool. The direct path localizes the adopted typed allocation once. The prototype’s UV and normal pools use double precision, whereas the legacy pools use float; that changes temporary attribute storage. The extra-copy control keeps prototype precision and decoding unchanged, so its result should not be mistaken for a complete emulation of legacy memory behavior.</p>
<p>The current change removes a temporary coordinate copy. Shared immutable geometry ownership would reach further: annotation consumers could retain the published asset instead of copying it, and position-only models could use a specialized representation. These are separate architectural changes; the measurements here do not implement or claim their speedups.</p></section>''')
    parts.append('<section id="memory"><h2>Temporary savings versus the whole-process peak</h2>'+memory_plot(models,output))
    if any(m['workload']['id']=='cloud' for m in models): parts.append(cloud_timeline(data,output))
    rows=[]
    for m in models:
        c=m['mesh_capacities']; p=m['preparation_capacities']; a=m['annotation_capacities']
        source=sum(c.get(k,0) for k in ('source_position_bytes','source_index_bytes','source_id_bytes'))
        indices=sum(c.get(k,0) for k in ('triangle_index_bytes','line_index_bytes','point_index_bytes'))
        cloud=sum(v for k,v in p.items() if k.startswith('cloud_') and k.endswith('_bytes'))
        annotations=sum(a.values())
        rows.append([esc(SHORT[m['workload']['id']]),estimate(source),estimate(c.get('render_vertex_bytes',0)),
                     estimate(c.get('precise_position_bytes',0)),estimate(indices),estimate(cloud),estimate(annotations)])
    parts.append(table(['Workload','Source data','Packed vertices','Mapped double positions','Render indices','Point hierarchy/data','Annotation cache/snapshot'],rows))
    parts.append(f'''<p>These are capacities of major retained vectors, and they match across all readers. They exclude allocator metadata, small object/string allocations, UI state, freeform control/grid containers and driver memory. Shared annotation spatial data is counted once; its copied block array is counted separately. The largest measured direct-path import peak is {estimate(med(largest['variants']['prototype'],'memory','import_peak_commit_bytes'))} for {esc(largest['workload']['label'])}.</p>
<p>A parser saving reduces the application’s peak only if that allocation overlaps the phase that establishes the maximum. Later hierarchy construction, snapshot creation or GPU staging can set a larger peak. Live-memory traces and stage endpoints are therefore more informative than comparing parser peaks alone.</p></section>''')
    parts.append('<section><h2>Visualization after loading</h2><p>The final geometry, retained capacities and full-detail image pixels match across readers. Rendering measurements are a control: ownership of an earlier CPU parser buffer should not change the final draw workload.</p>')
    rows=[]
    for m in models:
        values=[]
        for variant in ('legacy','prototype'):
            row=m['variants'][variant]['render']
            for key in ('steady_gpu_ms','navigation_gpu_ms','navigation_frame_interval_ms'):
                values.append(interval(row[key]) if row[key] else 'No sample')
        rows.append([esc(SHORT[m['workload']['id']]),*values])
    parts.append(table(['Workload','Legacy cached GPU, ms','Legacy moving GPU, ms','Legacy moving frame, ms','Direct cached GPU, ms','Direct moving GPU, ms','Direct moving frame, ms'],rows))
    parts.append('''<p>Cached view timings follow one second of stationary recording. Navigation repeats 48 deterministic camera updates through twelve angular offsets. Adaptive points remain enabled; navigation may use a subset while the stationary view refines to full detail. GPU timestamp results are asynchronous and can repeat between completed queries. Frame intervals include pacing and command delivery, so they are not uncapped FPS claims.</p></section>''')
    parts.append('<section><h2>From process startup to a verified rendered image</h2><p>Startup is timed separately from import. The observed-ready endpoint waits for completed background preparation and stationary full point detail, with polling and a minimum 100 ms post-load settling interval. It is an operational upper bound, not an exact first-ready timestamp. PNG completion additionally includes the offscreen render, readback and encoding.</p>')
    rows=[]
    for m in models:
        l=m['variants']['legacy']; d=m['variants']['prototype']
        rows.append([esc(SHORT[m['workload']['id']]),duration(med(d,'timings','startup_ms')),
                     duration(med(l,'timings','preparation_settled_observed_ms')),duration(med(d,'timings','preparation_settled_observed_ms')),
                     duration(med(l,'timings','verified_image_ms')),duration(med(d,'timings','verified_image_ms')),
                     duration(med(d,'timings','capture_ms'))])
    parts.append(table(['Workload','Direct startup','Legacy all-ready observed*','Direct all-ready observed*','Legacy verified PNG*','Direct verified PNG*','Direct capture alone'],rows))
    parts.append('<p class="note">*Elapsed from the import request, excluding process startup and deliberate input warming. Screenshot export is a verification step rather than a prerequisite for interactive viewing. The roughly half-second capture overhead on small scenes belongs to this endpoint, not to OBJ parsing.</p></section>')
    parts.append('<section id="models"><h2>Model-by-model evidence</h2><p>Open a model for its measured stages. Each value is the median and observed range in milliseconds.</p>')
    descriptions={
        'small_batch':'This exercises the application’s bounded small-file prefetch, ordered publication, many UI groups and serial annotation-job scheduling. Aggregate parser work can overlap and must not be added to batch wall time.',
        'mixed':'Weighted positions and a rational curve exercise legacy fallback rereading and polygon-text reconstruction. The prototype resolves numeric freeform records without that fallback. Tessellation and annotation work remain downstream.',
        'quads':'This isolates triangulation and generated normals on an indexed grid. A final vertex writer still needs to preserve triangulation order, winding, precise coordinates and normal accumulation.',
        'lines':'Polyline expansion and explicit points exercise different primitive indices from triangles. Large point membership can still cause hierarchy preparation even though no annotation triangle snapshot is scheduled.',
        'trimmed':'Small analytic input expands into tessellated render geometry. Trimming, surface evaluation and append operations dominate; textual parsing is only a small part of this workload.',
        'bus':'A medium real scene exposes attribute handling, group preparation and the fixed costs of staging and publication. It is a useful check against conclusions drawn only from large synthetic inputs.',
        'cloud':'Every source position becomes a displayed point. There are no face seams or triangulation costs, but the current Mesh representation still retains packed vertices and mapped double positions before building compact point data.',
        'bennu':'A large triangle mesh without a UV/normal seam requirement exercises the position-only vertex map, generated normals, group bounds, point hierarchy and annotation spatial indexing.',
        'powerplant':'Authored attribute tuples split render vertices at seams. This increases packed vertices, mapped positions, point data and annotation snapshots relative to the source position pool; direct coordinate ownership alone cannot eliminate those representations.'}
    for m in models:
        identity=m['workload']['id']; d=m['variants']['prototype']; c=m['variants']['prototype_copy']
        raw_positions=int(med(c,'memory','copied_position_bytes')/24)
        g=m['geometry']
        top=sorted(((name,med(d,'phases',name)) for name in analyze.PHASES),key=lambda item:item[1],reverse=True)[:3]
        parts.append(f'<details><summary>{esc(m["workload"]["label"])} <span>{duration(med(d,"timings","first_submitted_ms"))} to first scene</span></summary><div class="detail"><p>{esc(descriptions[identity])}</p>')
        parts.append(table(['Input','Parsed positions','Render vertices','Triangles','Lines','Explicit/display points','Groups'],[[estimate(m['workload']['input_bytes']),integer(raw_positions),integer(g['vertexCount']),integer(g['triangleCount']),integer(g['lineSegmentCount']),integer(g['pointCount']),integer(g['groupCount'])]]))
        parts.append('<p>The largest measured CPU phases in the direct path are '+', '.join(f'<code>{esc(name)}</code> ({duration(value)})' for name,value in top)+'.</p>')
        rows=[[esc(name),*(interval(m['variants'][variant]['phases'][name]) for variant in NAMES)] for name in analyze.PHASES]
        parts.append(table(['CPU phase, ms',*NAMES.values()],rows))
        parts.append(f'<p class="note">Verified image SHA-256: <code>{m["image"]["sha256"]}</code>. Exact decoded RGB pixels match in every run.</p></div></details>')
    parts.append('</section><section id="next"><h2>What to optimize next</h2>')
    rows=[]
    for m in models:
        legacy=m['variants']['legacy']; total=med(legacy,'timings','fully_prepared_ms')
        parser=sum(med(legacy,'phases',name) for name in READ)
        bound='Not additive: prefetch overlaps' if m['geometry']['fileCount']>1 else f'{total/max(total-parser,.0001):.2f}×'
        rows.append([esc(SHORT[m['workload']['id']]),duration(parser),duration(total),bound])
    parts.append(table(['Workload','Legacy read/parse work','Legacy preparation endpoint','Ideal bound if parsing cost vanished'],rows))
    parts.append('''<p>This is an Amdahl-style serial bound using measured medians and unchanged downstream work, not a forecast. It ignores secondary cache/memory effects and cannot be applied to summed prefetched-file work. It shows when parser optimization alone has little remaining room.</p>
<ol><li><strong>Share immutable published geometry with annotation consumers.</strong> The measured snapshot-copy phase and retained snapshot arrays provide a concrete target. An owned shared asset can remove the extra vertex/index allocation while preserving worker lifetime and scene-version checks. The spatial index itself remains necessary.</li>
<li><strong>Batch tiny annotation jobs and profile large spatial-index construction.</strong> The 200-file tail is mostly scheduling rather than geometry work. Process multiple small jobs in an owned worker batch, then publish against unchanged input versions. For large meshes, profile radix-key generation, sorting and tree construction independently; removing snapshot copies alone will leave most annotation CPU work intact.</li>
<li><strong>Specialize point-cloud storage.</strong> A point-only model need not retain all attributes of a general triangle Vertex forever. Removing the 32-byte packed CPU vertex and 24-byte mapped-coordinate duplicate would remove 56 bytes per point from those two containers—about 534 MiB at 10 million points. This is a storage-design bound, not a measured implementation or a promised process-peak reduction.</li>
<li><strong>Use source-position indirection for seam-expanded precise coordinates.</strong> A 4-byte source ID can potentially replace each 24-byte mapped double coordinate when the coordinate frame is shared. That is a 20-byte-per-render-vertex payload opportunity, with changes required in Mesh readers, freeforms, coordinate operations and analysis consumers.</li>
<li><strong>Review when large triangle meshes need a point hierarchy.</strong> The current preparer constructs one above the size threshold even before point display is requested. Deferral could advance the first solid view, but marker/picking requirements, cancellation and publication must be audited before changing this behavior.</li>
<li><strong>Fuse compatible metadata work into construction.</strong> Source min/max, referenced group bounds and vertex mapping have different semantics. Reusing correct per-worker or per-group reductions can remove passes; bounds over all declared positions cannot silently replace bounds over referenced geometry.</li>
<li><strong>Treat upload scheduling separately from parser ownership.</strong> All readers upload the same payload. Faster parsing does not remove staging copies, the per-frame byte/time budgets or GPU resource creation. Any upload-budget experiment must measure UI responsiveness alongside first-view time.</li></ol>
<p>For general OBJ files, a staged direct writer is more plausible than a single-pass final-vertex writer: decode into owned typed pools, resolve global references and tuple IDs, assemble immutable render/source assets, then publish derived caches against that asset version. This preserves negative references, first-use ordering, seams, winding, large-coordinate precision and freeform control ownership.</p></section>''')
    parts.append('<section id="method"><h2>Method, scope and limitations</h2>')
    manifest=data['manifest']
    parts.append(table(['Setting','Value'],[
        ['Code baseline',esc(manifest['revision'])],['Executable SHA-256','<code>'+esc(manifest['executable_sha256'])+'</code>'],
        ['CPU','AMD Ryzen 7 5800H · 8 cores / 16 threads'],['System RAM',estimate(manifest['ram_bytes'])],
        ['GPU','NVIDIA GeForce RTX 3070 Laptop · 8 GiB · driver 616.92'],
        ['Renderer / build','NoGraphicsAPI Vulkan · Release · Visual Studio 2026 preset'],
        ['Window','Hidden desktop, 1280 × 720 drawable; scene viewport excludes the top UI strip'],
        ['Pacing','Benchmark forces the foreground branch while keeping the window hidden; actual presentation visibility is unknown'],
        ['Repetitions','Three fresh processes per reader/workload; reader order rotates once per round'],
        ['Storage cache','OBJ input warmed immediately before each import; this is not a cold-disk study'],
        ['Model policy','Default group modes: solid triangles, explicit points visible, lines preserved; helpers and side panes hidden'],
        ['Comparison gates','Exact geometry counts, retained major-vector capacities, GPU-preparation payload and decoded screenshot pixels'],
        ['Validation','Warning-free Debug and Release builds; 954 CTest checks passed; 10 comparison-analysis unit tests'],
        ['Interference',f'{len(data["excluded"])} recorded attempts excluded; active build/test/viewer jobs are guarded and affected attempts retried']]))
    pacing=models[0]['variants']['prototype']['pacing']
    parts.append(f'''<p>The recorded foreground pacing target is {pacing['targetFps']:g} frames/s on this display configuration. Woby’s normal hidden-window branch uses 20 frames/s. Because upload work is budgeted per frame, using that background cadence for the primary experiment would measure a different loading policy. The benchmark override changes only the window-activity decision in an instrumented copy of the graphics adapter; it applies equally to all readers. This is a controlled desktop-workflow comparison, not a measurement of physical display latency.</p>
<p>One application executable serves all three readers, so renderer, compiler, dependencies and UI work are identical. Exact source hashes, input hashes, environment observations, raw frames, per-stage events and memory samples are retained with each campaign. Instrumentation is limited to stage boundaries, writes its JSON at shutdown, and does not scan geometry to hash it in the loading path. Its overhead is most relevant to very small files.</p>
<p>Annotation readiness and completed background snapshots are distinct. Small models can be considered ready for inline snapshots before every background job has run. Large triangle-free models schedule no annotation snapshot; their annotation-ready flag is recorded but is not used to block a rendering measurement. A false flag on an otherwise rendered point/line model is a readiness-policy finding, not a failed parser or proof of an infinite parsing operation.</p>
<p>Point refinement is observed by RPC and therefore includes polling/frame granularity. The campaign waits for background jobs and stationary full detail before requesting its verification screenshot, so readback does not distort the initial loading path. The verified-image endpoint includes this wait, a 1920 × 1800 offscreen render, readback and PNG encoding. It is an upper bound for obtaining a checked image, not the earliest possible visible pixel.</p>
<p>Memory sampling is approximately 20 ms. Windows peak counters are cumulative over the process lifetime; private memory, working set, major-vector capacities and upload payloads answer different questions. GPU upload bytes are exact application payload, not measured VRAM residency. No separate cold-storage, operating-system I/O trace, allocation-stack profiler, DMA timestamp or device-residency experiment is claimed here.</p>
<p>Three repeats show run-to-run ranges, not statistical confidence. Laptop power/thermal behavior, the display compositor and unrelated system activity remain sources of noise even with build/test guards. The conclusions apply to this branch, hardware, corpus and warm-cache workflow. Other formats, cold starts, saved-scene restoration, detectors/analysis execution, editing/undo and every possible OBJ grammar combination are outside this campaign.</p>
<h3>Reproduce</h3><pre>cmake --preset vs2026-vcpkg -DWOBY_PROFILE_LOAD_WORKFLOW=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby
uv run experiments/load-workflow/run.py build/vs2026-vcpkg/bin/Release/woby.exe build/workflow-results --rounds 3
python experiments/load-workflow/analyze.py build/workflow-results
uv run experiments/load-workflow/report.py build/workflow-results

# Restore a normal viewer build afterward:
cmake --preset vs2026-vcpkg -DWOBY_PROFILE_LOAD_WORKFLOW=OFF</pre>
<p>Use a new output directory. The default real-model root is <code>D:/temp/obj_tests</code>; pass <code>--models</code> to change it. Synthetic inputs are generated beneath one unique temporary directory and cleaned after the campaign. The optional <code>--contract</code> run uses smaller generated inputs; <code>--no-guard</code> is for functional checks and is rejected as performance evidence by the analyzer.</p>
<h3>Source map</h3><p><code>src/obj_mesh.cpp</code>: parsing, adoption, localization, triangulation and mapping. <code>src/model_mesh.cpp</code>: normals and bounds. <code>src/background_load.cpp</code>: prefetch and the worker batch. <code>src/ui_state.cpp</code>: group bounds. <code>src/scene_mesh_preparation.cpp</code> and <code>src/point_cloud.cpp</code>: membership, hierarchy and compact points. <code>src/main.cpp</code> and <code>src/scene_renderer.cpp</code>: staged upload and publication. <code>src/surface_annotation.cpp</code> and <code>src/annotation_preparation.cpp</code>: spatial index, snapshot and background publication.</p>
<p>All measurement code lives in <code>experiments/load-workflow</code>. Its generator checks exact source anchors and fails on drift. Normal builds compile the original sources and use the unified RapidOBJ loader. The experiment provides the legacy comparison path.</p></section></main>
<footer>Woby load workflow analysis · 2026-10-05 · <a href="measurements.csv">Measurements CSV</a> · <a href="summary.json">Per-run summaries and provenance</a><br>Full raw events, frame samples, logs and verified images remain in the campaign directory.</footer>''')
    css='''*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:#f5f7fa;color:#233149;font:16px/1.6 "Segoe UI",Arial,sans-serif}header{background:#112a43;color:white;padding:64px max(5vw,24px) 50px}header h1{font-size:clamp(40px,6vw,68px);line-height:1.06;letter-spacing:-2px;margin:20px 0}.eyebrow{letter-spacing:2px;font-size:12px;color:#83c7e8;font-weight:700}.deck{max-width:800px;font-size:21px;color:#dce8f1}.badges{display:flex;flex-wrap:wrap;gap:10px;margin-top:25px}.badges span{border:1px solid #446077;border-radius:30px;padding:5px 14px;font-size:13px}nav{display:flex;gap:24px;flex-wrap:wrap;padding:18px 5vw;background:white;border-bottom:1px solid #dde4ed}nav a{color:#176da3;text-decoration:none;font-weight:600;font-size:14px}main{max-width:1280px;margin:auto;padding:25px 30px}section{background:white;padding:32px 38px;margin:24px 0;border:1px solid #e2e7ef;border-radius:12px;scroll-margin-top:20px}h2{font-size:29px;line-height:1.2;letter-spacing:-.5px;color:#143a59;margin:0 0 24px}h3{color:#143a59;font-size:20px}p{max-width:100ch}figure{margin:32px 0 26px}figure svg{width:100%;height:auto}figcaption,.note{font-size:13px;color:#5b697d;line-height:1.55}.callout{background:#edf6fc;border-left:4px solid #1479b8;padding:18px 24px;margin-top:25px}.table-wrap{overflow-x:auto;margin:24px 0}table{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums}th{text-align:left;color:#36536b;background:#edf2f7;border-bottom:2px solid #d5deea;padding:11px 12px;white-space:normal}td{padding:11px 12px;border-bottom:1px solid #e7ecf2;vertical-align:top}tr:nth-child(even) td{background:#fafbfd}td:first-child{font-weight:600;min-width:130px}td small{display:block;font-size:11px;white-space:nowrap;color:#778395}code{font-family:Consolas,monospace;font-size:.86em;overflow-wrap:anywhere;background:#edf2f7;border-radius:3px;padding:1px 4px}pre{background:#112a43;color:#e7f1f8;padding:22px;border-radius:8px;overflow-x:auto;font:13px/1.8 Consolas,monospace}li{margin:12px 0;padding-left:5px}details{border:1px solid #dbe3ed;border-radius:8px;margin:14px 0}summary{cursor:pointer;padding:18px;font-weight:650;color:#143a59;display:list-item}summary span{float:right;font-weight:400;color:#637389;font-size:13px}.detail{padding:0 22px 20px}.flow{display:flex;align-items:stretch;gap:10px;margin:28px 0}.flow div{flex:1;border:1px solid #cad9e4;border-radius:8px;padding:18px 12px;text-align:center;background:#f6f9fc}.flow .accent{background:#e4f4ef;border-color:#9ac9bb}.flow b{display:block;font-size:14px}.flow span{display:block;font-size:12px;color:#65758a;margin-top:8px}.flow i{align-self:center;color:#1479b8;font-size:23px}footer{padding:30px;text-align:center;font-size:13px;color:#69788b}@media(max-width:760px){main{padding:12px}section{padding:22px 18px}h2{font-size:24px}.flow{flex-direction:column}.flow i{transform:rotate(90deg)}summary span{float:none;display:block}nav{gap:14px}}@media print{body{background:white;font-size:11px}header{padding:25px;color:#112a43;background:white}header .deck{color:#344a5f}nav{display:none}section{break-inside:auto;border:0;padding:15px 0}figure{break-inside:avoid}main{padding:0}details{break-inside:avoid}summary{display:none}.detail{display:block}table{font-size:9px}h2{font-size:22px}}'''
    document='<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Woby · OBJ loading workflow performance</title><style>'+css+'</style><body>'+''.join(parts)+'</body></html>'
    (output/'index.html').write_text(document,encoding='utf-8')
    with (output/'measurements.csv').open('w',newline='',encoding='utf-8') as stream:
        writer=csv.writer(stream)
        writer.writerow(['workload','variant','metric','median','minimum','maximum','unit'])
        for m in models:
            for variant,row in m['variants'].items():
                for field,unit in [('timings','ms'),('phases','ms'),('memory','bytes')]:
                    for key,value in row[field].items():
                        writer.writerow([m['workload']['id'],variant,field+'.'+key,value['median'],value['minimum'],value['maximum'],unit])
                for key,value in row['render'].items():
                    if value:
                        writer.writerow([m['workload']['id'],variant,'render.'+key,value['median'],value['minimum'],value['maximum'],'ms'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    directory=args.directory.resolve()
    data=analyze.load(directory)
    (directory/'summary.json').write_text(json.dumps(data,indent=2),encoding='utf-8')
    output=(args.output or directory/'report').resolve()
    report(data,output)
    (output/'summary.json').write_text(json.dumps(data,indent=2),encoding='utf-8')


if __name__=='__main__':
    main()
