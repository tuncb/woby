# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Check cell quality, both metric buffers, cell focus, and scene reload."""
import math
import json
import os
from pathlib import Path
import sys
import tempfile
import time

from PIL import ImageChops
from ctl_headless_smoke import capture, session


def main():
    executable = Path(sys.argv[1]).resolve()
    reference = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1),
                 (.5, 0, 0), (.5, .5, 0), (0, .5, 0), (0, 0, .5), (.5, 0, .5), (0, .5, .5)]
    with tempfile.TemporaryDirectory(prefix="woby tet quality ") as directory:
        root = Path(directory).resolve()
        points, cells, types = [], [], []
        # One linear regular cell, one curved positive cell, and a folded cell.
        for cell in range(3):
            start = len(points)
            for r, s, t in reference[:4 if cell == 0 else 10]:
                if cell == 0:
                    p = (r + .5*s + .5*t, math.sqrt(3)*s/2 + math.sqrt(3)*t/6, math.sqrt(2/3)*t)
                elif cell == 1:
                    p = (r, s, t*(1 + .25*r))
                else:
                    p = (r, s, t*(1 - 2*r))
                points.append((p[0]+cell*2, p[1], p[2]))
            count = 4 if cell == 0 else 10
            cells.append(list(range(start, start+count)))
            types.append(10 if cell == 0 else 24)
        model = root / "cells.vtk"
        text = ["# vtk DataFile Version 3.0", "Cell quality", "ASCII", "DATASET UNSTRUCTURED_GRID", f"POINTS {len(points)} double"]
        text.extend(" ".join(map(str, point)) for point in points)
        text.append(f"CELLS {len(cells)} {sum(len(cell)+1 for cell in cells)}")
        text.extend(" ".join(map(str, [len(cell), *cell])) for cell in cells)
        text.append(f"CELL_TYPES {len(types)}")
        text.extend(map(str, types))
        model.write_text("\n".join(text)+"\n", encoding="utf-8")
        saved = root / "quality.woby"
        with session(executable, root, os.environ) as (ctl, viewer):
            ctl("model", "add", model)
            file_id = next(o["id"] for o in ctl("objects")["objects"] if o["kind"] == "file")
            analysis = ctl("analysis", "create", "--a", file_id)["target"]
            ctl("transform", "set", analysis, "--translation", 0, 0, 0)
            ctl("visibility", "set", file_id, "--visible", "false")
            for helper in ("grid", "origin", "dimensions"):
                ctl(helper, "set", "--visible", "false")
            ctl("analysis", "set", analysis, "--mode", "surface_quality", "--tetrahedral", "true",
                "--tetrahedral-metric", "mean_ratio", "--show-edges", "true")
            result = ctl("analysis", "results", analysis)
            quality = result["aToB"]["tetrahedralMeshQuality"]
            assert quality["cellCount"] == 3 and quality["quadraticCells"] == 2, quality
            assert quality["validity"]["positive"] == 2 and quality["validity"]["folded"] == 1, quality
            page = ctl("analysis", "findings", analysis, "--side", "a", "--detector", "tetrahedral_quality", "--limit", 2)
            assert len(page["items"]) == 2 and page["nextOffset"] == 2, page
            tail = ctl("analysis", "findings", analysis, "--side", "a", "--detector", "tetrahedral_quality", "--offset", 2, "--revision", page["revision"])
            assert tail["items"][0]["validity"] == "folded" and tail["nextOffset"] is None, tail
            exported = root / "cells.json"
            ctl("analysis", "export", analysis, "--path", exported)
            deadline = time.monotonic() + 15
            while (job := ctl("analysis", "export-status")["export"])["state"] == "running":
                assert time.monotonic() < deadline
                time.sleep(.025)
            assert job["state"] == "complete", job
            full = json.loads(exported.read_text(encoding="utf-8"))
            assert full["aToB"]["tetrahedralCells"] == page["items"] + tail["items"]
            ctl("camera", "frame", "--object", analysis)
            first = capture(ctl, root/"shape.png")
            ctl("analysis", "set", analysis, "--tetrahedral-metric", "jacobian_ratio")
            second = capture(ctl, root/"ratio.png")
            assert ImageChops.difference(first, second).getbbox(), "Changing metric buffers did not change the image"
            ctl("analysis", "focus", analysis, "--side", "a", "--detector", "tetrahedral_quality", "--index", 2)
            ctl("analysis", "set", analysis, "--tetrahedral-selected-only", "true")
            selected = capture(ctl, root/"selected.png")
            assert ImageChops.difference(second, selected).getbbox(), "Cell isolation did not change the image"
            ctl("analysis", "set", analysis, "--tetrahedral-minimum-enabled", "true", "--tetrahedral-minimum", .3)
            ctl("scene", "save-as", saved)
        with session(executable, root, os.environ, "--scene", saved) as (ctl, viewer):
            analysis = next(o["id"] for o in ctl("objects")["objects"] if o["kind"] == "analysis")
            result = ctl("analysis", "results", analysis)
            assert result["aToB"]["tetrahedralMeshQuality"]["validity"] == quality["validity"]
            capture(ctl, root/"reloaded.png")
    print("Tetrahedral quality rendering and automation passed")


if __name__ == "__main__":
    main()
