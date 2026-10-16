# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12", "Pillow>=11", "duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Check phase nesting, independent endpoints, provenance, and fixture cleanup."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from e2e_run import load_trace
from e2e_batch import schedule
from e2e_analyze import percentile, native_first_frame_bounds
from e2e_hardware import sample_thread_ids
from e2e_nsys import pair_packets, event_association
from e2e_evidence import vtune_rows
from annotation_compare import sample_metrics, viewer_contention


class TraceReadTests(unittest.TestCase):
    def test_completed_viewer_identity_removes_only_owned_launch_races(self):
        processes = {11: {"pid": 11, "name": "woby.exe"}, 12: {"pid": 12, "name": "woby.exe"},
                     13: {"pid": 13, "name": "woby.exe"}, 14: {"pid": 14, "name": "cl.exe"}}
        filtered = viewer_contention(processes, {"launcher_pid": 11, "pid": 12})
        self.assertEqual(filtered, {pid: processes[pid] for pid in (13, 14)})
        self.assertEqual(len(processes), 4)
        self.assertEqual(viewer_contention(processes, {}), processes)
        self.assertEqual(viewer_contention({11: processes[11]}, {"pid": 11, "launcher_pid": 11}), {})

    def test_import_and_normal_comparisons_keep_measurement_boundaries_and_identity(self):
        imported = sample_metrics("import", {"cpu_load_ms": 15, "peak_commit_bytes": 200,
            "fingerprint": 123, "color_fingerprint": 456})
        self.assertEqual(imported, {"wall_ms": 15, "peak_commit_bytes": 200,
            "output": {"fingerprint": 123, "color_fingerprint": 456}})
        replay = {"import_ms": 90, "vertices": 4, "triangles": 2,
            "records": [{"wall_ms": 3, "cpu_ms": 7, "normal_digest": 789}]}
        measured = sample_metrics("normals", replay)
        self.assertEqual(measured["wall_ms"], 3)
        self.assertEqual(measured["cpu_ms"], 7)
        self.assertEqual(measured["output"], {"normal_digest": 789, "vertices": 4, "triangles": 2})
        self.assertNotIn("output", replay["records"][0])
        for mode in ("annotation", "post-load"):
            self.assertEqual(sample_metrics(mode, {"records": [{"wall_ms": 5, "output": {"tree": 42}}]}),
                             {"wall_ms": 5, "output": {"tree": 42}})

    def test_vtune_export_discards_progress_messages_and_keeps_function_rows(self):
        with tempfile.TemporaryDirectory(prefix="woby vtune report ") as temporary:
            path=Path(temporary).resolve()/'functions.csv'
            path.write_text('vtune: processing Function\tCPU Time\tModule\nparse\t3.2\twoby.exe\n'
                            'vtune: finished\n',encoding='utf-8')
            self.assertEqual(vtune_rows(path,'Function'),[{'Function':'parse','CPU Time':'3.2','Module':'woby.exe'}])

    def test_kernel_events_use_payload_ownership_and_queue_pairing_keeps_orphans(self):
        self.assertEqual(event_association(42, 4 << 24, {'ProcessId': '42'}), 'payload')
        self.assertEqual(event_association(42, 42 << 24, {'hProcessId': '0x2A'}), 'emitter+payload')
        self.assertIsNone(event_association(42, 4 << 24, {'ProcessId': '43'}))
        starts = [{'context': 1, 'sequence': 2, 'start': 10, 'globalTid': 1},
                  {'context': 1, 'sequence': 3, 'start': 12, 'globalTid': 1}]
        stops = [{'context': 1, 'sequence': 2, 'start': 20, 'globalTid': 4},
                 {'context': 9, 'sequence': 2, 'start': 30, 'globalTid': 4}]
        result = pair_packets(starts, stops, ('context', 'sequence'))
        self.assertEqual(result['pairs'][0]['duration_ms'], .00001)
        self.assertEqual(len(result['unmatched_starts']), 1)
        self.assertEqual(len(result['unmatched_stops']), 1)

    def test_sample_thread_filter_uses_the_sample_table_id_domain(self):
        mapping = {42: [100042], 43: [100043, 200043]}
        self.assertEqual(sample_thread_ids(42, {42, 100042}, mapping), [42])
        self.assertEqual(sample_thread_ids(43, {100043}, mapping), [100043])
        self.assertEqual(sample_thread_ids(43, {99}, mapping), [])

    def test_native_frame_timestamp_is_bracketed_and_separate_from_gpu_completion(self):
        record = {"load_request_ms": 130, "rpc": [{"method": "performance.begin", "start_ms": 123, "end_ms": 127}],
                  "load_frames": {"environments": [{"sceneEditRevision": 0}, {"sceneEditRevision": 2}],
                                  "events": [{"environment": 0, "completedMilliseconds": 10},
                                             {"environment": 1, "completedMilliseconds": 20}]}}
        self.assertEqual(native_first_frame_bounds(record), [13, 17])
        self.assertEqual(percentile([None, 0, 10, 20], .95), 19)

    def test_corrupt_completed_trace_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="woby e2e corrupt trace ") as temporary:
            trace = Path(temporary).resolve() / "trace.jsonl"
            trace.write_text('{"kind":"mark"}\n{"kind":\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Malformed complete"):
                load_trace(trace)

    def test_schedule_rotates_each_configuration_once_per_round(self):
        configurations = ["production-hidden", "production-visible", "traced-hidden"]
        rows = schedule(configurations, 3)
        self.assertEqual(len(rows), 9)
        for round_number in range(1, 4):
            selected = [c for r, c in rows if r == round_number]
            self.assertCountEqual(selected, configurations)
            self.assertEqual(selected[0], configurations[round_number - 1])

    def test_incomplete_live_record_does_not_hide_completed_records(self):
        with tempfile.TemporaryDirectory(prefix="woby e2e trace ") as temporary:
            root = Path(temporary).resolve()
            trace = root / "trace.jsonl"
            trace.write_text('{"kind":"mark","time_ms":1}\n{"kind":', encoding="utf-8")
            self.assertEqual(load_trace(trace), [{"kind": "mark", "time_ms": 1}])


class ViewerTraceTests(unittest.TestCase):
    executable = None

    def test_complete_point_group_reuses_validated_bounds(self):
        if self.executable is None:
            self.skipTest("No viewer supplied")
        with tempfile.TemporaryDirectory(prefix="woby point bounds fixture ") as temporary:
            root = Path(temporary).resolve()
            model = root / "points.obj"
            count = 65536
            model.write_text(''.join(f"v {i % 256} {i // 256} 0\n" for i in range(count))
                             + ''.join(f"p {i + 1}\n" for i in range(count)), encoding="utf-8")
            output = root / "measurements"
            result = subprocess.run([sys.executable, str(Path(__file__).with_name("e2e_run.py")),
                                     str(self.executable), str(model), "--hidden", "--output", str(output),
                                     "--settle-seconds", "0", "--render-seconds", "0"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout)
            record = json.loads((output / "result.json").read_text(encoding="utf-8"))
            self.assertNotIn("error", record)
            bounds = [p for p in record["trace"] if p.get("name") == "point_range_bounds"]
            self.assertEqual(len(bounds), 1)
            self.assertEqual(bounds[0]["data"], {"bounds_reused": 1, "scanned_vertices": 0})

    def test_complete_pipeline_has_valid_nesting_and_distinct_endpoints(self):
        if self.executable is None:
            self.skipTest("No viewer supplied")
        with tempfile.TemporaryDirectory(prefix="woby e2e fixture ") as temporary:
            root = Path(temporary).resolve()
            model = root / "seam.obj"
            model.write_text("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                             "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt .5 .5\n"
                             "g first\nf 3/3 1/1 2/2\ng second\nf 1/5 3/3 4/4\n", encoding="utf-8")
            output = root / "measurements"
            result = subprocess.run([sys.executable, str(Path(__file__).with_name("e2e_run.py")),
                                     str(self.executable), str(model), "--hidden", "--output", str(output),
                                     "--settle-seconds", "0", "--render-seconds", "0", "--post-load-navigation"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout)
            record = json.loads((output / "result.json").read_text(encoding="utf-8"))
            self.assertNotIn("error", record)
            self.assertTrue(record["image"]["non_background_fraction"] > .01)
            trace = record["trace"]
            self.assertEqual(sum(t["kind"] == "metadata" for t in trace), 1)
            phases = {t["id"]: t for t in trace if t["kind"] == "phase"}
            self.assertTrue(phases)
            for phase in phases.values():
                self.assertTrue(phase["nesting_valid"], phase)
                self.assertLessEqual(phase["start_ms"], phase["end_ms"])
                if phase["parent_id"]:
                    parent = phases[phase["parent_id"]]
                    self.assertEqual(parent["thread_id"], phase["thread_id"])
                    self.assertLessEqual(parent["start_ms"], phase["start_ms"])
                    self.assertGreaterEqual(parent["end_ms"], phase["end_ms"])
            marks = {t["name"]: t for t in trace if t["kind"] == "mark"}
            names = ("model_add_requested", "first_scene_frame_submitted", "first_scene_frame_gpu_complete", "all_tools_ready", "model_add_rpc_complete")
            self.assertTrue(set(names) <= marks.keys(), marks.keys())
            self.assertEqual(sorted(marks[n]["time_ms"] for n in names), [marks[n]["time_ms"] for n in names])
            toast_names = ("all_tools_ready", "ready_toast_drawn", "ready_toast_frame_submitted", "ready_toast_frame_gpu_complete")
            self.assertTrue(set(toast_names) <= marks.keys(), marks.keys())
            self.assertEqual(sorted(marks[n]["time_ms"] for n in toast_names), [marks[n]["time_ms"] for n in toast_names])
            self.assertEqual(marks["ready_toast_frame_submitted"]["data"]["frame_token"],
                             marks["ready_toast_frame_gpu_complete"]["data"]["frame_token"])
            self.assertTrue(record["post_load_navigation"])
            self.assertLessEqual(marks["first_scene_frame_gpu_complete"]["time_ms"], marks["post_navigation_begin"]["time_ms"])
            self.assertLess(marks["post_navigation_begin"]["time_ms"], marks["all_tools_ready"]["time_ms"])
            self.assertGreaterEqual(marks["post_navigation_end"]["time_ms"], marks["all_tools_ready"]["time_ms"])
            self.assertGreater(len({event["camera"][3] for event in record["load_frames"]["events"]}), 1)
            self.assertGreaterEqual(len({p["thread_id"] for p in phases.values()}), 2)
            validation = [p for p in phases.values() if p["name"] == "gpu_index_validation"]
            self.assertEqual([p["data"]["validation_reused"] for p in validation], [0, 1])
            self.assertEqual(len(record["executable_sha256"]), 64)
            # Every fixture, output and working directory is owned by this test.
            self.assertTrue(all(p.is_relative_to(root) for p in (model, output)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--viewer", type=Path)
    args, remaining = parser.parse_known_args()
    ViewerTraceTests.executable = args.viewer.resolve(strict=True) if args.viewer else None
    unittest.main(argv=[sys.argv[0], *remaining])
