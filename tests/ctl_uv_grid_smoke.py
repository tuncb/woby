# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Verify UV grid pixels, density, up axis, missing UVs, undo, and scene reload.

An optional second argument writes a persistent curved-patch demo and screenshots.
"""
import math
import os
from pathlib import Path
import sys
import tempfile

from PIL import ImageChops

from ctl_headless_smoke import capture, session


def grid_pixels(image):
    colors = image.getcolors(image.width * image.height)
    cyan = sum(n for n, (r, g, b) in colors if g > r + 50 and b > r + 70)
    orange = sum(n for n, (r, g, b) in colors if r > g + 60 and g > b + 35)
    return cyan, orange


def prepare(ctl, model):
    ctl("model", "add", model)
    groups = [item["id"] for item in ctl("objects")["objects"] if item["kind"] == "group"]
    for group in groups:
        ctl("color", "set", group, "--rgb", .42, .46, .52)
    for helper in ("grid", "origin", "dimensions"):
        ctl(helper, "set", "--visible", "false")
    return groups


def demo(executable, output, env):
    output.mkdir(parents=True, exist_ok=True)
    model = output / "uv-patches.obj"
    lines = []
    segments = 48
    for patch, center in enumerate((1.3, -1.3)):
        lines.append("g " + ("Uniform_UV" if patch == 0 else "Stretched_UV"))
        for j in range(segments + 1):
            v = j / segments
            for i in range(segments + 1):
                u = i / segments
                z = .6 * math.sin(math.pi * u) * math.sin(math.pi * v)
                lines.append(f"v {center + 2*u-1:.8f} {2*v-1:.8f} {z:.8f}")
                lines.append(f"vt {u if patch == 0 else u**2.5:.8f} {v:.8f}")
        offset = patch * (segments + 1)**2
        for j in range(segments):
            for i in range(segments):
                a = offset + j * (segments + 1) + i + 1
                b, c, d = a + 1, a + segments + 2, a + segments + 1
                lines.append(f"f {a}/{a} {b}/{b} {c}/{c}")
                lines.append(f"f {a}/{a} {c}/{c} {d}/{d}")
    model.write_text("\n".join(lines) + "\n", encoding="utf-8")
    with session(executable, output, env) as (ctl, viewer):
        prepare(ctl, model)
        ctl("up-axis", "set", "z")
        ctl("camera", "look-at", "--eye", 0, -4.8, 6.6, "--target", 0, 0, .15)
        ctl("camera", "set", "--distance", 5.7, "--fov-degrees", 50)
        capture(ctl, output / "01-solid.png")
        ctl("render", "set", "scene", "--uv-grid", "true", "--uv-density-u", 8, "--uv-density-v", 8)
        capture(ctl, output / "02-uv-grid.png")
        ctl("render", "set", "scene", "--uv-density-u", 16, "--uv-density-v", 8)
        capture(ctl, output / "03-denser-u.png")
        ctl("render", "set", "scene", "--uv-density-u", 8)
        ctl("scene", "save-as", output / "uv-grid.woby", "--overwrite")
        ctl("quit")
        assert viewer.wait(timeout=15) == 0


def analysis_demo(executable, output, env):
    output.mkdir(parents=True, exist_ok=True)
    model = output / "uv-analysis-source.obj"
    lines = ["g Curved_mesh"]
    segments = 24
    for j in range(segments + 1):
        v = j / segments
        for i in range(segments + 1):
            u = i / segments
            z = .8 * math.sin(math.pi * u) * math.sin(math.pi * v)
            lines.append(f"v {2*u-1:.8f} {2*v-1:.8f} {z:.8f}")
            lines.append(f"vt {u**1.8:.8f} {v:.8f}")
    for j in range(segments):
        for i in range(segments):
            a = j * (segments + 1) + i + 1
            b, c, d = a + 1, a + segments + 2, a + segments + 1
            lines.extend((f"f {a}/{a} {b}/{b} {c}/{c}", f"f {a}/{a} {c}/{c} {d}/{d}"))
    model.write_text("\n".join(lines) + "\n", encoding="utf-8")
    with session(executable, output, env) as (ctl, viewer):
        groups = prepare(ctl, model)
        analysis = ctl("analysis", "create", "--type", "uv", "--name", "UV inspection", "--a", groups[0])["target"]
        # This camera sees +X on the left: put the original there and the UV view on the right.
        ctl("transform", "set", analysis, "--translation", -2.65, 0, 0)
        ctl("up-axis", "set", "z")
        ctl("camera", "look-at", "--eye", -1.325, -4.8, 6.6, "--target", -1.325, 0, .25)
        ctl("camera", "set", "--distance", 5.7, "--fov-degrees", 50)
        ctl("analysis", "set", analysis, "--uv-view", "surface", "--show-edges", "false",
            "--uv-density-u", 8, "--uv-density-v", 8)
        surface = capture(ctl, output / "04-separate-3d-uv.png")
        ctl("scene", "save-as", output / "uv-analysis-surface.woby", "--overwrite")
        ctl("analysis", "set", analysis, "--uv-view", "layout", "--show-edges", "true")
        layout = capture(ctl, output / "05-separate-uv-layout.png")
        assert all(count > 1000 for count in grid_pixels(surface)), grid_pixels(surface)
        assert all(count > 1000 for count in grid_pixels(layout)), grid_pixels(layout)
        assert ImageChops.difference(surface, layout).getbbox()
        uv_results = ctl("analysis", "results", analysis)
        assert uv_results["aToB"]["triangleCount"] > 0
        assert uv_results["aToB"]["sampleCount"] == 0 and uv_results["aToB"]["maximum"] is None
        assert uv_results["bToA"] is None
        source_area = (0, 0, 500, layout.height)
        assert ImageChops.difference(surface.crop(source_area), layout.crop(source_area)).getbbox() is None
        ctl("scene", "undo")
        assert ImageChops.difference(surface, capture(ctl, output / "analysis-undo.png")).getbbox() is None
        ctl("scene", "redo")
        assert ImageChops.difference(layout, capture(ctl, output / "analysis-redo.png")).getbbox() is None
        ctl("analysis", "set", analysis, "--mode", "distance", code=-32602)
        ctl("analysis", "swap", analysis, code=-32602)
        ctl("scene", "save-as", output / "uv-analysis-layout.woby", "--overwrite")
        ctl("quit")
        assert viewer.wait(timeout=15) == 0
    with session(executable, output, env, "--scene", output / "uv-analysis-layout.woby") as (ctl, viewer):
        assert ImageChops.difference(layout, capture(ctl, output / "analysis-reloaded.png")).getbbox() is None
        ctl("quit")
        assert viewer.wait(timeout=15) == 0


def layout_orientation(executable, root, model, env):
    saved = root / "uv-layout-orientation.woby"
    with session(executable, root, env) as (ctl, viewer):
        groups = prepare(ctl, model)
        analysis = ctl("analysis", "create", "--type", "uv", "--a", groups[0])["target"]
        ctl("analysis", "set", analysis, "--show-edges", "false",
            "--uv-density-u", 4, "--uv-density-v", 4)
        for group in groups:
            ctl("visibility", "set", group, "--visible", "false")
        # Revisit each axis after GPU upload to exercise cached-geometry refresh.
        for index, axis in enumerate(("y", "z", "y", "z")):
            ctl("up-axis", "set", axis)
            ctl("camera", "view", "front")
            ctl("camera", "frame", "--object", analysis)
            image = capture(ctl, root / f"layout-{index}-{axis}-up.png")
            assert all(count > 1000 for count in grid_pixels(image)), (axis, grid_pixels(image))
        ctl("scene", "save-as", saved)
        ctl("quit")
        assert viewer.wait(timeout=15) == 0
    with session(executable, root, env, "--scene", saved) as (ctl, viewer):
        assert ImageChops.difference(image, capture(ctl, root / "layout-orientation-reloaded.png")).getbbox() is None
        ctl("quit")
        assert viewer.wait(timeout=15) == 0


def inspection_demo(executable, output, env):
    output.mkdir(parents=True, exist_ok=True)
    model = output / "patch-inspection.obj"
    lines = []
    n = 16
    for patch, center in enumerate((1.3, -1.3)):
        lines.append("g " + ("Uniform" if patch == 0 else "Stretched"))
        for j in range(n + 1):
            v = j / n
            for i in range(n + 1):
                u = i / n
                lines.append(f"v {center + 2*u-1} {.45*math.sin(math.pi*u)*math.sin(math.pi*v)} {2*v-1}")
                lines.append(f"vt {u if patch == 0 else u**2.5} {v}")
        offset = patch*(n+1)**2
        for j in range(n):
            for i in range(n):
                a = offset+j*(n+1)+i+1
                b,c,d = a+1,a+n+2,a+n+1
                lines.extend((f"f {a}/{a} {b}/{b} {c}/{c}",f"f {a}/{a} {c}/{c} {d}/{d}"))
    model.write_text("\n".join(lines)+"\n",encoding="utf-8")
    saved = output / "uv-inspection.woby"
    with session(executable, output, env) as (ctl, viewer):
        groups = prepare(ctl, model)
        file_id = next(o["id"] for o in ctl("objects")["objects"] if o["kind"] == "file")
        ctl("up-axis","set","z")
        quality = ctl("analysis","create","--type","uv_quality","--a",file_id,"--name","UV quality")["target"]
        ctl("transform","set",quality,"--translation",0,0,0)
        layout = ctl("analysis","create","--type","uv","--a",file_id,"--name","Patch UV layout")["target"]
        ctl("analysis","set",layout,"--uv-separated","true","--uv-color","v","--show-edges","false")
        ctl("transform","set",layout,"--translation",-8,0,0)
        ctl("visibility","set",file_id,"--visible","false")
        ctl("camera","look-at","--eye",-5.3,-32,4,"--target",-5.3,0,0)
        ctl("camera","set","--distance",32,"--fov-degrees",50)
        angle = capture(ctl,output / "06-angle-and-uv.png")
        results = ctl("analysis","results",quality)["uvQuality"]
        assert results["validTriangles"] == 4*n*n, results
        assert results["maximumAngleDegrees"] > 40, results
        assert results["mixedOrientationPatches"] == 0, results
        ctl("analysis","set",quality,"--uv-metric","area")
        area = capture(ctl,output / "07-area-and-uv.png")
        scene_area = (0,0,1000,angle.height)
        assert ImageChops.difference(angle.crop(scene_area),area.crop(scene_area)).getbbox()
        ctl("analysis","set",layout,"--uv-color","u","--uv-minimum",-.5,"--uv-maximum",1.5)
        gradient = capture(ctl,output / "08-u-gradient.png")
        assert ImageChops.difference(area.crop(scene_area),gradient.crop(scene_area)).getbbox()
        ctl("scene","undo")
        assert ImageChops.difference(area,capture(ctl,output / "gradient-undo.png")).getbbox() is None
        ctl("scene","redo")
        assert ImageChops.difference(gradient,capture(ctl,output / "gradient-redo.png")).getbbox() is None
        ctl("analysis","enable",quality,"--side","a","--object",groups[1],"--enabled","true","--isolate","true")
        assert ctl("analysis","results",quality)["uvQuality"]["validTriangles"] == 2*n*n
        ctl("analysis","enable",quality,"--side","a","--enabled","true")
        ctl("analysis","set",quality,"--uv-grid","true",code=-32602)
        ctl("analysis","set",layout,"--uv-metric","area",code=-32602)
        ctl("scene","save-as",saved,"--overwrite")
        expected = capture(ctl,output / "09-final.png")
        for analysis in (quality, layout):
            ctl("analysis","enable",analysis,"--side","a","--object",groups[1],"--enabled","true","--isolate","true")
        ctl("transform","set",layout,"--translation",-3,0,0)
        ctl("camera","look-at","--eye",-2.8,-10.5,1.2,"--target",-2.8,0,0)
        ctl("camera","set","--distance",10.5,"--fov-degrees",50)
        isolated = capture(ctl,output / "10-isolated-patch.png")
        assert sum(count for count,(r,g,b) in isolated.getcolors(isolated.width*isolated.height) if r > 200 and g < 100 and b < 60) > 500
        ctl("scene","save-as",output / "uv-isolated.woby","--overwrite")
        ctl("quit")
        assert viewer.wait(timeout=15) == 0
    with session(executable,output,env,"--scene",saved) as (ctl,viewer):
        assert ImageChops.difference(expected,capture(ctl,output / "inspection-reloaded.png")).getbbox() is None
        ctl("quit")
        assert viewer.wait(timeout=15) == 0


def main():
    executable = Path(sys.argv[1]).resolve()
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    with tempfile.TemporaryDirectory(prefix="woby uv grid ") as directory:
        root = Path(directory).resolve()
        model = root / "parts.obj"
        model.write_text("""v -2 -1 0
