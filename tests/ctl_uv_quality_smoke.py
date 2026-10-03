# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Check all six UV diagnostics through the headless CLI, including rendered colors."""
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile

from PIL import ImageChops
from ctl_headless_smoke import capture, session


def stable_results(value):
    # Session handles intentionally change on reload; retain the stable numeric IDs.
    if isinstance(value, dict):
        return {k: stable_results(v) for k, v in value.items()
                if k not in {"sourceObject", "firstObject", "secondObject"}}
    if isinstance(value, list):
        return [stable_results(v) for v in value]
    return value


def triangles(ctl, analysis):
    page = ctl("analysis", "uv-triangles", analysis, "--limit", 2)
    rows, revision = page["items"], page["revision"]
    while page["nextOffset"] is not None:
        page = ctl("analysis", "uv-triangles", analysis, "--offset", page["nextOffset"],
                   "--limit", 2, "--revision", revision)
        assert page["revision"] == revision
        rows.extend(page["items"])
    assert len(rows) == page["total"] == 7, page
    return rows, revision


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    with tempfile.TemporaryDirectory(prefix="woby uv quality ") as directory:
        root = Path(directory).resolve()
        model = root / "quality.obj"
        lines = []
        scales = [(.5, .5), (2, 2), (4, .25), (1, 1/1024), (0, 0), (1, 1), (1, 1)]
        for i, (u, v) in enumerate(scales):
            if i != 6:
                lines.append(f"g patch_{i}")
            x, y = (i % 4) * 1.5, (i // 4) * 1.5
            lines.extend((f"v {x} {y} 0", f"v {x+1} {y} 0", f"v {x} {y+1} 0"))
            lines.extend(("vt 0 0", f"vt {u} 0", f"vt 0 {v}"))
            a = 3*i+1
            lines.append(f"f {a}/{a} {a+1}/{a+1} {a+2}/{a+2}")
        model.write_text("\n".join(lines)+"\n", encoding="utf-8")
        saved = root / "quality.woby"
        with session(executable, root, os.environ) as (ctl, viewer):
            methods = {m["method"] for m in ctl("capabilities")["methods"]}
            assert {"analysis.uv-triangles", "analysis.uv-probe", "analysis.uv-probe-get",
                    "analysis.uv-probe-clear"} <= methods
            ctl("model", "add", model)
            file_id = next(o["id"] for o in ctl("objects")["objects"] if o["kind"] == "file")
            analysis = ctl("analysis", "create", "--type", "uv_quality", "--a", file_id)["target"]
            ctl("transform", "set", analysis, "--translation", 0, 0, 0)
            ctl("visibility", "set", file_id, "--visible", "false")
            for helper in ("grid", "origin", "dimensions"):
                ctl(helper, "set", "--visible", "false")
            ctl("up-axis", "set", "z")
            ctl("camera", "view", "top")
            ctl("camera", "frame", "--object", analysis)
            ctl("analysis", "set", analysis, "--uv-metric", "area", "--uv-normalization", "absolute")
            q = ctl("analysis", "results", analysis)["uvQuality"]
            assert q["convention"] == "surface_to_uv" and q["normalization"] == "absolute"
            faces, area_revision = triangles(ctl, analysis)
            assert math.isclose(faces[0]["areaLog2"], -2) and math.isclose(faces[1]["areaLog2"], 2)
            assert faces[4]["collapsedUv"] and faces[4]["value"] is None
            area = capture(ctl, root / "area.png")
            colors = area.getcolors(area.width*area.height)
            blue = sum(n for n, (r, g, b) in colors if b > r+60 and b > g+20)
            red = sum(n for n, (r, g, b) in colors if r > b+80 and r > g+40)
            assert blue > 1000 and red > 1000, (blue, red)
            ctl("analysis", "set", analysis, "--uv-metric", "anisotropy", "--uv-threshold", 3,
                "--uv-threshold-enabled", "true")
            q = ctl("analysis", "results", analysis)["uvQuality"]
            faces, _ = triangles(ctl, analysis)
            assert math.isclose(faces[2]["anisotropy"], 16)
            assert math.isclose(faces[3]["anisotropy"], 1024, rel_tol=1e-6), faces[3]
            assert sum(t["thresholdExceeded"] for t in faces) == 2
            assert sum(t["highlighted"] for t in faces) == q["statistics"]["highlightedCount"] == 2
            ctl("analysis", "uv-triangles", analysis, "--revision", area_revision, code=-32602)
            stretch = capture(ctl, root / "anisotropy.png")
            assert ImageChops.difference(area, stretch).getbbox()
            q = ctl("analysis", "results", analysis)["uvQuality"]
            assert q["statistics"]["count"] == 6 and q["collapsedTriangles"] == 1, q
            assert q["statistics"]["thresholdCount"] == 2, q
            assert sum(q["histogram"]["counts"]) == 6
            assert math.isclose(sum(q["histogram"]["surfaceAreas"]), 3)
            assert math.isclose(q["statistics"]["thresholdAreaPercent"], 100/3)
            ctl("analysis", "set", analysis, "--uv-metric", "min_stretch", "--uv-threshold", .01,
                "--uv-near-collapse", .01, "--uv-threshold-enabled", "false")
            q = ctl("analysis", "results", analysis)["uvQuality"]
            faces, _ = triangles(ctl, analysis)
            assert math.isclose(faces[3]["minStretch"], 1/1024, rel_tol=1e-6)
            assert [i for i, t in enumerate(faces) if t["nearCollapse"]] == [3]
            assert q["statistics"]["highlightedCount"] == 0
            minimum = capture(ctl, root / "minimum-stretch.png")
            assert ImageChops.difference(area, minimum).getbbox()
            q = ctl("analysis", "results", analysis)["uvQuality"]
            assert q["statistics"]["nearCollapseCount"] == 1, q
            ctl("analysis", "set", analysis, "--uv-metric", "overlap", "--uv-overlap-scope", "per_patch")
            ctl("analysis", "results", analysis)
            capture(ctl, root / "overlap.png")
            q = ctl("analysis", "results", analysis)["uvQuality"]
            assert q["overlaps"]["checked"] and not q["overlaps"]["truncated"], q
            assert len(q["overlaps"]["pairs"]) == 1 and q["overlaps"]["crossPatchPairs"] == 0, q
            pair = q["overlaps"]["pairs"][0]
            picked = ctl("analysis", "uv-probe", analysis, "--object", pair["firstObject"],
                         "--index", pair["firstTriangle"])["probe"]
            assert picked["overlapping"] and picked["sourcePartId"] == pair["firstObject"]
            ctl("analysis", "set", analysis, "--uv-overlap-scope", "selected_patches")
            ctl("analysis", "results", analysis)
            capture(ctl, root / "shared-domain.png")
            q = ctl("analysis", "results", analysis)["uvQuality"]
            assert q["overlaps"]["crossPatchPairs"] > 0, q
            ctl("analysis", "set", analysis, "--uv-metric", "anisotropy", "--uv-range-enabled", "true",
                "--uv-range-minimum", 10, "--uv-range-maximum", 20)
            q = ctl("analysis", "results", analysis)["uvQuality"]
            faces, _ = triangles(ctl, analysis)
            assert [i for i, t in enumerate(faces) if t["highlighted"]] == [2]
            assert q["statistics"]["highlightedCount"] == 1
            assert math.isclose(q["statistics"]["highlightedAreaPercent"], 100/6)
            part = faces[2]["sourcePartId"]
            # A precise barycentric point links UVs to the original surface and its analysis copy.
            ctl("scene", "save-as", root / "probe-check.woby")
            assert not ctl("status")["dirty"]
            probe = ctl("analysis", "uv-probe", analysis, "--object", part, "--index", 1,
                        "--barycentric", .5, .2, .3)["probe"]
            # Preserve the importer's existing V-flip convention.
            assert all(math.isclose(a, b) for a, b in zip(probe["uv"], [.8, .925]))
            assert math.isclose(probe["surfacePosition"][0] + probe["coordinateOrigin"][0], 3.2)
            assert math.isclose(probe["surfacePosition"][1] + probe["coordinateOrigin"][1], .3)
            assert probe["surfacePosition"] == probe["displayPosition"]
            assert ctl("analysis", "uv-probe-get", analysis)["probe"] == probe
            assert not ctl("status")["dirty"]
            ctl("analysis", "uv-probe", analysis, "--object", part, "--index", 999, code=-32602)
            assert ctl("analysis", "uv-probe-get", analysis)["probe"] == probe
            assert ctl("analysis", "uv-probe-clear", analysis)["probe"] is None
            assert ctl("analysis", "uv-probe-get", analysis)["probe"] is None
            # Layout and separated copies preserve the same source point and UV.
            ctl("analysis", "set", analysis, "--uv-view", "layout", "--uv-separated", "true")
            ctl("analysis", "results", analysis)
            layout_probe = ctl("analysis", "uv-probe", analysis, "--object", part, "--index", 1,
                               "--barycentric", .5, .2, .3)["probe"]
            assert layout_probe["uv"] == probe["uv"]
            assert layout_probe["surfacePosition"] == probe["surfacePosition"]
            assert layout_probe["displayPosition"] != probe["displayPosition"]
            ctl("analysis", "set", analysis, "--uv-linked-selection", "false")
            stale = ctl("analysis", "uv-probe-get", analysis)
            assert stale["stale"] and stale["probe"] is None
            ctl("analysis", "results", analysis)
            ctl("analysis", "uv-probe", analysis, "--object", part, "--index", 1, code=-32602)
            ctl("analysis", "uv-probe-clear", analysis)
            ctl("analysis", "set", analysis, "--uv-linked-selection", "true",
                "--uv-view", "surface", "--uv-separated", "false")
            ctl("analysis", "results", analysis)
            # Probe a disabled source part; other populated parts keep the analysis valid.
            ctl("analysis", "enable", analysis, "--side", "a", "--object", part, "--enabled", "false")
            ctl("analysis", "results", analysis)
            ctl("analysis", "uv-probe", analysis, "--object", part, "--index", 1, code=-32602)
            ctl("analysis", "enable", analysis, "--side", "a", "--object", part, "--enabled", "true")
            ctl("analysis", "results", analysis)
            highlighted = capture(ctl, root / "range.png")
            before = ctl("analysis", "results", analysis)["uvQuality"]
            ctl("analysis", "uv-probe", analysis, "--object", part, "--index", 1)
            ctl("scene", "save-as", saved)
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
        with session(executable, root, os.environ, "--scene", saved) as (ctl, viewer):
            reloaded = capture(ctl, root / "reloaded.png")
            analysis = next(o["id"] for o in ctl("objects")["objects"] if o["kind"] == "analysis")
            after = ctl("analysis", "results", analysis)["uvQuality"]
            assert stable_results(before) == stable_results(after), (before, after)
            assert ctl("analysis", "uv-probe-get", analysis)["probe"] is None
            triangles(ctl, analysis)
            # Compare geometry exactly. The legend's font-atlas edge filtering
            # can differ by a few pixels across fresh processes; compare its
            # underlying measurements exactly above instead.
            geometry = (0, 0, highlighted.width // 2, highlighted.height)
            assert ImageChops.difference(highlighted.crop(geometry), reloaded.crop(geometry)).getbbox() is None
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
        if output:
            output.mkdir(parents=True, exist_ok=True)
            for image in root.glob("*.png"):
                shutil.copy2(image, output / image.name)
            (output / "results.json").write_text(json.dumps(q, indent=2), encoding="utf-8")
    print("All six UV diagnostics passed CLI and GPU checks")


if __name__ == "__main__":
    main()
