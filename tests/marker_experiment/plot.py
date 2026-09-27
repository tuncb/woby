import json
from pathlib import Path
import statistics
import sys
import matplotlib.pyplot as plt
import numpy as np

data=json.loads(Path(sys.argv[1]).read_text())
names={"20190810_BearTrap_Ground_Model.obj":"BearTrap","bennu_OLA_v21_PTM_very-high.obj":"Bennu",
       "powerplant.obj":"Powerplant","san-miguel.obj":"San Miguel","uploads_files_2720101_BusGameMap.obj":"Bus"}
models=list(names)
def value(model,case,mode,get):
    values=[get(row) for run in data if run["model"]==model for row in run["scenarios"]
            if row["settings"].get("case")==case and row["settings"].get("picker")==mode]
    return statistics.median(v for v in values if v is not None)
plt.rcParams.update({"font.family":"DejaVu Sans","font.size":10,"axes.spines.top":False,"axes.spines.right":False})
fig,axes=plt.subplots(2,2,figsize=(13,8.3),layout="constrained")
x=np.arange(len(models))
for i,(mode,label,color) in enumerate([("id_resident","GPU highlight","#268bba"),("id_async","Highlight + async text","#e99432")]):
    ratios=[value(m,"fit",mode,lambda r:r["fps"])/value(m,"fit","none",lambda r:r["fps"])*100 for m in models]
    bars=axes[0,0].bar(x+(i-.5)*.34,ratios,.34,label=label,color=color)
    axes[0,0].bar_label(bars,fmt="%.0f%%",fontsize=9,padding=3)
axes[0,0].axhline(100,color="#666",lw=1,ls="--")
axes[0,0].set(title="Fitted view: FPS retained versus no picking",ylabel="% of no-picking FPS",xticks=x,xticklabels=[names[m] for m in models])
axes[0,0].legend(frameon=False,fontsize=9)
for i,case in enumerate(["far","near","zoom","orbit","pan"]):
    ys=[value(m,case,"id_async",lambda r:r["fps"])/value(m,case,"none",lambda r:r["fps"])*100 for m in models]
    axes[0,1].plot(x,ys,marker="o",label=case)
axes[0,1].axhline(100,color="#666",lw=1,ls="--")
axes[0,1].set(title="Zoom and movement: FPS retained with async text",ylabel="% of no-picking FPS",xticks=x,xticklabels=[names[m] for m in models])
axes[0,1].legend(frameon=False,ncol=3,fontsize=9)
for i,(view,label,color) in enumerate([("2","Cursor lookup","#268bba"),("4","Highlight","#e99432")]):
    ys=[value(m,"fit","id_async",lambda r:r["views"][view]["median"])*1000 for m in models]
    axes[1,0].bar(x+(i-.5)*.34,ys,.34,label=label,color=color)
axes[1,0].set(title="Small GPU passes (excludes ID-buffer rendering)",ylabel="Microseconds",xticks=x,xticklabels=[names[m] for m in models])
axes[1,0].legend(frameon=False,fontsize=9)
ys=[value(m,"fit","id_async",lambda r:r["readback_ms"]["median"]) for m in models]
bars=axes[1,1].bar(x,ys,color="#e99432")
axes[1,1].bar_label(bars,fmt="%.1f",padding=3)
axes[1,1].set(title="CPU coordinate age; GPU highlight avoids this wait",ylabel="Issue-to-completion milliseconds",xticks=x,xticklabels=[names[m] for m in models])
fig.suptitle("Visible-marker GPU picking · Opaque 4 px markers · RTX 3070 Laptop · D3D11",fontsize=14,fontweight="bold")
fig.savefig(sys.argv[2],dpi=170)
