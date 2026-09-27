"""Contract tests for benchmark data analysis; no renderer timing assertions."""
import csv
import json
from pathlib import Path
import tempfile
import unittest
from run import percentile, summarize_run
from summarize import aggregate


def fixture(root, count):
    output = root / "run"
    output.with_suffix(".json").write_text(json.dumps(dict(scenarios=[dict(name="test")], min_frames=30)))
    fields = "scenario measured frame gpu_frame draws width height wall_ms time_ms total_ms gpu_ms render_cpu_ms events pending_io state_update imgui_build scene_state view_setup hover_pick submit_scene submit_helpers imgui_render bgfx_frame".split()
    with output.with_suffix(".csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for i in range(count):
            row = dict.fromkeys(fields, 0)
            row.update(measured=1, frame=i+1, gpu_frame=(i//2)*2, wall_ms=10 if i % 2 else 30,
                       total_ms=12, bgfx_frame=8, gpu_ms=5, width=1280, height=720)
            writer.writerow(row)
    return output


def test_percentile():
    assert percentile([3, 1, 2], .5) == 2
    assert percentile([2, 4], .25) == 2.5
    assert percentile([], .5) is None


def test_frame_weighting_cpu_wait_and_unique_gpu_samples():
    with tempfile.TemporaryDirectory(prefix="woby-render-analysis-") as directory:
        row = summarize_run(fixture(Path(directory).resolve(), 40))["scenarios"][0]
        assert row["fps"] == 50
        assert row["cpu_p50_ms"] == 4
        assert row["gpu_p50_ms"] == 5
        assert row["gpu_samples"] == 12


def test_incomplete_measurement_rejected():
    with tempfile.TemporaryDirectory(prefix="woby-render-analysis-") as directory:
        try:
            summarize_run(fixture(Path(directory).resolve(), 29))
        except ValueError:
            return
        raise AssertionError("Incomplete measurement accepted")


def test_independent_runs_are_not_weighted_by_frame_count():
    with tempfile.TemporaryDirectory(prefix="woby-render-analysis-") as directory:
        row = summarize_run(fixture(Path(directory).resolve(), 40))["scenarios"][0]
        metadata = dict(models=[dict(vertices=1)], renderer="test", vendor_id=1, device_id=2)
        a = dict(model="fixture", metadata=metadata, scenarios=[row])
        b = dict(model="fixture", metadata=metadata, scenarios=[dict(row, fps=100, frames=400)])
        result = aggregate([a, b])["fixture"]["scenarios"]["test"]
        assert result["fps"] == 75
        assert result["fps_range"] == [50, 100]
        assert result["frames"] == 440
        b["metadata"] = dict(metadata, device_id=3)
        try:
            aggregate([a, b])
        except ValueError:
            return
        raise AssertionError("Mismatched device accepted")


if __name__ == "__main__":
    cases = [unittest.FunctionTestCase(value) for name, value in list(globals().items()) if name.startswith("test_")]
    result = unittest.TextTestRunner().run(unittest.TestSuite(cases))
    raise SystemExit(0 if result.wasSuccessful() else 1)
