# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Paired viewer load and first capture measurements. Run after both builds finish."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sys
import time

from PIL import Image, ImageChops, ImageStat

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from ctl_headless_smoke import capture, session


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def measure(executable, model, root):
    root.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SDL_VIDEO_DRIVER="woby-test-no-video-driver")
    with session(executable, root, env) as (ctl, viewer):
        for helper in ("grid", "origin", "dimensions"):
            ctl(helper, "set", "--visible", "false")
        start = time.perf_counter()
        result = ctl("model", "add", model)
        loaded = time.perf_counter()
        assert result["addedIds"] and result["failedCount"] == 0, result
        capture(ctl, root / "first.png")
        captured = time.perf_counter()
        stats = ctl("stats")
        performance = ctl("performance", "get")
        camera = ctl("camera", "get")
        status = ctl("status")
        ctl("quit", "--on-dirty", "discard")
        assert viewer.wait(timeout=15) == 0
    return dict(add_ms=(loaded-start)*1000, first_capture_ms=(captured-loaded)*1000,
                add_through_capture_ms=(captured-start)*1000, result=result, stats=stats,
                performance=performance, camera=camera, status=status)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("models", type=Path, nargs="+")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    executables = dict(before=args.before.resolve(), after=args.after.resolve())
    report = dict(executables={k: dict(path=str(v), sha256=sha(v)) for k, v in executables.items()},
                  recorded_at=datetime.now(timezone.utc).isoformat(),
                  script_sha256=sha(Path(__file__)),
                  scope="Wall time for model.add through completed first screenshot. Includes RPC and PNG readback; excludes viewer startup. Alternating build order.",
                  runs=[], comparisons=[])
    for index, source in enumerate(args.models):
        model = source.resolve()
        for repeat in range(args.repeats):
            order = ("before", "after") if repeat % 2 == 0 else ("after", "before")
            for variant in order:
                root = args.output / f"model-{index}-{repeat}-{variant}"
                measured = measure(executables[variant], model, root)
                report["runs"].append(dict(model=str(model), model_sha256=sha(model),
                                          repeat=repeat, variant=variant, **measured))
                (args.output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
                print(model.name, repeat, variant, round(measured["add_through_capture_ms"], 1), flush=True)
            with Image.open(args.output / f"model-{index}-{repeat}-before/first.png") as a, Image.open(args.output / f"model-{index}-{repeat}-after/first.png") as b:
                delta = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
                changed = sum(count for count, rgb in delta.getcolors(delta.width*delta.height) if rgb != (0, 0, 0))
                report["comparisons"].append(dict(model=str(model), repeat=repeat,
                    changed_pixels=changed, total_pixels=delta.width*delta.height,
                    mean_absolute_channel_difference=ImageStat.Stat(delta).mean))
                delta.save(args.output / f"model-{index}-{repeat}-difference.png")
            (args.output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