v -.2 -1 0
v -.2 1 0
v -2 1 0
v .2 -1 0
v 2 -1 0
v 2 1 0
v .2 1 0
vt -.25 0
vt 1.75 0
vt 1.75 1
vt -.25 1
g supplied
f 1/1 2/2 3/3
f 1/1 3/3 4/4
g missing
f 5 6 7
f 5 7 8
""", encoding="utf-8")
        saved = root / "saved.woby"
        with session(executable, root, env) as (ctl, viewer):
            groups = prepare(ctl, model)
            ctl("up-axis", "set", "y")
            ctl("camera", "look-at", "--eye", 0, 0, 7, "--target", 0, 0, 0)
            baseline = capture(ctl, root / "solid.png")
            ctl("render", "set", "scene", "--uv-grid", "true", "--uv-density-u", 4, "--uv-density-v", 4)
            grid = capture(ctl, root / "grid.png")
            assert all(count > 1000 for count in grid_pixels(grid)), grid_pixels(grid)
            no_uv = (0, 0, grid.width // 2, grid.height)
            assert ImageChops.difference(grid.crop(no_uv), baseline.crop(no_uv)).getbbox() is None
            ctl("render", "set", groups[1], "--uv-grid", "true", code=-32602)
            ctl("render","set",groups[0],"--uv-color","u","--uv-minimum",-.25,"--uv-maximum",1.75)
            u_color = capture(ctl,root / "source-u.png")
            ctl("render","set",groups[0],"--uv-color","v")
            v_color = capture(ctl,root / "source-v.png")
            assert ImageChops.difference(u_color,v_color).getbbox()
            assert ImageChops.difference(u_color.crop(no_uv),baseline.crop(no_uv)).getbbox() is None
            ctl("render","set",groups[0],"--uv-color","grid")
            ctl("render", "set", groups[0], "--uv-density-u", 8)
            dense = capture(ctl, root / "dense.png")
            assert grid_pixels(dense)[0] > grid_pixels(grid)[0] * 1.4
            ctl("scene", "undo")
            assert ImageChops.difference(grid, capture(ctl, root / "undo.png")).getbbox() is None
            ctl("scene", "redo")
            assert ImageChops.difference(dense, capture(ctl, root / "redo.png")).getbbox() is None
            ctl("render", "set", "scene", "--uv-grid", "false")
            assert ImageChops.difference(baseline, capture(ctl, root / "off.png")).getbbox() is None
            ctl("render", "set", "scene", "--uv-grid", "true")
            ctl("scene", "save-as", saved)
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
        with session(executable, root, env, "--scene", saved) as (ctl, viewer):
            assert ImageChops.difference(dense, capture(ctl, root / "restored.png")).getbbox() is None
            ctl("quit")
            assert viewer.wait(timeout=15) == 0
        analysis_demo(executable, root / "analysis", env)
        inspection_demo(executable, root / "inspection", env)
        layout_orientation(executable, root, model, env)
    print("UV grid render passed: U/V lines, density, up-axis changes, missing UV fallback, disable, undo/redo, and reload.")
    if len(sys.argv) > 2:
        inspection_demo(executable, Path(sys.argv[2]).resolve() / "inspection", env)
        demo(executable, Path(sys.argv[2]).resolve(), env)
        analysis_demo(executable, Path(sys.argv[2]).resolve(), env)
        print("Demo screenshots and scene:", Path(sys.argv[2]).resolve())


if __name__ == "__main__":
    main()
