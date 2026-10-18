"""Create a portable interactive overview from the exported measurements."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def distance_control(cases, names):
    """Combine complete, uncontaminated captures without averaging medians."""
    selected = {case["name"]:case for case in cases}
    events = []
    for name in names:
        case = selected.get(name)
        if not case or not case["capture_complete"] or case["compiler_overlap_observed"]:
            return None
        records = [s for s in case["records"] if s["operation"] == "distance"]
        if not records or any(s["status"] != "ok" for s in records):
            return None
        current = [e for e in case["events"] if e["name"] == "op.distance"]
        if not current:
            return None
        events.extend(current)
    if not events:
        return None
    values = [e["wall_ms"] for e in events]
    return {"cases":names,"runs":len(events),"median_ms":statistics.median(values),
        "min_ms":min(values),"max_ms":max(values),
        "median_cpu_ms":statistics.median(e["cpu_ms"] for e in events)}


def create(root):
    summary = read(root / "summary.json"); rows = summary["rows"]
    names = {"bus":"Bus","powerplant":"Powerplant","sanmiguel":"San Miguel","bennu":"Bennu","beartrap":"Bear Trap"}
    native = [{**r,"model":names[r["case"].removesuffix("-native")]} for r in rows
        if r["case"].removesuffix("-native") in names and r["case"].endswith("-native")]
    native.sort(key=lambda r:list(names.values()).index(r["model"]))
    hardware = read(root / "hardware-summary.json") if (root / "hardware-summary.json").exists() else []
    counters = []
    for op in ("auto","quality","intersections","distance"):
        item = {"operation":op}
        for case,key,label in (("bus-branch","ipc_estimate","ipc"),
                ("bus-branch","branch_mispredict_percent_estimate","branch"),
                ("bus-l2","l2_data_request_miss_percent_estimate","l2"),
                ("bus-tlb","dtlb_walk_share_of_l1_misses_percent_estimate","tlb")):
            values = [h[key] for h in hardware if h["case"] == case and h["stage"] == "op."+op and h.get(key) is not None]
            item[label] = statistics.mean(values) if values else None
        counters.append(item)
    queries = read(root / "queries.json") if (root / "queries.json").exists() else []
    query_rows = []
    for key,label in names.items():
        values = [v for v in queries if v["case"] == key+"-query-shift-.001"]
        if values:
            query_rows.append({"model":label,"stride":values[0]["batch_stride"],
                "visits":statistics.mean(v["mean_stack_visits"] for v in values),
                "triangles":statistics.mean(v["mean_triangle_tests"] for v in values),
                "maximum":max(v["maximum_stack_visits"] for v in values)})
    scaling = [r for r in rows if r["operation"] == "difference" and any(r["case"] == f"{shape}-{size}"
        for shape in ("grid","sphere") for size in (32,64,128,256,512,1024))]
    scaling.sort(key=lambda row:(row["case"].split("-")[0],row["triangles"]))
    workers = sorted((r for r in rows if r["case"].startswith("bus-workers-") and r["operation"] == "distance"),
        key=lambda r:int(r["case"].rsplit("-",1)[1]))
    tokens = []
    max3 = []
    batches = []
    for model in ("bus","sanmiguel"):
        for operation in ("auto","duplicate-points","duplicate-triangles","degenerates","topology"):
            selected = {int(r["case"].rsplit("-",1)[1]):r["median_ms"]/1000 for r in rows
                if r["case"].startswith(model+"-batches-") and r["operation"] == operation}
            if selected:
                batches.append({"model":names[model],"operation":operation,"seconds":selected})
    application = read(root / "application.json") if (root / "application.json").exists() else []
    full_batches = []
    for model,count,base_case in (("powerplant",21,"powerplant-isolated-detectors"),("sanmiguel",2203,"sanmiguel-batches-1")):
        base = next((r for r in rows if r["case"] == base_case and r["operation"] == "degenerates"),None)
        full = next((r for r in rows if r["case"] == model+"-all-batches-degenerates" and r["operation"] == "degenerates"),None)
        if base and full:
            full_batches.append({"model":names[model],"batches":count,"base":base["median_ms"]/1000,
                "full":full["median_ms"]/1000,"ratio":full["median_ms"]/base["median_ms"]})
    app_rows = []
    for operation in ("automatic_ready","quality_and_full_results","manual_intersections_ready","cached_results_query","identical_pair_full_results"):
        value = {"operation":operation}
        for mode in ("whole","per_volume"):
            selected = [r["wall_ms"]/1000 for r in application if r["analysis_mode"] == mode and r["name"] == operation]
            value[mode] = statistics.median(selected) if selected else None
        app_rows.append(value)
    for key,label in names.items():
        live = next((r for r in rows if r["case"] == key+"-token-live" and r["operation"] == "quality"),None)
        reference = next((r for r in rows if r["case"] == key+"-token-reference" and r["operation"] == "quality"),None)
        if live and reference:
            tokens.append({"model":label,"live":live,"reference":reference,
                "saving":100*(1-reference["median_ms"]/live["median_ms"])})
        if key == "bus":
            baseline = distance_control(summary["cases"],[f"bus-max3-clean-control-{i}" for i in range(4)])
            experimental = distance_control(summary["cases"],[f"bus-max3-clean-max3-{i}" for i in range(4)])
        else:
            baseline = distance_control(summary["cases"],[key+"-native"])
            experimental = distance_control(summary["cases"],[key+("-max3-clean" if key == "bennu" else "-max3")])
        if experimental and baseline:
            max3.append({"model":label,"baseline":baseline,"experimental":experimental,
                "saving":100*(1-experimental["median_ms"]/baseline["median_ms"])})
    data = {"captures":len(summary["cases"]),"native":native,"counters":counters,"queries":query_rows,
        "scaling":scaling,"workers":workers,"tokens":tokens,"max3":max3,"batches":batches,"full_batches":full_batches,"application":app_rows}
    fields = ("case","model","operation","triangles","median_ms","min_ms","max_ms","status","error",
        "intersection_truncated","runs","median_cpu_ms","median_average_logical_cpus")
    compact = lambda row: {key:row[key] for key in fields if key in row}
    for name in ("native","scaling","workers"):
        data[name] = [compact(row) for row in data[name]]
    for row in data["tokens"]:
        row["live"] = compact(row["live"]); row["reference"] = compact(row["reference"])
    html = TEMPLATE.replace("__DATA__",json.dumps(data,separators=(",",":")).replace("</","<\\/"))
    (root / "report.html").write_text(html,encoding="utf-8")
    print(json.dumps({"html":str(root / "report.html"),"captures":data["captures"]}))


TEMPLATE = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Detector performance</title><style>
:root{--ink:var(--foreground,#203044);--dim:var(--muted-foreground,#5a6b7c);--line:var(--border,#dce3e9);--panel:var(--muted,#f2f5f7);--blue:var(--chart-1,#3269a8);--green:var(--chart-2,#15978c);--orange:var(--chart-3,#d68c31)}
html,body{background:transparent;color:var(--ink);margin:0;font-family:var(--font-sans,Segoe UI,Arial,sans-serif);font-size:14px;line-height:1.5}
*{box-sizing:border-box}h1{font-size:27px;line-height:1.2;margin:0 0 8px;font-weight:650;letter-spacing:-.6px}h2{font-size:18px;margin:24px 0 10px;font-weight:650}p{margin:8px 0 14px}.dim,.note{color:var(--dim)}.note{font-size:12px}.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:20px 0}.stat{padding:16px;background:var(--panel);border-radius:var(--radius,10px)}.stat strong{display:block;font-size:27px;font-weight:650;letter-spacing:-.6px}.stat span{display:block;font-size:12px;color:var(--dim)}.tabs{display:flex;gap:5px;border-bottom:1px solid var(--line);margin:18px 0 20px;flex-wrap:wrap}.tabs button{font:inherit;font-size:13px;padding:9px 12px;border:0;border-bottom:3px solid transparent;background:none;color:var(--dim);cursor:pointer}.tabs button[aria-selected=true]{color:var(--ink);border-bottom-color:var(--blue);font-weight:650}button:focus-visible,select:focus-visible{outline:2px solid var(--blue);outline-offset:2px}.panel[hidden]{display:none}.toolbar{display:flex;gap:10px;align-items:center;justify-content:space-between;flex-wrap:wrap}select{font:inherit;font-size:13px;border:1px solid var(--line);background:var(--panel);color:var(--ink);border-radius:6px;padding:7px}.chart{width:100%;height:310px;display:block}.tablebox{overflow-x:auto}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums;font-size:13px}th,td{border-bottom:1px solid var(--line);padding:10px 8px;text-align:right;white-space:nowrap}th:first-child,td:first-child{text-align:left;padding-left:0}th{font-size:11px;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.3px}td:last-child,th:last-child{padding-right:0}.flag{color:var(--ink);background:var(--warning-surface,#fff2d5);border-radius:3px;padding:2px 4px;font-size:11px}.finding{border-left:3px solid var(--green);padding-left:14px;margin:16px 0}.legend{display:flex;gap:16px;flex-wrap:wrap;font-size:12px;color:var(--dim)}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:5px}.footer{border-top:1px solid var(--line);padding-top:12px;margin-top:24px;font-size:12px;color:var(--dim)}svg text{font-family:inherit;fill:var(--dim)}@media(max-width:480px){h1{font-size:24px}.stats{gap:7px}.stat{padding:10px}.stat strong{font-size:21px}.stat span{font-size:11px}.tabs button{padding:9px 8px}th,td{padding:9px 6px}.toolbar{align-items:flex-start}}
</style></head><body>
<h1>Detector performance</h1><p class="dim" id="subtitle"></p>
<div class="stats"><div class="stat"><strong id="distanceStat"></strong><span>Powerplant distance<br>12.76 million triangles per side</span></div><div class="stat"><strong id="qualityStat"></strong><span>Less quality time<br>Isolated reference-parameter test</span></div><div class="stat"><strong>524,288</strong><span>Triangles per input<br>Simple difference cases hit the limit</span></div></div>
<div class="tabs" role="tablist" aria-label="Performance evidence"><button role="tab" aria-selected="true" data-tab="costs">Operation costs</button><button role="tab" aria-selected="false" data-tab="cpu">CPU and query work</button><button role="tab" aria-selected="false" data-tab="difference">Difference meshes</button><button role="tab" aria-selected="false" data-tab="controls">Controls</button><button role="tab" aria-selected="false" data-tab="batches">Source batches</button></div>
<section class="panel" id="costs"><div class="toolbar"><h2 style="margin:0">Native wall time</h2><label>Operation <select id="operation"><option value="distance">Two-way distance</option><option value="auto">Automatic detectors</option><option value="quality">Triangle quality</option><option value="intersections">Intersections</option><option value="difference">Difference mesh</option></select></label></div><svg class="chart" id="costChart" role="img" aria-label="Native operation times"></svg><div class="tablebox"><table id="costTable"></table></div><p class="note">Medians and min–max ranges from unprofiled runs. Most cases have three runs; Bear Trap has one. Intersections can return a partial result. A rejected difference is not a completed construction.</p><div class="finding">Powerplant needs much more query work than the larger Bennu mesh. Triangle count alone does not predict comparison time.</div></section>
<section class="panel" id="cpu" hidden><h2>Bus hardware counter estimates</h2><div class="tablebox"><table id="cpuTable"></table></div><p class="note">Two repeats per event group. IPC = retired instructions / cycles. L2 miss share uses cacheable data requests. DTLB walk share uses L1 DTLB misses. These are different denominators, from separate sampled captures.</p><h2>Work per shifted-pair query</h2><div class="tablebox"><table id="queryTable"></table></div><p class="note">Means combine both directions. Large meshes select every 100th 128-face batch against the full target trees. Histogram and second-stride checks are retained in the data. Maximum visits are observed maxima, not percentile estimates.</p><div class="finding">Counter data cover demand cache fills, L2 requests, prefetches, branches, TLB walks, dispatch stalls, and divide/SSE events. IBS, L3 PMCs, and memory-controller counters are unavailable on this host.</div></section>
<section class="panel" id="difference" hidden><h2>Validation changes the scaling curve</h2><div class="legend"><span><i class="dot" style="background:var(--blue)"></i>Open grids</span><span><i class="dot" style="background:var(--green)"></i>Closed spheres</span><span>● Completed &nbsp; × Rejected</span></div><svg class="chart" id="differenceChart" role="img" aria-label="Difference construction scaling"></svg><p class="note">Logarithmic axes. Native controls and coarse timing scopes agree closely on the smaller successful cases. A larger mesh can reject sooner because it stops at an earlier validation stage.</p><div class="tablebox"><table id="differenceTable"></table></div><div class="finding">The 5-million-candidate validation budget rejects simple valid geometry as well as imported meshes. Most of the open-grid time is intersection validation. Raising the limit alone would keep the high CPU cost.</div></section>
<section class="panel" id="controls" hidden><h2>Quality: one isolated code change</h2><div class="tablebox"><table id="tokenTable"></table></div><p class="note">The local stop-check helper takes a const reference. Cancellation remains enabled. All recorded quality summaries match. The production algorithm is unchanged; this is a separate experimental executable.</p><h2>Distance: fixed-size maximum expression</h2><div class="tablebox"><table id="max3Table"></table></div><p class="note">A separate experimental build changes only the maximum-of-three expression in the degeneracy check. Bus uses four alternating runs per variant after compiler activity stops. Large models have one experimental run; they need more repeats before a production change.</p><h2>Bus distance: worker budget</h2><div class="tablebox"><table id="workerTable"></table></div><div class="finding">Parallel queries improve with more workers. About 5.3 seconds of serial preparation and statistics remain on Bus. Face order, retained data, physical-core affinity, cancellation, and local deformation also have separate controls in the report.</div></section>
<section class="panel" id="batches" hidden><h2>Same triangles, more detector source batches</h2><div class="tablebox"><table id="batchTable"></table></div><p class="note">Seconds. All triangles retained; parts are divided into contiguous detector source groups. Bus has three runs per case; San Miguel has one. A dash means the case was not run. The grouping changes detection domains.</p><h2>Degenerate check with every imported part separate</h2><div class="tablebox"><table id="fullBatchTable"></table></div><p class="note">One run per large-model control. The full geometry and recorded degenerate counts stay unchanged.</p><h2>Real application requests on Bus</h2><div class="tablebox"><table id="appTable"></table></div><p class="note">Headless viewer through CLI acknowledgments. Three runs except the identical-pair request, which has one. The two-sided request includes quality and automatic detectors as well as distance.</p><div class="finding">Repeated full-source validation and scratch arrays make separate detector batches costly. Cache source validation while preserving the separate detection domains.</div></section>
<p class="footer">Release build · Ryzen 7 5800H · 64 GiB RAM · MSVC 19.51 · 17 October 2026<br>Shifted copies and local deformation are controlled test pairs. Profiler times are separate from native latency. Detailed methods, results, limits, and raw evidence are saved with the report.</p>
<script>
const D=__DATA__;
const label={auto:'Automatic',quality:'Quality',intersections:'Intersections',distance:'Distance',difference:'Difference'};
const colors=['#3269a8','#15978c','#d68c31','#9467b4','#bd5260'];
const f=(v,d=2)=>v==null?'—':Number(v).toLocaleString('en-US',{minimumFractionDigits:d,maximumFractionDigits:d});
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const table=(id,headers,rows)=>document.getElementById(id).innerHTML='<thead><tr>'+headers.map(h=>'<th>'+h+'</th>').join('')+'</tr></thead><tbody>'+rows.map(r=>'<tr>'+r.map(c=>'<td>'+c+'</td>').join('')+'</tr>').join('')+'</tbody>';
document.getElementById('subtitle').textContent=D.captures+' retained captures · Large-model CPU analysis · source 61efba0';
const power=D.native.find(r=>r.model==='Powerplant'&&r.operation==='distance');
document.getElementById('distanceStat').textContent=power?f(power.median_ms/1000,0)+' s':'—';
document.getElementById('qualityStat').textContent=D.tokens.length?f(Math.min(...D.tokens.map(r=>r.saving)),0)+'–'+f(Math.max(...D.tokens.map(r=>r.saving)),0)+'%':'—';
function cost(){const op=document.getElementById('operation').value,rows=D.native.filter(r=>r.operation===op),svg=document.getElementById('costChart');const w=svg.clientWidth||700,h=310,left=94,right=62,top=26,bottom=35,plot=w-left-right,max=Math.max(...rows.map(r=>r.max_ms/1000),.001)*1.06;let s='';for(let i=0;i<=4;i++){const x=left+plot*i/4;s+=`<line x1="${x}" y1="${top-10}" x2="${x}" y2="${h-bottom}" stroke="var(--line)"/><text x="${x}" y="${h-12}" text-anchor="middle" font-size="11">${f(max*i/4,max<10?1:0)}</text>`;}rows.forEach((r,i)=>{const y=top+i*(h-top-bottom)/Math.max(rows.length,1),v=r.median_ms/1000,bw=plot*v/max;s+=`<text x="${left-9}" y="${y+15}" text-anchor="end" font-size="12">${esc(r.model)}</text><rect x="${left}" y="${y}" width="${Math.max(bw,1)}" height="25" rx="3" fill="${colors[i]}" opacity="${r.status==='ok'?1:.35}"/><text x="${left+bw+6}" y="${y+16}" font-size="11">${f(v,v<10?2:1)}</text>`;});s+=`<text x="${w-2}" y="${h-12}" text-anchor="end" font-size="11">seconds</text>`;svg.setAttribute('viewBox',`0 0 ${w} ${h}`);svg.innerHTML=s;table('costTable',['Model','Triangles','Median (s)','Range (s)','Result'],rows.map(r=>[esc(r.model),f(r.triangles/1e6,2)+' M',f(r.median_ms/1000),f(r.min_ms/1000)+'–'+f(r.max_ms/1000),r.status!=='ok'?'<span class="flag" title="'+esc(r.error)+'">Rejected</span>':r.intersection_truncated?'<span class="flag">Partial</span>':'Complete']));}
table('cpuTable',['Operation','IPC','Branch miss %','L2 miss %','DTLB walk %'],D.counters.map(r=>[label[r.operation],f(r.ipc),f(r.branch),f(r.l2),f(r.tlb)]));
table('queryTable',['Model','Batch selection','Stack visits','Triangle tests','Max visits'],D.queries.map(r=>[esc(r.model),r.stride===1?'All':'1 / '+r.stride,f(r.visits,1),f(r.triangles,1),f(r.maximum,0)]));
table('tokenTable',['Model','Production (s)','Reference (s)','Time saved','Runs'],D.tokens.map(r=>[esc(r.model),f(r.live.median_ms/1000),f(r.reference.median_ms/1000),f(r.saving,1)+'%',r.live.runs+' / '+r.reference.runs]));
table('max3Table',['Model','Baseline (s)','Experiment (s)','Time saved','Runs'],D.max3.map(r=>[esc(r.model),f(r.baseline.median_ms/1000),f(r.experimental.median_ms/1000),f(r.saving,1)+'%',r.baseline.runs+' / '+r.experimental.runs]));
const batchLabels={'auto':'Automatic','duplicate-points':'Duplicate points','duplicate-triangles':'Duplicate triangles','degenerates':'Degenerates','topology':'Topology'};
table('batchTable',['Model / check','1 batch','8 batches','32 batches','65 batches'],D.batches.map(r=>[esc(r.model)+' / '+batchLabels[r.operation],...['1','8','32','65'].map(k=>f(r.seconds[k],3))]));
table('fullBatchTable',['Model','Batches','One batch (s)','All parts (s)','Time ratio'],D.full_batches.map(r=>[esc(r.model),f(r.batches,0),f(r.base,3),f(r.full,3),f(r.ratio,1)+'×']));
const appLabels={automatic_ready:'Automatic results',quality_and_full_results:'Quality + full response',manual_intersections_ready:'Bounded intersections',cached_results_query:'Cached full response',identical_pair_full_results:'Identical pair, full response'};
table('appTable',['Request','Whole model (s)','Per volume (s)'],D.application.map(r=>[appLabels[r.operation],f(r.whole,3),f(r.per_volume,3)]));
table('workerTable',['Caller + helpers','Median (s)','CPU time (s)','Average active CPUs'],D.workers.map(r=>[Number(r.case.split('-').at(-1))+1,f(r.median_ms/1000),f(r.median_cpu_ms/1000),f(r.median_average_logical_cpus)]));
table('differenceTable',['Input','Triangles','Time (s)','Result'],D.scaling.filter(r=>r.triangles>=131072).map(r=>[r.case.startsWith('grid')?'Open grid':'Closed sphere',f(r.triangles,0),f(r.median_ms/1000),r.status==='ok'?'Complete':'<span class="flag">Validation limit</span>']));
function difference(){const svg=document.getElementById('differenceChart'),w=svg.clientWidth||700,h=310,l=52,r=20,t=22,b=48,rows=D.scaling;const xs=[512,8192,131072,2097152],ys=[.01,.1,1,10,100],x=v=>l+(Math.log10(v)-Math.log10(512))/(Math.log10(2097152)-Math.log10(512))*(w-l-r),y=v=>h-b-(Math.log10(v)+2)/4*(h-t-b);let s='';for(const v of xs)s+=`<line x1="${x(v)}" y1="${t}" x2="${x(v)}" y2="${h-b}" stroke="var(--line)"/><text x="${x(v)}" y="${h-b+18}" text-anchor="middle" font-size="10">${v<1e6?f(v/1000,v<1000?1:0)+'k':f(v/1e6,1)+'M'}</text>`;for(const v of ys)s+=`<line x1="${l}" y1="${y(v)}" x2="${w-r}" y2="${y(v)}" stroke="var(--line)"/><text x="${l-8}" y="${y(v)+4}" text-anchor="end" font-size="11">${v}</text>`;for(const [shape,color] of [['grid','#3269a8'],['sphere','#15978c']]){const points=rows.filter(q=>q.case.startsWith(shape)&&q.status==='ok').sort((a,b)=>a.triangles-b.triangles);s+=`<polyline points="${points.map(q=>x(q.triangles)+','+y(q.median_ms/1000)).join(' ')}" fill="none" stroke="${color}" stroke-width="2"/>`;for(const q of rows.filter(q=>q.case.startsWith(shape))){const a=x(q.triangles),c=y(q.median_ms/1000);s+=q.status==='ok'?`<circle cx="${a}" cy="${c}" r="4" fill="${color}"><title>${esc(q.case)}: ${f(q.median_ms/1000)} s</title></circle>`:`<path d="M ${a-5} ${c-5} L ${a+5} ${c+5} M ${a-5} ${c+5} L ${a+5} ${c-5}" stroke="${color}" stroke-width="2"><title>${esc(q.error)}</title></path>`;}}s+=`<text x="${(w+l)/2}" y="${h-5}" text-anchor="middle" font-size="11">Triangles per input</text><text x="${l}" y="12" font-size="11">Seconds</text>`;svg.setAttribute('viewBox',`0 0 ${w} ${h}`);svg.innerHTML=s;}
document.getElementById('operation').addEventListener('change',cost);
document.querySelectorAll('[data-tab]').forEach(button=>button.addEventListener('click',()=>{document.querySelectorAll('[data-tab]').forEach(b=>b.setAttribute('aria-selected',b===button?'true':'false'));document.querySelectorAll('.panel').forEach(p=>p.hidden=p.id!==button.dataset.tab);if(button.dataset.tab==='costs')cost();if(button.dataset.tab==='difference')difference();}));
window.addEventListener('resize',()=>{if(!document.getElementById('costs').hidden)cost();if(!document.getElementById('difference').hidden)difference();});
cost();console.log(JSON.stringify({captures:D.captures,nativeRows:D.native.length,counterRows:D.counters.length,queryModels:D.queries.length,qualityControls:D.tokens.length}));
</script></body></html>'''


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("evidence",type=Path)
    create(parser.parse_args().evidence.resolve())
