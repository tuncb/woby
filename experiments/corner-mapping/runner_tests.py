# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Check run scheduling, exact comparisons, paired summaries, and saved failures."""
import argparse
import copy
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

from run import (canonical_geometry, configuration_key, configurations_for, DEFAULT_VARIANTS, execute_sweep,
                 measure, parse_workers, ROOT, rotated_order, stage_wall_ms, summarize, validate_probe)


def configuration(variant="baseline", preparation="deferred", workers=1):
    return {"variant": variant, "parser_workers": 2, "gather_workers": workers,
            "preparation": preparation, "unique_uv_validation": True}


def output_for(config, load_ms=100):
    return {"variant": config["variant"], "parser_workers_requested": config["parser_workers"],
            "gather_workers_requested": config["gather_workers"], "deferred": config["preparation"] == "deferred",
            "unique_uv_validation": config["unique_uv_validation"],
            "cpu_load_ms": load_ms,
            "geometry": {"vertices": 3, "geometry_fingerprint": 17, "normal_fingerprint": 19,
                         "source_fingerprint": 23, "nodes": [{"name": "triangle", "indexOffset": 0}],
                         "retained_buffer_capacity_bytes": 1024},
            "events": [{"name": "corner_mapping", "wall_ms": load_ms / 2, "cpu_ms": load_ms / 2,
                        "working_set_end": 1000, "data": {"buffers": [{"bytes": 128}], "passes": 2}}]}


def record_for(config, round_index, load_ms):
    return {"configuration": config, "round": round_index, "process_wall_ms": load_ms + 20,
            "probe": output_for(config, load_ms)}


def check_schedule_rotation_and_serial_controls():
    checks = unittest.TestCase()
    configs = configurations_for(["baseline", "pointer", "split", "dense"], 16, [1, 2, 8], ["deferred", "eager"])
    checks.assertEqual(len(configs), 16)
    for variant in ("baseline", "pointer"):
        checks.assertEqual({row["gather_workers"] for row in configs if row["variant"] == variant}, {1})
    expected_keys = [configuration_key(row) for row in configs]
    checks.assertEqual(len(set(expected_keys)), len(configs))
    first_positions = []
    for round_index in range(len(configs)):
        order = rotated_order(configs, round_index)
        checks.assertEqual(sorted(configuration_key(row) for row in order), sorted(expected_keys))
        first_positions.append(configuration_key(order[0]))
    checks.assertEqual(first_positions, expected_keys)
    checks.assertEqual(rotated_order(configs, len(configs)), configs)
    checks.assertEqual(parse_workers("1,2,8"), [1, 2, 8])
    for workers in ("", "0", "33", "65", "1,1", "two"):
        with checks.assertRaises(argparse.ArgumentTypeError):
            parse_workers(workers)
    checks.assertEqual(parse_workers("32"), [32])
    with checks.assertRaisesRegex(ValueError, "1 to 32 gather workers"):
        configurations_for(["split"], 16, [33], ["deferred"])


def check_comparison_keeps_normal_source_and_order():
    checks = unittest.TestCase()
    config = configuration()
    baseline = output_for(config)
    validate_probe(baseline, config)
    capacity_only = copy.deepcopy(baseline)
    capacity_only["geometry"]["retained_buffer_capacity_bytes"] = 4096
    checks.assertEqual(canonical_geometry(baseline), canonical_geometry(capacity_only))
    for field in ("geometry_fingerprint", "normal_fingerprint", "source_fingerprint"):
        changed = copy.deepcopy(baseline)
        changed["geometry"][field] += 1
        checks.assertNotEqual(canonical_geometry(baseline), canonical_geometry(changed))
    changed = copy.deepcopy(baseline)
    changed["geometry"]["nodes"][0]["indexOffset"] = 3
    checks.assertNotEqual(canonical_geometry(baseline), canonical_geometry(changed))
    for field, value in (("variant", "split"), ("gather_workers_requested", 8), ("unique_uv_validation", False),
                         ("deferred", False), ("cpu_load_ms", -1)):
        changed = copy.deepcopy(baseline)
        changed[field] = value
        with checks.assertRaises(ValueError):
            validate_probe(changed, config)


def check_summaries_pair_same_round_and_preparation():
    checks = unittest.TestCase()
    baseline, split = configuration(), configuration("split", workers=8)
    eager = configuration(preparation="eager")
    records = [record_for(baseline, 1, 100), record_for(split, 1, 50),
               record_for(eager, 1, 1000), record_for(baseline, 2, 200),
               record_for(split, 2, 160), record_for(split, 3, 400)]
    records[0]["probe"]["events"].append({"name": "corner_mapping", "wall_ms": 20,
                                        "cpu_ms": 10, "working_set_end": 2000})
    summary = summarize(records)
    split_key = configuration_key(split)
    checks.assertEqual(summary["configurations"][split_key]["cpu_load_median_ms"], 160)
    pair = summary["paired_with_baseline"][split_key]
    checks.assertEqual(pair["pairs"], 2)
    checks.assertAlmostEqual(pair["load_ratio_median"], .65)
    checks.assertAlmostEqual(pair["load_change_percent_median"], -35)
    buffers = summary["configurations"][split_key]["pass_and_buffer_data"]
    checks.assertEqual(buffers["corner_mapping.buffers.0.bytes"], {"median": 128, "min": 128, "max": 128})
    events = summary["configurations"][configuration_key(baseline)]["event_medians"]
    checks.assertEqual(events["corner_mapping.wall_ms"], 85)
    checks.assertEqual(events["corner_mapping.working_set_end"], 1500)


