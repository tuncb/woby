"""Unit tests of measurement math and resource limits, without a viewer."""

import unittest
import argparse
import errno
import json
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
from unittest.mock import patch

from run import add_memory_limit_arguments, fixture_copy, memory_stop_reason, summarize_frames, wait_detector
from prepare_derivatives import finite_vertices
from summarize import aggregate, analysis_summary, collect, resource_summary


def sample(frame, timestamp, gpu=2):
    return dict(frameIndex=frame, midpoint=timestamp, frameMilliseconds=10,
                gpuFrameMilliseconds=gpu, stagesMilliseconds={"events": 1})


def test_fps_uses_frame_counter_and_elapsed_time():
    result = summarize_frames([sample(100, 1), sample(105, 1.5), sample(110, 2)])
    assert result["fps"] == 10
    assert result["sampled_frame_median_ms"] == 10

def test_repeated_frame_ids_are_not_extra_frames():
    result = summarize_frames([sample(1, 0), sample(1, .1), sample(11, 1)])
    assert result["samples"] == 2
    assert result["fps"] == 10

def test_gpu_unavailable_remains_unknown():
    result = summarize_frames([sample(1, 0, None), sample(2, 1, None)])
    assert result["sampled_gpu_median_ms"] is None

def test_insufficient_frames_have_no_fps():
    assert summarize_frames([])["fps"] is None
    assert summarize_frames([sample(1, 0)])["fps"] is None

def test_memory_limits_distinguish_process_and_system():
    assert memory_stop_reason(11, 20, 10, 5) == "process_private_limit"
    assert memory_stop_reason(9, 4, 10, 5) == "system_available_floor"
    assert memory_stop_reason(10, 5, 10, 5) is None


def test_memory_monitor_has_no_default_cutoff():
    parser = argparse.ArgumentParser()
    add_memory_limit_arguments(parser)
    defaults = parser.parse_args([])
    assert defaults.max_private_gib == defaults.min_available_gib == 0
    assert memory_stop_reason(100 * 1024**3, 0, 0, 0) is None
    assert memory_stop_reason(100, 4, 0, 5) == "system_available_floor"
    assert memory_stop_reason(11, 0, 10, 0) == "process_private_limit"
    explicit = parser.parse_args(["--max-private-gib", "38", "--min-available-gib", "6"])
    assert explicit.max_private_gib == 38 and explicit.min_available_gib == 6
    for invalid in ("-1", "nan", "inf"):
        with unittest.TestCase().assertRaises(SystemExit):
            parser.parse_args(["--max-private-gib", invalid])


def test_finite_derivative_preserves_source_and_records_removed_rows():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        source, target = root / "source.obj", root / "finite.obj"
        original = b"# cloud\nv 1 2 3\nv nan 0 1\nv 4 inf 2\nv 5 6 7\n"
        source.write_bytes(original)
        result = finite_vertices(source, target)
        assert result["source_vertices"] == 4
        assert result["kept_vertices"] == 2
        assert result["removed_nonfinite_vertices"] == 2
        assert target.read_bytes() == b"# cloud\nv 1 2 3\nv 5 6 7\n"
        assert source.read_bytes() == original


def test_finite_derivative_rejects_indexed_geometry_and_cleans_partial_output():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        source, target = root / "source.obj", root / "finite.obj"
        source.write_text("v 1 2 3\np 1\n", encoding="utf-8")
        with unittest.TestCase().assertRaises(ValueError):
            finite_vertices(source, target)
        assert not target.exists()


def test_derivative_does_not_overwrite_existing_output():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        source, target = root / "source.obj", root / "finite.obj"
        source.write_text("v 1 2 3\n", encoding="utf-8")
        target.write_bytes(b"keep")
        with unittest.TestCase().assertRaises(FileExistsError):
            finite_vertices(source, target)
        assert target.read_bytes() == b"keep"


def test_fixture_copy_handles_different_drives_without_changing_source():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        source, target = root / "source.obj", root / "copy.obj"
        source.write_bytes(b"v 1 2 3\n")
        with patch("run.os.link", side_effect=OSError(errno.EXDEV, "Different drives")):
            fixture_copy(source, target)
        assert target.read_bytes() == source.read_bytes()


def test_fixture_copy_does_not_mask_other_filesystem_errors():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        with patch("run.os.link", side_effect=PermissionError("denied")):
            with unittest.TestCase().assertRaises(PermissionError):
                fixture_copy(root / "source.obj", root / "copy.obj")
        assert not (root / "copy.obj").exists()


