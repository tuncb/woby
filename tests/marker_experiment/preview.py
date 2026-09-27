"""Build a visual QA artifact from the real Release fixture captures."""
from pathlib import Path
import matplotlib.pyplot as plt
from PIL import Image

root=Path(__file__).resolve().parents[2]
fig,axes=plt.subplots(1,2,figsize=(13,4.3),layout="constrained")
for ax,frame,title in zip(axes,(3,9),("Early capture · GPU highlight", "Later capture · coordinates received")):
    path=root/f"build/marker-fixture-final/overlap-1-opaque_id_async-frame{frame}.png"
    picture=Image.open(path).convert("RGB").crop((350,170,1270,710))
    ax.imshow(picture); ax.axis("off"); ax.set_title(title,fontsize=12,pad=10)
fig.suptitle("Selected marker appears before the asynchronous coordinate panel",fontsize=14,fontweight="bold")
fig.savefig(root/"doc/marker-picking-preview.png",dpi=170)