def check_mapping_measurement_includes_allocation_once():
    checks = unittest.TestCase()
    baseline, dense = configuration(), configuration("dense", workers=8)
    reference, variant = record_for(baseline, 1, 100), record_for(dense, 1, 80)
    reference["probe"]["events"].append({"name": "mesh_allocation", "wall_ms": 10, "cpu_ms": 5})
    variant["probe"]["events"].extend([
        {"name": "mesh_allocation", "wall_ms": 20, "cpu_ms": 10},
        {"name": "mapping_attribute_gather", "wall_ms": 5, "cpu_ms": 10}])
    variant["probe"]["memory"] = {"vertex_capacity_bytes": 4096, "position_map_capacity_bytes": 1024}
    checks.assertEqual(stage_wall_ms(variant["probe"], "mesh_allocation", "corner_mapping"), 60)
    summary = summarize([reference, variant])
    key = configuration_key(dense)
    checks.assertEqual(summary["configurations"][key]["mapping_pipeline_median_ms"], 60)
    checks.assertEqual(summary["paired_with_baseline"][key]["mapping_ratio_median"], 1)
    checks.assertEqual(summary["configurations"][key]["memory_data"]["vertex_capacity_bytes"],
                       {"median": 4096, "min": 4096, "max": 4096})


def python_probe_source(vary_geometry=False):
    output = output_for(configuration())
    source = f"import json, os, pathlib, sys\noutput = {output!r}\n"
    source += "output['unique_uv_validation'] = os.environ.get('WOBY_MAPPING_UNIQUE_UVS', '1') != '0'\n"
    if vary_geometry:
        source += ("counter = pathlib.Path(__file__).with_suffix('.counter')\n"
                   "count = int(counter.read_text()) + 1 if counter.exists() else 1\n"
                   "counter.write_text(str(count))\n"
                   "output['geometry']['geometry_fingerprint'] = count\n")
    source += "print(json.dumps(output))\n"
    return source


def check_raw_snapshots_and_existing_output_protection():
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_corner_runner_") as temporary:
        root = Path(temporary).resolve()
        model = root / "fixture.obj"
        model.write_text(python_probe_source(), encoding="utf-8")
        output = root / "measurements.json"
        executable = Path(sys.executable).resolve(strict=True)
        result = execute_sweep(model, {"baseline": executable}, [configuration()], 2, 30, output)
        checks.assertEqual(result["status"], "complete")
        checks.assertEqual(len(result["records"]), 2)
        original = output.read_bytes()
        for record in result["records"]:
            snapshot = Path(record["stdout_snapshot"])
            checks.assertEqual(json.loads(snapshot.read_bytes()), record["probe"])
            checks.assertTrue(snapshot.with_suffix(".command.json").is_file())
        with checks.assertRaises(FileExistsError):
            execute_sweep(model, {"baseline": executable}, [configuration()], 1, 30, output)
        checks.assertEqual(output.read_bytes(), original)


def check_geometry_failure_is_saved_before_rejection():
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_corner_failure_") as temporary:
        root = Path(temporary).resolve()
        model = root / "fixture.obj"
        model.write_text(python_probe_source(vary_geometry=True), encoding="utf-8")
        output = root / "failed.json"
        with checks.assertRaisesRegex(ValueError, "Ordered geometry differs"):
            execute_sweep(model, {"baseline": Path(sys.executable).resolve(strict=True)},
                          [configuration()], 2, 30, output)
        saved = json.loads(output.read_bytes())
        checks.assertEqual(saved["status"], "failed")
        checks.assertEqual(len(saved["records"]), 2)
        for record in saved["records"]:
            checks.assertTrue(Path(record["stdout_snapshot"]).is_file())


def check_process_failure_preserves_stderr():
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_corner_process_failure_") as temporary:
        root = Path(temporary).resolve()
        model = root / "fixture.obj"
        model.write_text("import sys\nprint('failure details', file=sys.stderr)\nsys.exit(7)\n", encoding="utf-8")
        snapshot = root / "failure.stdout.json"
        with checks.assertRaisesRegex(RuntimeError, "exited with 7"):
            measure(Path(sys.executable).resolve(strict=True), model, configuration(), 30, snapshot)
        checks.assertTrue(snapshot.is_file())
        checks.assertIn("failure details", snapshot.with_suffix(".stderr.txt").read_text(encoding="utf-8"))


