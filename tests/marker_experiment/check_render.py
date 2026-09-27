# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Check that MRT routing and ID output preserve the scene's visible colors."""
import argparse
import json
from pathlib import Path
import tempfile
from PIL import Image, ImageChops
from run import execute


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--binary",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    results=[]
    with tempfile.TemporaryDirectory(prefix="woby marker color fixture ") as directory:
        root=Path(directory).resolve()
        model=root/"colors.obj"
        model.write_text("v 0 0 0\nv -1 -1 0\nv 1 -1 0\nv 0 0 1\nv -1 1 1\nv 1 1 1\ng rear\nf 1 2 3\ng front\nf 4 5 6\n")
        settings=[dict(case="opaque"),dict(case="alpha",opacity=.4),dict(case="solid",solid=True),dict(case="pane",pane=True),dict(case="single",msaa=False)]
        choices=[dict(name=f"{s['case']}_{mode}",picker=mode,vertices=True,solid=False,pointer="center",point_size=16)|s
                 for s in settings for mode in ("none","routed","id_buffer")]
        execute(args.binary.resolve(),model,root,choices,0,captures=True,fixture=True,
                overrides=dict(warmup_frames=2,warmup_seconds=0,min_frames=8,measure_seconds=0))
        for s in settings:
            baseline=Image.open(root/f"colors-1-{s['case']}_none-frame9.tga.tga").convert("RGB")
            for mode in ("routed","id_buffer"):
                actual=Image.open(root/f"colors-1-{s['case']}_{mode}-frame9.tga.tga").convert("RGB")
                difference=ImageChops.difference(baseline,actual)
                maximum=max(v for limits in difference.getextrema() for v in limits)
                results.append(dict(case=s['case'],mode=mode,max_channel_difference=maximum))
                actual.save(output/f"{s['case']}_{mode}.png")
                baseline.save(output/f"{s['case']}_none.png")
                assert maximum<=1,(s,mode,maximum,difference.getbbox())
    (output/"validation.json").write_text(json.dumps(results,indent=2))
    print(f"PASS: {len(results)} scene-color comparisons, maximum one 8-bit channel level")


if __name__=="__main__":
    main()
