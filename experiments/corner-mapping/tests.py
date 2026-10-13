# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Check ordered loader output across mapping variants and worker counts."""
import argparse
import json
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from run import (DEFAULT_VARIANTS, ROOT, SERIAL_VARIANTS, canonical_geometry, configurations_for,
                 parse_workers, validate_probe)


TRIANGLE = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
QUAD = TRIANGLE + "v 0 0 1\n"
COMPACT_VARIANTS = {"compact", "compact_release", "fast", "fast_release"}


def fixture_cases():
    seam = (QUAD + "vt 0 0\nvt 1 0\nvt 0 1\nvt .5 .5\n"
            "g first\nf 1/1 2/2 3/3\ng seam\nf 1/4 3/3 4/2\n"
            "g reused\nf 1/1 2/2 3/3\n")
    yield "uv_seams_across_groups", seam, {"vertices": 5, "triangles": 3, "groups_with_texcoords": 3}
    yield "hard_normals", (TRIANGLE + "vn 0 0 1\nvn 0 0 -1\n"
                           "g front\nf 1//1 2//1 3//1\ng back\nf 1//2 3//2 2//2\n"), {
                               "vertices": 6, "triangles": 2, "groups_with_texcoords": 0}
    yield "missing_and_unused_attributes", (TRIANGLE + "vt 0 0\nvt 1 0\nvt 0 1\nvt .9 .9\n"
                                           "vn 0 0 1\nvn 0 1 0\n"
                                           "g full\nf 1/1/1 2/2/1 3/3/1\n"
                                           "g uv_only\nf 1/1 2/2 3/3\n"
                                           "g no_attributes\nf 1 3 2\n"), {
                                               "vertices": 9, "triangles": 3, "groups_with_texcoords": 2}
    yield "unused_attributes_position_faces", (TRIANGLE + "vt .5 .5\nvn 0 0 1\nf 3 1 2\n"), {
                                                   "vertices": 3, "triangles": 1, "groups_with_texcoords": 0}
    yield "uv_float_overflow_used", (TRIANGLE + "vt 1e39 .2\nvt .3 .4\nvt .5 .6\n"
                                      "f 1/1 2/2 3/3\n"), {
                                          "vertices": 3, "triangles": 1, "groups_with_texcoords": 0}
    yield "uv_float_overflow_unused", (TRIANGLE + "vt .1 .2\nvt .3 .4\nvt .5 .6\nvt 1e39 1e39\n"
                                        "f 1/1 2/2 3/3\n"), {
                                            "vertices": 3, "triangles": 1, "groups_with_texcoords": 1}
    yield "uv_overflow_then_valid_group", (TRIANGLE + "vt 1e39 .2\nvt .3 .4\nvt .5 .6\nvt .1 .2\n"
                                          "g overflow\nf 1/1 2/2 3/3\n"
                                          "g valid\nf 1/4 2/2 3/3\n"), {
                                              "vertices": 4, "triangles": 2, "groups_with_texcoords": 1}
    yield "position_only", TRIANGLE + "f 1 2 3\n", {"vertices": 3, "triangles": 1}
    yield "source_positions_without_primitives", TRIANGLE, {"vertices": 3, "triangles": 0, "source_positions": 3}
    yield "unused_source_positions", (QUAD + "v 9 9 9\nv -9 -9 -9\nf 4 2 3\n"), {
                                         "vertices": 3, "triangles": 1, "source_positions": 6}
    sparse = "".join(f"v {i} {i % 5} {i % 13}\n" for i in range(4096))
    sparse += "vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\nf 4096/1/1 3981/2/1 3001/3/1\nf 3001/3/1 3981/2/1 4096/1/1\n"
    yield "sparse_attribute_positions", sparse, {
        "vertices": 3, "triangles": 2, "source_positions": 4096, "groups_with_texcoords": 1}
    yield "out_of_order_first_use", (QUAD + "f 3 1 4\nf 2 4 1\n"), {"vertices": 4, "triangles": 2}
    yield "lines", QUAD + "g chain\nl 4 2 1\nl -2 -3\n", {"vertices": 4, "triangles": 0}
    yield "points", QUAD + "g dots\np 3 1 3\ng more\np -1 1\n", {"vertices": 3, "triangles": 0}
    yield "unused_attributes_lines", (QUAD + "vt .5 .5\nvn 0 0 1\ng chain\nl 4 2 1\nl -2 -3\n"), {
        "vertices": 4, "triangles": 0}
    yield "unused_attributes_vertex_cloud", (QUAD + "vt .5 .5\nvn 0 0 1\n"), {
        "vertices": 4, "triangles": 0, "source_positions": 4}
    yield "mixed_primitives", (QUAD + "vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 -1\n"
                                "g mixed\nf 1/1/1 2/2/1 3/3/1\nl 1 2 4\np 3 3 1\n"), {
                                    "vertices": 7, "triangles": 1, "groups_with_texcoords": 1}
    yield "negative_indices_concave_polygon", (
        "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\nv 9 9 9\n"
        "g concave\nf -6 -5 -4 -3 -2\ng triangle\nf -2 -3 -4\n"), {"vertices": 5, "triangles": 4}
    yield "freeform_surface", (
        "v 0 0 0\nv 2 0 0\nv 0 3 0\nv 2 3 0\n"
        "cstype bezier\ndeg 1 1\nsurf 0 1 0 1 1 2 3 4\nparm u 0 1\nparm v 0 1\nend\n"), {
            "vertices": 33 * 33, "triangles": 32 * 32 * 2}
    yield "freeform_mixed_authored_normals", (
        TRIANGLE + "vn 0 0 -1\ng face\nf 1//1 2//1 3//1\ng line\nl 1 2\ng dot\np 3\n"
        "g curve\ncstype bezier\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 1\nend\n"), {"triangles": 1}
    yield "face_and_freeform_without_explicit_primitives", (
        QUAD + "vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 -1\ng face\nf 1/1/1 2/2/1 3/3/1\n"
        "g curve\ncstype bezier\ndeg 2\ncurv 0 1 1 2 4\nparm u 0 1\nend\n"), {
            "triangles": 1, "groups_with_texcoords": 1}
    seams = 257
    text = QUAD + "".join(f"vt {i / seams:.17g} .5\n" for i in range(seams))
    text += "".join(f"g seam_{i}\nf 1/{i} 2/{i} 3/{i}\nf 1/{i} 3/{i} 4/{i}\n"
                    for i in range(1, seams + 1))
    yield "many_secondary_table_seams", text, {"vertices": 4 * seams, "triangles": 2 * seams}
    yield "large_coordinate_precision", (
        "v 1000000000000.125 -4.5 8\nv 1000000000000.5 -20.5 9\n"
        "v 1000000000000.25 4.25 -1\nf 3 1 2\n"), {"vertices": 3, "triangles": 1}