def test_resource_summary_keeps_retained_and_peak_memory_separate():
    values = [dict(phase="empty", private=40, rss=20, cpu_seconds=1),
              dict(phase="loaded", private=200, rss=150, cpu_seconds=2),
              dict(phase="empty", private=30, rss=10, cpu_seconds=3)]
    result = resource_summary(values)
    assert result["empty"]["peak_private_bytes"] == 40
    assert result["empty"]["last_private_bytes"] == 30
    assert result["loaded"]["last_rss_bytes"] == 150
    assert result["empty"]["sampled_cpu_seconds"] == 0


def test_summary_preserves_incomplete_detector_status():
    result = analysis_summary({"aToB": {"detectors": {"self_intersections": {
        "count": None, "knownCount": 10, "status": "truncated", "detectionTruncated": True,
        "truncationReason": "candidate_limit", "findings": [1, 2, 3]}}}})
    detector = result["aToB"]["detectors"]["self_intersections"]
    assert detector["count"] is None and detector["knownCount"] == 10
    assert detector["detectionTruncated"] and detector["truncationReason"] == "candidate_limit"
    assert "findings" not in detector


def test_import_aggregate_includes_successful_import_before_guarded_analysis():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        cases = [dict(model=str(root / "mesh.obj"), status="process_private_limit", load_outcome={"addedCount": 1},
                      operations=[dict(name="model_add", status="ok", seconds=2)])]
        assert aggregate(cases)["mesh.obj"]["median_seconds"] == 2


def test_harness_cleanup_is_not_reported_as_an_application_crash():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        case_dir = root / "campaign" / "case"
        case_dir.mkdir(parents=True)
        case = dict(suite="comparison", status="process_exit", forced_cleanup=True,
                    exit_code=0xFFFFFFFF, model=str(root / "model.obj"), frames=[])
        (case_dir / "case.json").write_text(json.dumps(case), encoding="utf-8")
        assert collect(root)[0]["status"] == "failed"
        case.update(forced_cleanup=False, exit_code=0xC0000374)
        (case_dir / "case.json").write_text(json.dumps(case), encoding="utf-8")
        assert collect(root)[0]["status"] == "process_exit"


def test_exception_exit_is_preserved_when_cleanup_races_process_exit():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        case_dir = root / "campaign" / "case"
        case_dir.mkdir(parents=True)
        case = dict(suite="comparison", status="failed", forced_cleanup=True,
                    exit_code=0xC0000409, model=str(root / "model.obj"), frames=[])
        (case_dir / "case.json").write_text(json.dumps(case), encoding="utf-8")
        result = collect(root)[0]
        assert result["status"] == "process_exit"
        assert "ordering is uncertain" in result["classification_note"]
        case["status"] = "process_private_limit"
        (case_dir / "case.json").write_text(json.dumps(case), encoding="utf-8")
        assert collect(root)[0]["status"] == "process_private_limit"


def test_manual_detector_poll_waits_past_queued_and_running_responses():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        state = dict(directory=root, args=SimpleNamespace(operation_timeout=10), resources=[],
                     operations=[], started=time.perf_counter())
        responses = [{"aToB": {"detectors": {"self_intersections": {"status": status}}}}
                     for status in ("queued", "running", "complete")]
        with patch("run.operation", side_effect=responses) as rpc, patch("run.time.sleep"):
            result = wait_detector(state, "analysis", "self_intersections", "ready")
        assert rpc.call_count == 3
        assert result["aToB"]["detectors"]["self_intersections"]["status"] == "complete"
        assert state["operations"][-1]["detector_status"] == "complete"


def test_manual_detector_partial_result_is_terminal_but_not_complete():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        state = dict(directory=root, args=SimpleNamespace(operation_timeout=10), resources=[],
                     operations=[], started=time.perf_counter())
        partial = {"aToB": {"detectors": {"self_intersections": {
            "status": "partial", "count": None, "knownCount": 10000,
            "detectionTruncated": True, "truncationReason": "pair_limit"}}}}
        with patch("run.operation", return_value=partial) as rpc:
            result = wait_detector(state, "analysis", "self_intersections", "ready")
        assert rpc.call_count == 1
        assert result["aToB"]["detectors"]["self_intersections"]["count"] is None
        assert state["operations"][-1]["detector_status"] == "partial"


if __name__ == "__main__":
    suite = unittest.TestSuite(unittest.FunctionTestCase(value) for name, value in list(globals().items()) if name.startswith("test_"))
    result = unittest.TextTestRunner().run(suite)
    raise SystemExit(not result.wasSuccessful())