def check_generation_uses_current_loader_and_rejects_anchor_drift():
    checks = unittest.TestCase()
    spec = importlib.util.spec_from_file_location("corner_mapping_generator", Path(__file__).with_name("generate.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    original = (ROOT / "src/obj_mesh.cpp").read_bytes()
    with tempfile.TemporaryDirectory(prefix="woby_corner_generation_") as temporary:
        generated = Path(temporary).resolve()
        module.generate(ROOT, generated)
        for variant in DEFAULT_VARIANTS.split(","):
            checks.assertTrue((generated / f"obj_{variant}.cpp").is_file())
        baseline = (generated / "obj_baseline.cpp").read_text(encoding="utf-8")
        checks.assertIn("#ifdef OBJ_PROFILE_UV_PTR", baseline)
        split = (generated / "obj_split.cpp").read_text(encoding="utf-8")
        checks.assertIn("resolveSeparateFaces<Prototype>(", split)
        checks.assertIn("gatherResolvedVertices<Prototype>(", split)
        checks.assertIn("tagged & renderIdMask", split)
        compact = (generated / "obj_compact.cpp").read_text(encoding="utf-8")
        checks.assertIn('"mapping_primary_gather"', compact)
        checks.assertIn('"mapping_seam_gather"', compact)
        checks.assertIn('"mapping_position_identity"', compact)
        fast = (generated / "obj_fast.cpp").read_text(encoding="utf-8")
        checks.assertIn('"mapping_render_index_allocate"', fast)
        checks.assertIn('"primary_hit_inline", true', fast)
        release = (generated / "obj_release.cpp").read_text(encoding="utf-8")
        checks.assertIn("releaseResolvedTriangles(result, vertexMap, indexCount, progress, totalIndices)", release)
    checks.assertEqual((ROOT / "src/obj_mesh.cpp").read_bytes(), original)
    for source in ("no matching text", "anchor anchor"):
        with checks.assertRaisesRegex(ValueError, "anchor drift"):
            module.replace(source, "anchor", "replacement")


def check_environment_modes_have_separate_pairs():
    checks = unittest.TestCase()
    baseline_on, split_on = configuration(), configuration("split")
    baseline_off, split_off = copy.deepcopy(baseline_on), copy.deepcopy(split_on)
    baseline_off["unique_uv_validation"] = False
    split_off["unique_uv_validation"] = False
    summary = summarize([record_for(baseline_on, 1, 100), record_for(split_on, 1, 50),
                         record_for(baseline_off, 1, 1000), record_for(split_off, 1, 800)])
    checks.assertEqual(summary["paired_with_baseline"][configuration_key(split_on)]["load_ratio_median"], .5)
    checks.assertEqual(summary["paired_with_baseline"][configuration_key(split_off)]["load_ratio_median"], .8)


def check_measurement_freezes_reserve_environment():
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_corner_environment_") as temporary:
        root = Path(temporary).resolve()
        model = root / "environment-probe.obj"
        config = configuration()
        config["unique_uv_validation"] = False
        output = output_for(config)
        model.write_text(
            f"import json, os\noutput = {output!r}\n"
            "output['observed_reserve_percent'] = os.environ.get('WOBY_PROFILE_RESERVE_PERCENT')\n"
            "output['unique_uv_validation'] = os.environ.get('WOBY_MAPPING_UNIQUE_UVS') != '0'\n"
            "print(json.dumps(output))\n", encoding="utf-8")
        snapshot = root / "environment.stdout.json"
        with mock.patch.dict(os.environ, {"WOBY_PROFILE_RESERVE_PERCENT": "101", "WOBY_MAPPING_UNIQUE_UVS": "1"}):
            record = measure(Path(sys.executable).resolve(strict=True), model, config, 30, snapshot)
        checks.assertEqual(record["probe"]["observed_reserve_percent"], "0")
        checks.assertFalse(record["probe"]["unique_uv_validation"])
        checks.assertEqual(json.loads(snapshot.with_suffix(".environment.json").read_bytes()),
                           {"WOBY_PROFILE_RESERVE_PERCENT": "0", "WOBY_MAPPING_UNIQUE_UVS": "0"})


if __name__ == "__main__":
    suite = unittest.TestSuite(unittest.FunctionTestCase(test) for test in (
        check_schedule_rotation_and_serial_controls, check_comparison_keeps_normal_source_and_order,
        check_summaries_pair_same_round_and_preparation, check_mapping_measurement_includes_allocation_once,
        check_raw_snapshots_and_existing_output_protection,
        check_geometry_failure_is_saved_before_rejection, check_process_failure_preserves_stderr,
        check_generation_uses_current_loader_and_rejects_anchor_drift, check_environment_modes_have_separate_pairs,
        check_measurement_freezes_reserve_environment))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