def probe(executable, path, configuration, cwd, timeout):
    environment = dict(os.environ, WOBY_MAPPING_UNIQUE_UVS="1" if configuration["unique_uv_validation"] else "0")
    process = subprocess.run([str(executable), str(path), str(configuration["parser_workers"]),
                              str(configuration["gather_workers"]), configuration["preparation"]],
                             cwd=cwd, capture_output=True, timeout=timeout, env=environment)
    if process.returncode:
        raise AssertionError(f"{executable.name} exited with {process.returncode}: "
                             f"{process.stderr.decode('utf-8', errors='replace')}")
    try:
        output = json.loads(process.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AssertionError(f"{executable.name} did not return JSON: {process.stdout!r}") from error
    validate_probe(output, configuration)
    return output


def check_fixture(name, content, expected, executables, configurations, timeout):
    checks = unittest.TestCase()
    # A fixture and its explicit working directory share one unique root.
    with tempfile.TemporaryDirectory(prefix=f"woby_corner_{name}_") as temporary:
        root = Path(temporary).resolve()
        model_dir = root / "模型 ü"
        model_dir.mkdir()
        path = model_dir / "corner 曲面.obj"
        path.write_text(content, encoding="utf-8")
        reference = {}
        for configuration in configurations:
            variant = configuration["variant"]
            with checks.subTest(fixture=name, variant=variant,
                                gather_workers=configuration["gather_workers"],
                                preparation=configuration["preparation"]):
                output = probe(executables[variant], path, configuration, root, timeout)
                geometry = canonical_geometry(output)
                for key, value in expected.items():
                    actual = (sum(node["has_texcoords"] for node in geometry["nodes"])
                              if key == "groups_with_texcoords" else geometry[key])
                    checks.assertEqual(actual, value, key)
                # Deferred source indices and original bounds can differ from eager data.
                reference_geometry = reference.setdefault(configuration["preparation"], geometry)
                checks.assertEqual(geometry, reference_geometry, "Ordered geometry differs from baseline.")
                for fingerprint in ("geometry_fingerprint", "normal_fingerprint", "source_fingerprint"):
                    checks.assertIn(fingerprint, geometry)
                mapping = next(event for event in output["events"] if event["name"] == "corner_mapping")
                checks.assertIsInstance(mapping["data"], dict)
                checks.assertGreaterEqual(mapping["working_set_end"], 0)


def cancellation_fixture(with_seams=False):
    triangles = 24000
    positions = "".join(f"v {i * 2} 0 0\nv {i * 2 + 1} 0 0\nv {i * 2} 1 0\n" for i in range(triangles))
    # This first-use order also forces the mapped source-position pass.
    faces = "f 3/1/1 1/1/1 2/1/1\n"
    faces += "".join(f"f {i * 3 + 1}/1/1 {i * 3 + 2}/1/1 {i * 3 + 3}/1/1\n" for i in range(1, triangles))
    if with_seams:
        faces += "".join(f"f {i * 3 + 1}/2/1 {i * 3 + 2}/2/1 {i * 3 + 3}/2/1\n" for i in range(triangles))
    return positions + "vt .5 .5\nvt .25 .25\nvn 0 0 1\n" + faces


def check_cancellation(executables):
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_corner_cancel_") as temporary:
        root = Path(temporary).resolve()
        path = root / "large-seams.obj"
        path.write_text(cancellation_fixture(with_seams=True), encoding="utf-8")
        identity_path = root / "large-identity-check.obj"
        identity_path.write_text(cancellation_fixture(), encoding="utf-8")
        variants = [variant for variant in executables if variant not in SERIAL_VARIANTS]
        for variant in variants:
            phases = [("mapping_resolve", 2), ("mapping_position_indices", 2)]
            if variant in COMPACT_VARIANTS:
                phases += [("mapping_vertex_allocate", 1), ("mapping_position_identity", 1),
                           ("mapping_primary_gather", 1), ("mapping_seam_gather", 1)]
            else:
                phases += [("mapping_attribute_gather", 1)]
            if variant.endswith("release"):
                phases += [("mapping_input_release", 1)]
            for phase, count in phases:
                start = time.perf_counter()
                selected_path = identity_path if phase == "mapping_position_identity" else path
                process = subprocess.run([str(executables[variant]), str(selected_path), "2", "8", "deferred",
                                          f"cancel:{phase}:{count}"], cwd=root,
                                         capture_output=True, timeout=20)
                checks.assertNotEqual(process.returncode, 0, f"Cancellation did not stop {variant}/{phase}.")
                checks.assertIn("Mapping cancellation test", process.stderr.decode("utf-8", errors="replace"))
                checks.assertLess(time.perf_counter() - start, 10, "Cancellation did not finish promptly.")


def check_parallel_gather(executables):
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_corner_parallel_") as temporary:
        root = Path(temporary).resolve()
        path = root / "threaded-gather.obj"
        path.write_text(cancellation_fixture(with_seams=True), encoding="utf-8")
        variants = [variant for variant in executables if variant != "pointer"]
        reference = None
        for configuration in configurations_for(variants, 2, [1, 2, 8], ["deferred"]):
            output = probe(executables[configuration["variant"]], path, configuration, root, 30)
            geometry = canonical_geometry(output)
            checks.assertEqual(geometry["vertices"], 144000)
            if reference is None:
                reference = geometry
            checks.assertEqual(geometry, reference)
            if configuration["variant"] not in SERIAL_VARIANTS:
                names = ({"mapping_primary_gather", "mapping_seam_gather"}
                         if configuration["variant"] in COMPACT_VARIANTS else {"mapping_attribute_gather"})
                gathers = [event for event in output["events"] if event["name"] in names]
                checks.assertEqual({event["name"] for event in gathers}, names)
                for gather in gathers:
                    checks.assertGreaterEqual(gather["data"]["workers"], 1)
                    checks.assertLessEqual(gather["data"]["workers"], configuration["gather_workers"])
                    if configuration["gather_workers"] > 1:
                        checks.assertGreater(gather["data"]["workers"], 1, "The fixture did not exercise parallel gathering.")
                checks.assertEqual(sum(event["data"]["vertices"] for event in gathers), 144000)


def check_serial_controls_ignore_gather_workers(executables):
    checks = unittest.TestCase()
    with tempfile.TemporaryDirectory(prefix="woby_corner_serial_controls_") as temporary:
        root = Path(temporary).resolve()
        path = root / "serial.obj"
        path.write_text(TRIANGLE + "vt .5 .5\nf 1/1 2/1 3/1\n", encoding="utf-8")
        for variant in ("baseline", "pointer"):
            if variant not in executables:
                continue
            configuration = configurations_for([variant], 2, [1], ["deferred"])[0]
            reference = canonical_geometry(probe(executables[variant], path, configuration, root, 20))
            configuration["gather_workers"] = 8
            output = probe(executables[variant], path, configuration, root, 20)
            checks.assertEqual(canonical_geometry(output), reference)
            checks.assertNotIn("mapping_attribute_gather", {event["name"] for event in output["events"]})


def check_rejected_attribute_indices(executables):
    checks = unittest.TestCase()
    attributes = TRIANGLE + "vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n"
    cases = {
        "uv_positive_out_of_range": "f 1/4 2/2 3/3\n",
        "uv_negative_out_of_range": "f 1/-4 2/-2 3/-1\n",
        "normal_positive_out_of_range": "f 1//2 2//1 3//1\n",
        "normal_negative_out_of_range": "f 1//-2 2//-1 3//-1\n",
    }
    for name, face in cases.items():
        with tempfile.TemporaryDirectory(prefix=f"woby_corner_reject_{name}_") as temporary:
            root = Path(temporary).resolve()
            path = root / "invalid attributes ü.obj"
            path.write_text(attributes + face, encoding="utf-8")
            for variant, executable in executables.items():
                process = subprocess.run([str(executable), str(path), "2", "8", "deferred"],
                                         cwd=root, capture_output=True, timeout=20)
                checks.assertNotEqual(process.returncode, 0, f"{variant} accepted {name}.")
                checks.assertTrue(process.stderr.strip(), f"{variant} returned no diagnostic for {name}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, default=ROOT / "build/vs2026-vcpkg/bin/Release")
    parser.add_argument("--variants", default=DEFAULT_VARIANTS)
    parser.add_argument("--gather-workers", type=parse_workers, default=parse_workers("1,8"))
    parser.add_argument("--preparation", default="deferred,eager")
    parser.add_argument("--unique-uv-validation", default="on,off")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--fixture", action="append", default=[], help="Run only the named fixture; can be repeated.")
    parser.add_argument("--skip-cancellation", action="store_true")
    args = parser.parse_args()
    variants = args.variants.split(",")
    if "baseline" not in variants:
        parser.error("Include baseline so each fixture has a reference result.")
    variants = ["baseline"] + [variant for variant in variants if variant != "baseline"]
    suffix = ".exe" if os.name == "nt" else ""
    executables = {variant: (args.bin / f"woby_corner_mapping_{variant}{suffix}").resolve(strict=True)
                   for variant in variants}
    uv_modes = args.unique_uv_validation.split(",")
    if any(mode not in {"on", "off"} for mode in uv_modes):
        parser.error("Unique UV validation must be on or off.")
    configurations = configurations_for(variants, 2, args.gather_workers,
                                        args.preparation.split(","), [mode == "on" for mode in uv_modes])
    fixtures = [fixture for fixture in fixture_cases() if not args.fixture or fixture[0] in args.fixture]
    if not fixtures or set(args.fixture) - {fixture[0] for fixture in fixtures}:
        parser.error("A requested fixture does not exist.")
    suite = unittest.TestSuite()
    for name, content, expected in fixtures:
        suite.addTest(unittest.FunctionTestCase(
            lambda name=name, content=content, expected=expected:
            check_fixture(name, content, expected, executables, configurations, args.timeout), description=name))
    if not args.skip_cancellation:
        suite.addTest(unittest.FunctionTestCase(lambda: check_cancellation(executables), description="mapping cancellation"))
    if not args.fixture:
        suite.addTest(unittest.FunctionTestCase(lambda: check_parallel_gather(executables), description="parallel gather"))
        suite.addTest(unittest.FunctionTestCase(lambda: check_serial_controls_ignore_gather_workers(executables),
                                                description="serial gather-worker controls"))
        suite.addTest(unittest.FunctionTestCase(lambda: check_rejected_attribute_indices(executables),
                                                description="invalid UV and normal indices"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())


if __name__ == "__main__":
    main()
