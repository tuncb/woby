# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil>=6", "Pillow>=11"]
# ///
"""Check timing classification and rejection of invalid benchmark evidence."""
import copy
import importlib.util
import math
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location("point_benchmark", Path(__file__).with_name("point_render_benchmark.py"))
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)

DRAWABLE = dict(width=1280, height=720)
VIEWPORT = dict(x=0, y=33, width=1280, height=687)


def capture():
    return dict(droppedFrames=0, environments=[dict(drawable=DRAWABLE.copy(), viewport=VIEWPORT.copy(),
        pacing=dict(msaaSamples=4))], events=[dict(completedMilliseconds=10, gpuFrameMilliseconds=2,
        frameMilliseconds=50, cpuSubmitMilliseconds=49,
        stagesMilliseconds=dict(submit_scene=.4, imgui_build=.2, graphics_frame=49),
        points=dict(active=True, sourceCount=12, refinedCount=12, submittedCount=0,
                    gpuRasterCount=0, gpuRasterMilliseconds=0, navigation=False))])


def rejects_capture(record, cached=False):
    try:
        benchmark.validate_capture(record, 12, DRAWABLE, VIEWPORT, cached=cached)
    except AssertionError:
        return
    raise AssertionError("Invalid benchmark capture was accepted")


def test_delayed_gpu_work_is_classified_independently_of_cpu_submission():
    record = capture()
    record["events"][0]["points"]["submittedCount"] = 12
    delayed = copy.deepcopy(record["events"][0])
    delayed["completedMilliseconds"] = 20
    delayed["gpuFrameMilliseconds"] = 2.8
    delayed["points"].update(submittedCount=0, gpuRasterCount=12, gpuRasterMilliseconds=2.3)
    record["events"].append(delayed)
    summary = benchmark.summarize(record)
    assert summary["raster_frames"] == 1
    assert summary["raster_frame_gpu_ms"]["median"] == 2.8
    assert summary["point_raster_ms"]["median"] == 2.3
    assert summary["submitted_frames"] == 1
    assert math.isclose(summary["scene_submit_work_ms"]["median"], .4)
    assert math.isclose(summary["main_thread_work_ms"]["median"], .6)
    assert summary["cpu_submit_ms"]["median"] == 49


def test_cached_frames_need_complete_source_data_and_no_new_submission():
    record = capture()
    benchmark.validate_capture(record, 12, DRAWABLE, VIEWPORT, cached=True)
    for changed in (dict(active=False), dict(sourceCount=11), dict(refinedCount=11), dict(submittedCount=1)):
        invalid = copy.deepcopy(record)
        invalid["events"][0]["points"].update(changed)
        rejects_capture(invalid, cached=True)
    summary = benchmark.summarize(record)
    assert summary["raster_frames"] == 0
    assert summary["scene_submit_work_ms"] == {}


def test_dropped_samples_or_changed_render_dimensions_reject_comparisons():
    for fault in ("dropped", "empty", "drawable", "viewport", "samples"):
        invalid = capture()
        if fault == "dropped": invalid["droppedFrames"] = 1
        elif fault == "empty": invalid["events"] = []
        elif fault == "samples": invalid["environments"][0]["pacing"]["msaaSamples"] = 1
        else:
            environment = copy.deepcopy(invalid["environments"][0])
            environment[fault]["width"] = 640
            invalid["environments"].append(environment)
        rejects_capture(invalid)


if __name__ == "__main__":
    tests = (test_delayed_gpu_work_is_classified_independently_of_cpu_submission,
             test_cached_frames_need_complete_source_data_and_no_new_submission,
             test_dropped_samples_or_changed_render_dimensions_reject_comparisons)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(map(unittest.FunctionTestCase, tests)))
    raise SystemExit(0 if result.wasSuccessful() else 1)
