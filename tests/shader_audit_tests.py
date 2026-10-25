"""Shader probe input, paired reporting, and optional GPU control tests."""
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/"experiments/point-pipeline"/(name+".py"))
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value


audit = module("shader_audit")
analysis = module("audit_summary")


def expect_value_error(function):
    try:
        function()
    except ValueError:
        return
    raise AssertionError("Expected invalid evidence to be rejected")


def report_fixture():
    return dict(exact_samples=True,rounds=[dict(kind="points",point_size=4,variant=variant,round=0,gpu_ms=times)
        for variant,times in (("reference",[100,1]),("candidate",[10,.9]))],
        validation=[dict(kind="points",point_size=4,variant=variant,samples=100,
            different_colors=0,different_ids=0,different_depths=0) for variant in ("reference","candidate")])


def test_workload_validation():
    assert audit.workloads(["solid", "combined:1", "quads:40"]) == [
        dict(kind="solid",size=4),dict(kind="combined",size=1),dict(kind="quads",size=40)]
    for value in ("unknown", "points:0", "points:41", "points:nan", "points:inf", "points:4:5"):
        expect_value_error(lambda: audit.workloads([value]))
    expect_value_error(lambda: audit.workloads(["solid", "solid:4"]))


def test_pairs_each_camera_frame_before_summary():
    candidate = analysis.summarize(report_fixture())[1]
    assert math.isclose(candidate["reduction_percent"],50)
    assert math.isclose(candidate["median_ms"],5.45)
    assert candidate["exact"]


def test_sample_changes_are_never_hidden_by_timing():
    for field in ("different_colors", "different_ids", "different_depths"):
        report = report_fixture(); report["validation"][1][field] = 1
        assert not analysis.summarize(report)[1]["exact"]


def test_rejects_incomplete_invalid_or_duplicate_evidence():
    cases = []
    report = report_fixture(); report["rounds"][1]["gpu_ms"].pop(); cases.append(report)
    report = report_fixture(); report["rounds"].append(copy.deepcopy(report["rounds"][0])); cases.append(report)
    report = report_fixture(); report["validation"].pop(); cases.append(report)
    report = report_fixture(); report["rounds"].clear(); cases.append(report)
    report = report_fixture(); report["validation"][0]["samples"] = 0; cases.append(report)
    report = report_fixture(); del report["exact_samples"]; cases.append(report)
    report = report_fixture(); report["rounds"].pop(0); cases.append(report)
    report = report_fixture(); extra=copy.deepcopy(report["rounds"][0]); extra["round"]=1
    report["rounds"].append(extra); cases.append(report)
    for invalid in (0, -1, float("nan"), float("inf")):
        report = report_fixture(); report["rounds"][1]["gpu_ms"][0] = invalid; cases.append(report)
    for report in cases:
        expect_value_error(lambda: analysis.summarize(report))


def gpu_control(binary, shaders):
    binary, shaders = Path(binary).resolve(), Path(shaders).resolve()
    # All related fixture and output paths are under one unique temporary root.
    # The binary and shader paths remain absolute on their original drive.
    with tempfile.TemporaryDirectory(prefix="woby shader audit ") as temporary:
        root = Path(temporary)
        for samples in (1,4):
            configuration = root/f"configuration-{samples}.json"
            output = root/f"result-{samples}.json"
            configuration.write_text(json.dumps(dict(model="fixture",frames=1,rounds=1,samples=samples,
                variants=[dict(name=name,directory=str(shaders)) for name in ("reference","repeat")],
                workloads=audit.workloads(["solid", "edges", "pulled", "lines", "uv", "comparison", "quads:4",
                    "combined:1", "combined:8", "points:40", "transparent", "transparent-edges", "transparent-pulled", "line-sprites"])) ))
            subprocess.run([str(binary),"--audit",str(configuration),str(output)],cwd=root,check=True,
                env=dict(os.environ,VK_LOADER_LAYERS_DISABLE="~implicit~"),timeout=100)
            report = json.loads(output.read_text())
            assert report["exact_samples"]
            assert report["groups"]==4
            assert len(report["validation"])==56
            assert all(row["non_background_samples"]>0 for row in report["validation"])
            assert all(row["exact"] for row in analysis.summarize(report))


if __name__ == "__main__":
    if len(sys.argv)==4 and sys.argv[1]=="--gpu": gpu_control(*sys.argv[2:])
    else:
        suite=unittest.TestSuite(unittest.FunctionTestCase(test) for test in (
            test_workload_validation, test_pairs_each_camera_frame_before_summary,
            test_sample_changes_are_never_hidden_by_timing, test_rejects_incomplete_invalid_or_duplicate_evidence))
        sys.exit(0 if unittest.TextTestRunner().run(suite).wasSuccessful() else 1)
