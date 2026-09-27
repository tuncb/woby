"""Create a standalone comparison figure from enriched Release results."""
import argparse
import json
from pathlib import Path
import statistics
import matplotlib.pyplot as plt
import numpy as np


def short(name):
    for part,label in (("BearTrap","BearTrap"),("bennu","Bennu"),("powerplant","Powerplant"),("san-miguel","San Miguel"),("Bus","Bus")):
        if part in name:
            return label
    return name


def main():
    p=argparse.ArgumentParser()
    p.add_argument("input",type=Path)
    p.add_argument("output",type=Path)
    args=p.parse_args()
    data=json.loads(args.input.read_text())
    models=list(dict.fromkeys(row["model"] for row in data))
    def value(model,case,field,subfield=None):
        rows=[s for r in data if r["model"]==model for s in r["scenarios"] if s["name"]==case]
        values=[s[field][subfield] if subfield else s[field] for s in rows]
        return statistics.median(values)
    labels=[short(m) for m in models]; x=np.arange(len(models))
    plt.rcParams.update({"font.family":"DejaVu Sans","font.size":10,"axes.spines.top":False,"axes.spines.right":False})
    fig,grid=plt.subplots(2,2,figsize=(13,9.5),layout="constrained")
    axes=grid.ravel()
    colors=("#9ca3af","#087f8c","#de7a22")
    for i,(mode,label,color) in enumerate(zip(("legacy","cpu","gpu_cluster"),("Current scan","CPU BVH","GPU clusters"),colors)):
        fps=[value(m,"fit_"+mode,"fps") for m in models]
        bars=axes[0].bar(x+(i-1)*.25,fps,.23,label=label,color=color)
        axes[0].bar_label(bars,labels=[f"{v:.1f}" if v<10 else f"{v:.0f}" for v in fps],fontsize=8,padding=3,rotation=90)
    axes[0].set_yscale("log"); axes[0].set_ylim(.4,3500)
    axes[0].set_title("FPS while moving the pointer",loc="left",weight="bold")
    axes[0].set_ylabel("Frames per second · logarithmic scale")
    for i,(mode,label,color) in enumerate(zip(("legacy","cpu"),("Current scan","CPU BVH"),colors[:2])):
        vals=[value(m,"fit_"+mode,"stages","hover_pick") for m in models]
        bars=axes[1].bar(x+(i-.5)*.33,vals,.31,label=label,color=color)
        axes[1].bar_label(bars,labels=[f"{v:.2f}" if v<10 else f"{v:.0f}" for v in vals],fontsize=8,padding=3)
    axes[1].set_yscale("log"); axes[1].set_ylim(.01,3500)
    axes[1].set_title("CPU hover stage · pointer sweep includes misses",loc="left",weight="bold")
    axes[1].set_ylabel("Milliseconds · logarithmic scale")
    for i,(case,label,color) in enumerate((("center_cpu","Fitted center","#087f8c"),("far_center_cpu","4× farther center","#354f52"))):
        vals=[value(m,case,"stages","hover_pick") for m in models]
        bars=axes[2].bar(x+(i-.5)*.33,vals,.31,label=label,color=color)
        axes[2].bar_label(bars,labels=[f"{v:.2f}"+("*" if value(m,case,"cpu_hit_queries")==0 else "") for m,v in zip(models,vals)],fontsize=8,padding=3)
    axes[2].set_yscale("log"); axes[2].set_ylim(.01,200)
    axes[2].set_title("CPU BVH at a fixed center · cache bypassed",loc="left",weight="bold")
    axes[2].set_ylabel("Milliseconds · logarithmic scale")
    axes[2].legend(frameon=False,fontsize=8,loc="upper right")
    gpu=[value(m,"fit_gpu_cluster","latency_ms","p50") for m in models]
    bars=axes[3].bar(x,gpu,.6,color=colors[2])
    axes[3].bar_label(bars,labels=[f"{v:.1f} ms" for v in gpu],padding=3,fontsize=9)
    axes[3].set_ylim(0,max(gpu)*1.2)
    axes[3].set_title("GPU delayed readback · fitted pointer sweep",loc="left",weight="bold")
    axes[3].set_ylabel("Milliseconds · median issue to CPU observation")
    for ax in axes:
        ax.set_xticks(x,labels,rotation=20,ha="right")
        ax.grid(axis="y",alpha=.18); ax.set_axisbelow(True)
    axes[0].legend(frameon=False,fontsize=8,loc="upper left")
    fig.suptitle("Hover picking: throughput, query cost, and result age",x=.01,ha="left",weight="bold",fontsize=16)
    fig.supxlabel("RTX 3070 Laptop · Release D3D11 · 1280×720 · 4× MSAA · VSync off\nCore comparisons: median of two runs. Fixed-center controls: one run. * No vertex hit at this center.",fontsize=10)
    fig.savefig(args.output,dpi=190)


if __name__=="__main__":
    main()
