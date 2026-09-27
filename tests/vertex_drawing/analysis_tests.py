import copy
import unittest
from summarize import summarize


def row(mode, iteration, gpu_ms):
    result = {"model": "fixture.obj", "mode": mode, "round": iteration, "ok": True,
              "point_entries": 12, "point_hash": 123, "vertices": 9, "indices": 18, "groups": 4,
              "width": 320, "height": 240, "msaa": 1, "vsync": False,
              "enable_cpu_ms": 1, "enable_ready_ms": 2, "first_draw_ready_ms": 3, "marker_payload_bytes": 48,
              "base_ready_ms": 1, "scenarios": []}
    scenario = {"name": "points"}
    for metric in ("gpu_markers", "gpu_frame", "wall_frame", "cpu_submit"):
        scenario[metric] = {"count": 100, "median_ms": gpu_ms}
    result["scenarios"].append(scenario)
    return result


class AnalysisTests(unittest.TestCase):
    def test_independent_runs_receive_equal_weight(self):
        rows = [row(mode, i + 1, value) for mode, values in (("current", [10, 20, 30]), ("shader", [4, 5, 6]))
                for i, value in enumerate(values)]
        rows[0]["scenarios"][0]["gpu_markers"]["count"] = 100000
        gpu = summarize(rows)[0]["scenarios"][0]["gpu_markers"]
        self.assertEqual(gpu["current"]["median"], 20)
        self.assertEqual(gpu["shader"]["median"], 5)
        self.assertEqual(gpu["reduction_percent"], 75)

    def test_failed_baseline_does_not_become_speedup(self):
        failed = {"model": "fixture.obj", "mode": "current", "round": 1, "ok": False, "error": "allocation failed"}
        result = summarize([failed, row("shader", 1, 1)])[0]
        self.assertEqual(result["paired_rounds"], [])
        self.assertNotIn("scenarios", result)
        self.assertEqual(len(result["failures"]), 1)

    def test_unpaired_round_is_excluded(self):
        result = summarize([row("current", 1, 10), row("shader", 1, 5), row("shader", 2, .001)])[0]
        self.assertEqual(result["paired_rounds"], [1])
        self.assertEqual(result["scenarios"][0]["gpu_markers"]["shader"]["median"], 5)

    def test_different_membership_rejected(self):
        after = row("shader", 1, 5)
        after["point_hash"] = 456
        with self.assertRaisesRegex(ValueError, "point_hash"):
            summarize([row("current", 1, 10), after])

    def test_missing_gpu_samples_rejected(self):
        after = copy.deepcopy(row("shader", 1, 5))
        after["scenarios"][0]["gpu_markers"] = {"count": 0}
        with self.assertRaisesRegex(ValueError, "missing timing"):
            summarize([row("current", 1, 10), after])

    def test_flat_comparison_uses_its_own_measurements(self):
        result = summarize([row("current", 1, 10), row("shader", 1, 9), row("flat", 1, 4)], "flat")[0]
        self.assertEqual(result["scenarios"][0]["gpu_markers"]["reduction_percent"], 60)


if __name__ == "__main__":
    unittest.main()
