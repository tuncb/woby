"""Check measurement boundaries and geometry on a fixture with UV seams."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

EXE = Path(sys.argv.pop(1)).resolve(strict=True)
DIAGNOSTIC = Path(sys.argv.pop(1)).resolve(strict=True) if len(sys.argv) > 1 else None
UV_CHECK = Path(sys.argv.pop(1)).resolve(strict=True) if len(sys.argv) > 1 else None
UV_PTR = Path(sys.argv.pop(1)).resolve(strict=True) if len(sys.argv) > 1 else None
NORMALS = Path(sys.argv.pop(1)).resolve(strict=True) if len(sys.argv) > 1 else None
PREPARATION = Path(sys.argv.pop(1)).resolve(strict=True) if len(sys.argv) > 1 else None


def test_triangle_and_seam_geometry():
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_obj_profile_") as temporary:
        path = Path(temporary).resolve() / "seam.obj"
        path.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                        "vt 0 0\nvt 1 0\nvt 0 1\nvt .5 .5\n"
                        "f 1/1 2/2 3/3\nf 1/4 3/3 4/2\n", encoding="ascii")
        records = []
        for mode in ("parser", "load", "prepare"):
            run = subprocess.run([str(EXE), mode, str(path), "1"], capture_output=True, text=True, check=True)
            records.append(json.loads(run.stdout))
        parsed, loaded, prepared = records
        checks.assertEqual(parsed["geometry"]["positions"], 4)
        checks.assertEqual(parsed["geometry"]["faces"], 2)
        checks.assertEqual(loaded["geometry"]["vertices"], 5)
        checks.assertEqual(loaded["geometry"]["triangles"], 2)
        checks.assertEqual(loaded["geometry"], prepared["geometry"])
        if NORMALS:
            normals = json.loads(subprocess.check_output([str(NORMALS), str(path), "2"], text=True))
            checks.assertEqual(normals["vertices"], 5)
            checks.assertEqual(normals["triangles"], 2)
            checks.assertGreater(normals["import_ms"], 0)
            checks.assertEqual(len(normals["records"]), 2)
            checks.assertEqual(normals["records"][0]["normal_digest"], normals["records"][1]["normal_digest"])
            checks.assertTrue(all(row["wall_ms"] > 0 and row["cpu_ms"] >= 0 for row in normals["records"]))
            for rounds in ("0", "21"):
                rejected = subprocess.run([str(NORMALS), str(path), rounds], capture_output=True, text=True)
                checks.assertNotEqual(rejected.returncode, 0)
                checks.assertIn("Rounds must be between", rejected.stderr)
        if UV_PTR:
            direct = json.loads(subprocess.check_output([str(UV_PTR), "load", str(path), "1"], text=True))
            checks.assertEqual(loaded["geometry"], direct["geometry"])
        sampled = subprocess.run([str(EXE), "prepare", str(path), "1", "sample"],
                                 capture_output=True, text=True, check=True)
        sampled_record = json.loads(sampled.stdout)
        checks.assertEqual(loaded["geometry"], sampled_record["geometry"])
        checks.assertEqual(sampled_record["suspend_failures"], 0)
        for mode in ("read-native", "read-cached"):
            read = subprocess.run([str(EXE), mode, str(path), "2"], capture_output=True, text=True, check=True)
            checks.assertEqual(json.loads(read.stdout)["stats"]["input_bytes"], path.stat().st_size)
        events = {row["name"]: row for row in prepared["events"]}
        for name in ("cpu_import", "corner_mapping", "normal_face_accumulation", "original_bounds",
                     "group_state", "point_hierarchy", "geometry_validation"):
            checks.assertIn(name, events)
            checks.assertGreaterEqual(events[name]["wall_ms"], 0)
            checks.assertGreaterEqual(events[name]["cpu_ms"], 0)
            checks.assertGreater(events[name]["working_set_end"], 0)
        root = events["cpu_import"]
        for row in prepared["events"]:
            if row["name"] in ("corner_mapping", "render_bounds", "capacity_preflight"):
                checks.assertLessEqual(root["start_ms"], row["start_ms"])
                checks.assertGreaterEqual(root["end_ms"], row["end_ms"])
        if DIAGNOSTIC:
            diagnostic = subprocess.run([str(DIAGNOSTIC), "load", str(path), "1"],
                                        capture_output=True, text=True, check=True)
            record = json.loads(diagnostic.stdout)
            checks.assertEqual(loaded["geometry"], record["geometry"])
            mapping = next(row for row in record["events"] if row["name"] == "corner_mapping")
            if mapping["data"].get("production_staged"):
                counts = next(row["data"] for row in record["events"] if row["name"] == "mapping_staged_diagnostic")
            else:
                counts = mapping["data"]["diagnostic_counts"]
            checks.assertEqual(counts["first_positions"], 4)
            checks.assertEqual(counts["primary_hits"], 1)
            checks.assertEqual(counts["secondary_lookups"], 1)
            checks.assertEqual(counts["secondary_hits"], 0)
        replay = json.loads(subprocess.check_output([str(EXE), "replay", str(path), "1"], text=True))
        checks.assertEqual(replay["geometry"], loaded["geometry"])
        cases = replay["replay"]["bounds"]
        checks.assertEqual(len(cases), 48)
        checks.assertTrue(all(case["bounds"] == [[0, 0, 0], [1, 1, 1]] for case in cases))
        checks.assertEqual(len(replay["replay"]["access_sweep"]), 54)
        if loaded["cpu"]["sse42"]:
            simd = json.loads(subprocess.check_output([str(EXE), "simd-replay", str(path), "1"], text=True))
            checks.assertEqual(simd["geometry"], loaded["geometry"])
            cases = simd["simd_replay"]["bounds"]
            checks.assertEqual(len(cases), 18)
            checks.assertTrue(all(case["bounds"] == [[0, 0, 0], [1, 1, 1]] for case in cases))
            offset_path = path.parent / "offset.obj"
            offset_path.write_text("v 1000000000.125 -4.5 8\nv 1000000000.5 -20.5 9\n"
                                   "v 1000000000.25 4.25 -1\nf 1 2 3\n", encoding="ascii")
            simd = json.loads(subprocess.check_output([str(EXE), "simd-replay", str(offset_path), "1"], text=True))
            checks.assertTrue(all(case["bounds"] == [[1000000000.125, -20.5, -1], [1000000000.5, 4.25, 9]]
                                  for case in simd["simd_replay"]["bounds"]))
        for percent in ("2", "100"):
            reserved = json.loads(subprocess.check_output([str(EXE), "load", str(path), "1"], text=True,
                env=dict(os.environ, WOBY_PROFILE_RESERVE_PERCENT=percent)))
            for key in ("geometry_fingerprint", "normal_fingerprint", "vertices", "triangles"):
                checks.assertEqual(reserved["geometry"][key], loaded["geometry"][key])
        rejected = subprocess.run([str(EXE), "load", str(path), "1"], capture_output=True, text=True,
            env=dict(os.environ, WOBY_PROFILE_RESERVE_PERCENT="101"))
        checks.assertNotEqual(rejected.returncode, 0)
        checks.assertIn("Reserve percentage", rejected.stderr)
        control = json.loads(subprocess.check_output([str(EXE), "flop-control", str(path)], text=True))
        checks.assertEqual(control["stats"]["expected_flops"], 4 * 128 * 1024 * 1024)
        checks.assertGreater(control["stats"]["result"], 1)
        checks.assertLess(control["stats"]["result"], 2)
        if UV_CHECK:
            for uv_text in ("vt .1 .2\nvt .3 .4\nvt .5 .6\nvt 1e39 1e39\n", "vt 1e39 .2\nvt .3 .4\nvt .5 .6\n"):
                path.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\n" + uv_text + "f 1/1 2/2 3/3\n", encoding="ascii")
                expected = json.loads(subprocess.check_output([str(EXE), "load", str(path), "1"], text=True))
                cached = json.loads(subprocess.check_output([str(UV_CHECK), "load", str(path), "1"], text=True))
                checks.assertEqual(expected["geometry"], cached["geometry"])
                if UV_PTR:
                    direct = json.loads(subprocess.check_output([str(UV_PTR), "load", str(path), "1"], text=True))
                    checks.assertEqual(expected["geometry"], direct["geometry"])
                checks.assertIn("uv_validation_cache", [row["name"] for row in cached["events"]])


def test_concurrent_profile_events():
    if not PREPARATION:
        return
    checks = unittest.TestCase()
    report = json.loads(subprocess.check_output([str(PREPARATION), "--test-concurrent-events"], text=True))
    checks.assertEqual(len(report["events"]), 4 * 16 * 2)
    for worker in range(4):
        events = [event for event in report["events"] if event["data"]["worker"] == worker]
        checks.assertEqual(sum(event["name"] == "inner" for event in events), 16)
        checks.assertEqual(sum(event["name"] == f"outer-{worker}" for event in events), 16)
        for event in events:
            checks.assertEqual(event["parent"], f"outer-{worker}" if event["name"] == "inner" else "idle")


def test_point_preparation_identity():
    if not PREPARATION:
        return
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby point preparation ") as temporary:
        path = Path(temporary).resolve() / "rgb.obj"
        vertices = "".join(f"v {i % 97} {i % 31} {i % 13} 1 0 0\n" for i in range(65538))
        faces = "".join(f"f {i} {i + 1} {i + 2}\n" for i in range(1, 65539, 3))
        path.write_text(vertices + faces, encoding="ascii")
        outputs = {}
        for mode in ("membership", "points", "post-load", "annotation"):
            result = json.loads(subprocess.check_output([str(PREPARATION), str(path), "2", mode], text=True))
            checks.assertEqual(result["mode"], mode)
            checks.assertEqual(result["records"][0]["output"], result["records"][1]["output"])
            checks.assertTrue(all(row["wall_ms"] > 0 for row in result["records"]))
            outputs[mode] = result["records"][0]["output"]
        checks.assertEqual(outputs["points"], outputs["post-load"]["points"])
        checks.assertEqual(outputs["membership"]["membership_digest"], outputs["points"]["membership_digest"])
        checks.assertEqual(outputs["points"]["point_count"], 65538)
        checks.assertGreater(outputs["points"]["node_count"], 0)
        checks.assertEqual(outputs["annotation"], {k: v for k, v in outputs["post-load"].items() if k != "points"})
        path.write_text(vertices.replace("1 0 0\n", "0 1 0\n") + faces, encoding="ascii")
        changed = json.loads(subprocess.check_output([str(PREPARATION), str(path), "1", "points"], text=True))["records"][0]["output"]
        checks.assertNotEqual(changed["colors_digest"], outputs["points"]["colors_digest"])
        checks.assertEqual({k: v for k, v in changed.items() if k != "colors_digest"},
                           {k: v for k, v in outputs["points"].items() if k != "colors_digest"})
        for rounds, mode in (("0", "points"), ("21", "points"), ("1", "invalid")):
            checks.assertNotEqual(subprocess.run([str(PREPARATION), str(path), rounds, mode], capture_output=True).returncode, 0)


if __name__ == "__main__":
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([
        unittest.FunctionTestCase(test_triangle_and_seam_geometry), unittest.FunctionTestCase(test_concurrent_profile_events),
        unittest.FunctionTestCase(test_point_preparation_identity)]))
    sys.exit(not result.wasSuccessful())
