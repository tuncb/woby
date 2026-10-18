# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib>=3.9"]
# ///
"""Draw standalone figures from the exported detector evidence."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics
import zipfile

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

COLORS = ["#3269a8","#15978c","#d68c31","#9467b4","#bd5260","#8795a3"]
NAMES = {"bus":"Bus","powerplant":"Powerplant","sanmiguel":"San Miguel","bennu":"Bennu","beartrap":"Bear Trap"}


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save(figure,root,name):
    figure.savefig(root / (name+".svg"),bbox_inches="tight")
    figure.savefig(root / (name+".png"),dpi=180,bbox_inches="tight")
    plt.close(figure)


def phases(case,operation,names):
    totals = []
    for parent in (e for e in case["events"] if e["name"] == "op."+operation):
        values = [sum(e["wall_ms"] for e in case["events"] if e["name"] == name
            and parent["start_ms"] <= e["start_ms"] and e["end_ms"] <= parent["end_ms"])
            for name in names]
        totals.append([*values,max(0,parent["wall_ms"]-sum(values))])
    return [statistics.median(v) / 1000 for v in zip(*totals)] if totals else []


def plots(root):
    data = read(root / "summary.json"); rows = data["rows"]
    cases = {c["name"]:c for c in data["cases"]}
    lookup = {(r["case"],r["operation"]):r for r in rows}
    plt.rcParams.update({"font.family":"DejaVu Sans","font.size":10,"axes.spines.top":False,
        "axes.spines.right":False,"axes.titlelocation":"left","axes.titlesize":12,
        "figure.facecolor":"white","axes.facecolor":"white","svg.fonttype":"none"})
    figure,axes = plt.subplots(2,2,figsize=(12,7.4),layout="constrained")
    for ax,op,title in zip(axes.flat,("auto","quality","intersections","distance"),
            ("Nine automatic detectors","Triangle quality","Intersections · bounded work","Two-way distance · shifted pair")):
        selected = [(key,r) for key in NAMES if (r := lookup.get((key+"-native",op))) and r["status"] == "ok"]
        for index,(key,row) in enumerate(selected):
            value = row["median_ms"]/1000
            ax.barh(index,value,color=COLORS[index%len(COLORS)],height=.65)
            ax.errorbar(value,index,xerr=[[max(0,value-row["min_ms"]/1000)],[max(0,row["max_ms"]/1000-value)]],
                fmt="none",ecolor="#233446",capsize=3)
            ax.text(value*1.04,index,f"{value:.2f}s"+(" *" if op == "intersections" and row["intersection_truncated"] else ""),va="center",fontsize=9)
        ax.set_yticks(range(len(selected)),[NAMES[k] for k,r in selected]); ax.invert_yaxis()
        if selected:
            ax.set_xlim(0,max(r["max_ms"] for k,r in selected)/1000*1.25)
        ax.set_title(title); ax.set_xlabel("Wall time (seconds)"); ax.grid(axis="x",alpha=.15); ax.set_axisbelow(True)
    figure.suptitle("Large-model CPU costs",fontsize=18,fontweight="bold",x=.02,ha="left")
    figure.supxlabel("Native medians; whiskers show min–max. * Partial intersection result. Errors are excluded from bars.",fontsize=9)
    save(figure,root,"native-costs")

    figure,axes = plt.subplots(1,2,figsize=(12,4.7),layout="constrained")
    definitions = [
        ("distance",[("bus-scopes","Bus"),("powerplant-timechart","Powerplant"),("bennu-timechart","Bennu")],
            ["mesh_comparison.buildTree","distance.queries","distance.statistics"],["Build trees","Query triangles","Statistics","Other"]),
        ("quality",[("bus-scopes","Bus"),("powerplant-stages","Powerplant"),("bennu-stages","Bennu")],
            ["quality.metrics_and_weld","quality.edges_sort_reduce","quality.statistics"],["Metrics / weld","Edges","Statistics","Other"])]
    for ax,(operation,selected,names,labels) in zip(axes,definitions):
        available = [(name,label,phases(cases[name],operation,names)) for name,label in selected if name in cases]
        left = np.zeros(len(available))
        for index,label in enumerate(labels):
            values = [100*a[2][index]/sum(a[2]) for a in available]
            ax.barh(range(len(available)),values,left=left,color=COLORS[index],label=label)
            for y,value in enumerate(values):
                if value >= 9:
                    ax.text(left[y]+value/2,y,f"{value:.0f}%",ha="center",va="center",color="white",fontsize=9)
            left += values
        ax.set_yticks(range(len(available)),[f"{label}\n{sum(values):.1f}s" for name,label,values in available]); ax.invert_yaxis()
        ax.set_xlabel("Share of operation wall time (%)"); ax.set_xlim(0,100); ax.set_title(operation.capitalize()+" phases")
        ax.legend(loc="upper center",bbox_to_anchor=(.5,-.18),ncol=2,frameon=False,fontsize=9)
    figure.suptitle("Parallel query work and serial preparation",fontsize=17,fontweight="bold",x=.02,ha="left")
    save(figure,root,"stage-shares")

    figure,axes = plt.subplots(1,2,figsize=(12,4.7),layout="constrained")
    for shape,color in (("grid",COLORS[0]),("sphere",COLORS[1])):
        selected = [(cases[name]["mesh"]["triangles"],lookup[(name,"difference")])
            for size in (32,64,128,256,512,1024) if (name := f"{shape}-{size}") in cases]
        success = [(n,r) for n,r in selected if r["status"] == "ok"]
        failure = [(n,r) for n,r in selected if r["status"] != "ok"]
        axes[0].plot([n for n,r in success],[r["median_ms"]/1000 for n,r in success],"o-",color=color,label=shape.capitalize()+" success")
        axes[0].scatter([n for n,r in failure],[r["median_ms"]/1000 for n,r in failure],marker="x",s=70,color=color,label=shape.capitalize()+" rejected")
    axes[0].set_xscale("log"); axes[0].set_yscale("log"); axes[0].set_xlabel("Triangles per input")
    axes[0].set_ylabel("Wall time (seconds)"); axes[0].set_title("Construction and validation limits"); axes[0].legend(frameon=False,fontsize=8)
    axes[0].grid(alpha=.15,which="both")
    names = ["difference_mesh.read","difference_mesh.validate","difference_mesh.solid","difference_mesh.output","difference_mesh.publish"]
    labels = ["Read / weld","Validate","Make solids","Evaluate / extract","Publish","Other"]
    selected = [(name,label,phases(cases[name],"difference",names)) for name,label in
        (("grid-256","Open grid · 131,072"),("sphere-256","Sphere · 32,768"),("sphere-512","Sphere · 131,072")) if name in cases]
    left = np.zeros(len(selected))
    for index,label in enumerate(labels):
        values = [v[index] for name,label,v in selected]
        axes[1].barh(range(len(selected)),values,left=left,color=COLORS[index],label=label); left += values
    axes[1].set_yticks(range(len(selected)),[label for name,label,v in selected]); axes[1].invert_yaxis()
    axes[1].set_title("Successful difference-mesh phases"); axes[1].set_xlabel("Wall time (seconds)")
    axes[1].legend(loc="upper center",bbox_to_anchor=(.5,-.18),ncol=3,fontsize=8,frameon=False)
    figure.suptitle("Validation can cost more than construction",fontsize=17,fontweight="bold",x=.02,ha="left")
    save(figure,root,"difference-scaling")

    figure,axes = plt.subplots(1,2,figsize=(12,4.7),layout="constrained")
    workers = [(int(name.rsplit("-",1)[1])+1,lookup[(name,"distance")],cases[name])
        for name in cases if name.startswith("bus-workers-")]
    workers.sort()
    axes[0].plot([n for n,r,c in workers],[r["median_ms"]/1000 for n,r,c in workers],"o-",color=COLORS[0],label="Whole distance operation")
    axes[0].plot([n for n,r,c in workers],[phases(c,"distance",["distance.queries"])[0] for n,r,c in workers],"o-",color=COLORS[1],label="Both query phases")
    axes[0].set_xlabel("Caller + available helper slots"); axes[0].set_ylabel("Wall time (seconds)"); axes[0].set_title("Bus worker budget")
    axes[0].legend(frameon=False); axes[0].grid(alpha=.15); axes[0].set_xticks([n for n,r,c in workers])
    token = []
    for model in ("bus","powerplant","bennu"):
        live = lookup.get((model+"-token-live","quality")); ref = lookup.get((model+"-token-reference","quality"))
        if live and ref:
            token.append((NAMES[model],live,ref))
    x = np.arange(len(token)); width = .34
    axes[1].bar(x-width/2,[v[1]["median_ms"]/1000 for v in token],width,color=COLORS[0],label="Production live token")
    axes[1].bar(x+width/2,[v[2]["median_ms"]/1000 for v in token],width,color=COLORS[1],label="Helper takes reference")
    for index,(label,live,ref) in enumerate(token):
        axes[1].text(index,max(live["median_ms"],ref["median_ms"])/1000*1.025,f'{live["median_ms"]/ref["median_ms"]:.2f}×',ha="center")
    axes[1].set_xticks(x,[v[0] for v in token]); axes[1].set_ylabel("Quality wall time (seconds)")
    axes[1].set_title("Isolated cancellation-token experiment"); axes[1].legend(frameon=False,fontsize=9)
    figure.suptitle("Measured controls",fontsize=17,fontweight="bold",x=.02,ha="left")
    save(figure,root,"controls")

    figure,axes = plt.subplots(1,2,figsize=(12,4.6),layout="constrained")
    batch_ops = [("auto","All automatic"),("duplicate-points","Duplicate points"),
        ("duplicate-triangles","Duplicate triangles"),("degenerates","Degenerates")]
    for ax,model in zip(axes,("bus","sanmiguel")):
        counts = sorted(int(name.rsplit("-",1)[1]) for name in cases if name.startswith(model+"-batches-"))
        for color,(operation,label) in zip(COLORS,batch_ops):
            pairs = [(count,lookup[(f"{model}-batches-{count}",operation)]["median_ms"]/1000)
                for count in counts if (f"{model}-batches-{count}",operation) in lookup]
            if pairs:
                ax.plot([p[0] for p in pairs],[p[1] for p in pairs],"o-",color=color,label=label)
        ax.set_xscale("log",base=2); ax.set_xticks(counts,[str(n) for n in counts])
        ax.set_ylim(bottom=0); ax.set_xlabel("Detector source batches"); ax.set_ylabel("Wall time (seconds)")
        ax.set_title(NAMES[model]); ax.grid(alpha=.15); ax.legend(frameon=False,fontsize=9)
    figure.suptitle("Repeated source work grows with the batch count",fontsize=17,fontweight="bold",x=.02,ha="left")
    full = lookup.get(("sanmiguel-all-batches-degenerates","degenerates"))
    note = "All triangles retained. Contiguous part groups share source arrays. Bus: three runs. San Miguel: one run."
    if full:
        base = lookup[("sanmiguel-batches-1","degenerates")]
        note += f"\nSeparate full-part control: San Miguel, 2,203 batches, degenerates {full['median_ms']/1000:.1f} s ({full['median_ms']/base['median_ms']:.0f} times the one-batch time)."
    figure.supxlabel(note,fontsize=9)
    save(figure,root,"source-batches")

    query_path = root / "queries.json"
    if query_path.exists():
        query = read(query_path); figure,axes = plt.subplots(1,2,figsize=(12,4.7),layout="constrained")
        for index,model in enumerate(("bus","powerplant","bennu")):
            groups = []
            for shift in ("0",".00001",".001",".01"):
                values = [r for r in query if r["case"] == model+"-query-shift-"+shift]
                if values:
                    groups.append((shift,statistics.mean(r["mean_triangle_tests"] for r in values),statistics.mean(r["mean_stack_visits"] for r in values)))
            for ax,column in zip(axes,(1,2)):
                ax.plot(range(len(groups)),[g[column] for g in groups],"o-",color=COLORS[index],label=NAMES[model])
                ax.set_xticks(range(4),["0","0.00001","0.001","0.01"]); ax.set_yscale("log"); ax.grid(alpha=.15)
                ax.set_xlabel("Z shift / bounding-box diagonal"); ax.legend(frameon=False)
        axes[0].set_ylabel("Mean target triangle tests per query"); axes[1].set_ylabel("Mean stack entries visited per query")
        figure.suptitle("Geometry and displacement change query work",fontsize=17,fontweight="bold",x=.02,ha="left")
        figure.supxlabel("Both directions averaged. Bus: all queries. Large models: every 100th batch; full target trees.",fontsize=9)
        save(figure,root,"query-work")

    hardware_path = root / "hardware-summary.json"
    if hardware_path.exists():
        hardware = read(hardware_path); figure,axes = plt.subplots(1,3,figsize=(12,4.4),layout="constrained")
        metrics = [("bus-branch","branch_mispredict_percent_estimate","Branch mispredicts / branches (%)"),
            ("bus-l2","l2_data_request_miss_percent_estimate","L2 data-request miss share (%)"),
            ("bus-tlb","dtlb_walk_share_of_l1_misses_percent_estimate","DTLB walk share of L1 misses (%)")]
        for ax,(case,key,label) in zip(axes,metrics):
            ops = ("auto","quality","intersections","distance"); values = []
            for operation in ops:
                selected = [r[key] for r in hardware if r["case"] == case and r["stage"] == "op."+operation and r.get(key) is not None]
                values.append(statistics.mean(selected) if selected else float("nan"))
            ax.barh(range(4),values,color=COLORS[:4]); ax.set_yticks(range(4),["Automatic","Quality","Intersections","Distance"]); ax.invert_yaxis()
            ax.set_xlabel(label,fontsize=9); ax.grid(axis="x",alpha=.15); ax.set_axisbelow(True)
            for index,value in enumerate(values):
                if np.isfinite(value):
                    ax.text(value,index,f" {value:.2f}%",va="center",fontsize=9)
            ax.margins(x=.25)
        figure.suptitle("Bus CPU counter estimates",fontsize=17,fontweight="bold",x=.02,ha="left")
        figure.supxlabel("Separate two-run captures; period-scaled sampling estimates. Denominators differ between panels.",fontsize=9)
        save(figure,root,"hardware-rates")

    selected = [name for name in ("powerplant-timechart","bennu-timechart") if name in cases]
    if selected and (root / "raw-evidence.zip").exists():
        figure,axes = plt.subplots(len(selected),1,figsize=(12,3.2*len(selected)),layout="constrained",squeeze=False)
        with zipfile.ZipFile(root / "raw-evidence.zip") as archive:
            for ax,name in zip(axes.flat,selected):
                case = cases[name]; probe = json.loads(archive.read(name+"/probe.json"))
                resource = json.loads(archive.read(name+"/resources.json"))
                parent = next(e for e in probe["events"] if e["name"] == "op.distance")
                offset = (probe["clock_origin"]["steady_epoch_nanoseconds"]-case["metadata"]["observer_start_qpc_ns"])/1e9
                start,end = parent["start_ms"]/1000,parent["end_ms"]/1000
                points = [(s["elapsed_s"]-offset,s["rss"]/1024**3,s["cpu_s"]) for s in resource["samples"]]
                inside = [(t-start,rss,cpu) for t,rss,cpu in points if start <= t <= end]
                active = [(t-start,(cpu-points[i-1][2])/(t-points[i-1][0])) for i,(t,rss,cpu) in enumerate(points)
                    if i and start <= points[i-1][0] < t <= end]
                labels = set()
                for event in probe["events"]:
                    if event["start_ms"] < parent["start_ms"] or event["end_ms"] > parent["end_ms"]:
                        continue
                    for key,label,color in (("mesh_comparison.buildTree","Tree build",COLORS[0]),
                            ("distance.queries","Queries",COLORS[1]),("distance.statistics","Statistics",COLORS[2])):
                        if event["name"] == key:
                            ax.axvspan(event["start_ms"]/1000-start,event["end_ms"]/1000-start,color=color,alpha=.13,
                                label=label if label not in labels else None); labels.add(label)
                ax.plot([p[0] for p in inside],[p[1] for p in inside],color="#263d53",lw=1.7,label="Process RSS")
                cpu_ax = ax.twinx(); cpu_ax.plot([p[0] for p in active],[p[1] for p in active],color=COLORS[4],lw=.8,alpha=.8,label="Active logical CPUs")
                cpu_ax.set_ylim(0,10); cpu_ax.set_ylabel("Process CPU / wall time",color=COLORS[4])
                ax.set_xlim(0,end-start); ax.set_ylim(bottom=0); ax.set_ylabel("Process RSS (GiB)")
                ax.set_xlabel("Seconds from distance start"); ax.set_title(NAMES[name.split('-')[0]])
                handles,texts = ax.get_legend_handles_labels(); h2,t2 = cpu_ax.get_legend_handles_labels()
                ax.legend(handles+h2,texts+t2,loc="lower right",ncol=5,fontsize=8,frameon=True)
        figure.suptitle("Memory and parallel work over the distance operation",fontsize=17,fontweight="bold",x=.02,ha="left")
        figure.supxlabel("Observed process-sample interval: about 1.7 s median. Shaded intervals are probe scopes; short peaks can be missed.",fontsize=9)
        save(figure,root,"resource-timeline")

    print(json.dumps({"figures":[p.name for p in root.glob("*.png")]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("evidence",type=Path)
    plots(parser.parse_args().evidence.resolve())
