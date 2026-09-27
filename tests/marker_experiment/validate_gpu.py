# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Deterministic real-GPU raster/occlusion/alpha/highlight/coordinate checks."""
import argparse
import json
from pathlib import Path
import tempfile

from PIL import Image, ImageChops
from run import execute


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    artifacts = args.output.resolve()
    artifacts.mkdir(parents=True, exist_ok=False)
    report = []
    with tempfile.TemporaryDirectory(prefix="woby marker fixture ") as directory:
        root = Path(directory).resolve()
        for occluder in (False, True):
            model = root / ("occluder.obj" if occluder else "overlap.obj")
            front = "v -1 -1 1\nv 1 -1 1\nv 0 1 1\n" if occluder else "v 0 0 1\nv -1 1 1\nv 1 1 1\n"
            model.write_text("v 0 0 0\nv -1 -1 0\nv 1 -1 0\n" + front + "g rear\nf 1 2 3\ng front\nf 4 5 6\n")
            cases = [dict(case="opaque"), dict(case="single", msaa=False), dict(case="alpha", opacity=.4),
                     dict(case="zero", opacity=0), dict(case="hidden", visible=False), dict(case="rear", hide_group=1),
                     dict(case="solid", solid=True), dict(case="alpha_solid",solid=True,opacity=.4), dict(case="translated", translate=True),
                     dict(case="large", point_size=24), dict(case="pane", pane=True),
                     dict(case="resized", width=800, height=600)]
            choices = [dict(name=c["case"]+"_id_async", vertices=True, solid=False, pointer="center", picker="id_async", point_size=12) | c for c in cases]
            marker = execute(args.binary.resolve(), model, root, choices, 0, True, captures=True, fixture=True)
            for i, case in enumerate(cases):
                completions = [c for c in marker["completions"] if c["scenario"] == i]
                assert len(completions) == 10, (model, case, len(completions))
                expect_hit = case["case"] not in ("zero", "hidden") and not (occluder and case["case"] in ("solid","alpha_solid"))
                assert all(bool(c["id"]) == expect_hit for c in completions), (model, case, [c["id"] for c in completions])
                if expect_hit:
                    z = 0 if occluder or case["case"] == "rear" else 1
                    assert all(c["local"] == [0, 0, z] for c in completions), (model, case, [c["local"] for c in completions])
                    if case["case"] == "translated":
                        offsets = [c["world"][2] - c["local"][2] for c in completions]
                        assert all(abs(value - step * .1) < 1e-5 for step, value in enumerate(offsets)), offsets
                    else:
                        assert all(c["local"] == c["world"] for c in completions)
                report.append(dict(model=model.name, case=case["case"], requests=len(completions), hits=sum(bool(c["id"]) for c in completions)))
            for source in root.glob(model.stem + "-1*.tga"):
                im = Image.open(source).convert("RGB")
                im.save(artifacts / (source.name.removesuffix(".tga.tga") + ".png"))
            (artifacts / (model.stem + "-results.json")).write_text(json.dumps(marker))
        # The early capture is taken before the six-frame coordinate readback.
        # It must already contain the GPU-selected orange marker.
        images = sorted(artifacts.glob("overlap-1-opaque_id_async-frame*.png"))
        assert len(images) == 2, images
        for path in images:
            im = Image.open(path).convert("RGB")
            orange = sum(count for count,(r,g,b) in im.getcolors(im.width*im.height)
                         if r > 230 and 135 < g < 205 and b < 55)
            assert orange >= 40, (path, orange)
        early, late = (Image.open(p).convert("RGB") for p in images)
        # Coordinate panel appears once asynchronous results have arrived.
        assert ImageChops.difference(early.crop((700,550,1280,720)), late.crop((700,550,1280,720))).getbbox()
    (artifacts / "validation.json").write_text(json.dumps(report, indent=2))
    print(f"PASS: {len(report)} cases, {sum(r['requests'] for r in report)} GPU requests; early highlight and later coordinate panel")


if __name__ == "__main__":
    main()
