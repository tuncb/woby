"""Contract checks for the research harness; no wall-clock assertions."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

NATIVE, PROFILE = (Path(p).resolve() for p in sys.argv[1:3])
ROOT = Path(__file__).resolve().parents[2]
QUERIES = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None
TOKENREF = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else None
MAX3 = Path(sys.argv[5]).resolve() if len(sys.argv) > 5 else None


def run(exe, *args):
    process = subprocess.run([str(exe), *map(str, args)], text=True, capture_output=True, timeout=90)
    if process.returncode:
        raise AssertionError(process.stderr)
    return json.loads(process.stdout)


def test_native_profile_equivalence():
    args = ["--grid", "12", "--runs", "1", "--operations",
            "source,auto,duplicate-points,duplicate-triangles,degenerates,quality,intersections,distance,difference,topology-warm,intersections-warm,display"]
    native, profile = run(NATIVE, *args), run(PROFILE, *args)
    assert native["samples"] == profile["samples"]
    assert all(s["status"] == "ok" for s in native["samples"]), native["samples"]
    distance = next(s for s in native["samples"] if s["operation"] == "distance")["result"]
    assert distance["surfaces"][0]["distance_samples"] == 12 * 12 * 8
    assert abs(distance["surfaces"][0]["mean"] - native["mesh"]["shift"]) < 1e-10
    gap = next(s for s in native["samples"] if s["operation"] == "difference")["result"]
    assert gap["constructed"] and gap["closure_triangles"] == 8*12
    assert abs(gap["volumes"][0]-12*12*native["mesh"]["shift"]) < 1e-8
    assert gap["volumes"][1] == 0
    assert all(s["geometry_reused"] for s in native["samples"] if "geometry_reused" in s)
    assert len(profile["events"]) > len(native["events"])
    for row in (native, profile):
        assert all(e["end_ms"] >= e["start_ms"] and e["wall_ms"] >= 0 for e in row["events"])
        assert len([e for e in row["events"] if e["name"].startswith("op.")]) == len(row["samples"])


def test_helpers_and_expansion():
    args = ["--grid", "48", "--runs", "1", "--operations", "auto,quality,distance", "--exact"]
    serial = run(NATIVE, *args, "--helpers", "0")
    expanded = run(NATIVE, *args, "--expanded")
    assert serial["mesh"]["source_points"] == expanded["mesh"]["source_points"]
    assert expanded["mesh"]["vertices"] == expanded["mesh"]["triangles"] * 3
    for a, b in zip(serial["samples"], expanded["samples"]):
        assert a["status"] == b["status"] == "ok"
        assert a["result"]["detector_counts"] == b["result"]["detector_counts"]
        for sa, sb in zip(a["result"]["surfaces"], b["result"]["surfaces"]):
            for key in ("mean", "maximum", "p95", "quality_triangles"):
                assert abs(sa[key] - sb[key]) < 1e-9


def test_closed_difference():
    result = run(PROFILE, "--sphere", "16", "--runs", "1", "--operations", "difference", "--shift", ".05")
    sample = result["samples"][0]
    assert sample["status"] == "ok", sample
    assert not sample["result"]["constructed"]
    assert all(v > 0 for v in sample["result"]["volumes"])
    assert sample["result"]["closure_triangles"] == 0


def test_query_counters_and_batch_selection():
    if QUERIES is None:
        return
    args = ["--grid","64","--runs","1","--operations","distance"]
    reference = run(NATIVE,*args); counted = run(QUERIES,*args)
    assert reference["samples"] == counted["samples"]
    phases = [e for e in counted["events"] if e["name"] == "distance.queries"]
    assert len(phases) == 2
    assert all(e["data"]["queries"] == 32768 for e in phases)
    assert all(e["data"]["triangle_tests"] >= e["data"]["queries"] for e in phases)
    assert all(sum(e["data"]["visit_histogram"]) == e["data"]["queries"] for e in phases)
    assert all(sum(e["data"]["triangle_histogram"]) == e["data"]["queries"] for e in phases)
    selected = run(QUERIES,*args,"--query-stride","2")
    assert not selected["distance_statistics_valid"]
    assert all(e["data"]["queries"] == 16384 for e in selected["events"] if e["name"] == "distance.queries")
    assert not any(e["name"] == "distance.statistics" for e in selected["events"])


def test_token_reference_control():
    if TOKENREF is None:
        return
    for shape in (["--grid","24"],["--sphere","24"]):
        args = [*shape,"--runs","1","--operations","quality"]
        native = run(NATIVE,*args)
        assert native["samples"] == run(TOKENREF,*args)["samples"]
        assert native["samples"] == run(NATIVE,*args,"--empty-token")["samples"]


def test_high_qos_process_control():
    args = ["--grid","12","--runs","1","--operations","quality,distance"]
    normal = run(NATIVE,*args); high = run(NATIVE,*args,"--high-qos")
    assert normal["samples"] == high["samples"]
    policy = high["process_power_policy"]
    assert policy["high_qos_requested"]
    assert policy["control_mask_after"] & 1 and not policy["state_mask_after"] & 1
    assert not normal["process_power_policy"]["high_qos_requested"]


def test_intersection_predicate_counts():
    if QUERIES is None:
        return
    args = ["--grid","24","--runs","1","--operations","intersections,difference"]
    reference = run(NATIVE,*args); counted = run(QUERIES,*args)
    assert reference["samples"] == counted["samples"]
    phases = [e for e in counted["events"] if e["name"] == "mesh_intersections.inspectIntersections"]
    assert len(phases) >= 3
    for phase in phases:
        data = phase["data"]
        assert data["classifier_calls"] == sum(data[k] for k in ("fast_separated","adaptive_false","adaptive_true","rational_fallback"))
        assert data["node_box_tests"] > 0 and data["face_box_tests"] >= data["classifier_calls"]
    assert phases[0]["data"]["classifier_calls"] == counted["samples"][0]["result"]["surfaces"][0]["intersection"]["candidate_tests"]


def test_three_value_max_distance_control():
    if MAX3 is None:
        return
    for shape in (["--grid","24"],["--sphere","32"]):
        args = [*shape,"--runs","1","--operations","distance","--shift",".003"]
        assert run(NATIVE,*args)["samples"] == run(MAX3,*args)["samples"]
    with tempfile.TemporaryDirectory(prefix="woby-detector-max3-") as directory:
        root = Path(directory).resolve(); model = root / "edge-cases.obj"
        triangles = [((0,0,0),(0,0,0),(0,0,0)),((0,0,0),(1,0,0),(2,0,0)),
            ((0,0,0),(1,0,0),(1,1e-13,0)),((0,0,0),(1,0,0),(1,1e-10,0)),
            ((-1e8,0,0),(1e8,0,0),(0,1e8,1e8)),((0,0,0),(1e-10,0,0),(0,1e-10,0))]
        vertices = [f"v {x} {y} {z}" for triangle in triangles for x,y,z in triangle]
        faces = [f"f {3*i+1} {3*i+2} {3*i+3}" for i in range(len(triangles))]
        model.write_text("\n".join([*vertices,*faces])+"\n",encoding="utf-8")
        args = ["--model",model,"--runs","1","--operations","distance","--shift",".001"]
        expected = run(NATIVE,*args)
        assert expected["samples"][0]["status"] == "ok"
        assert expected["samples"] == run(MAX3,*args)["samples"]


def test_local_deformation_pair():
    args = ["--grid","48","--shift","0","--bump",".05","--runs","1","--operations","distance"]
    serial = run(NATIVE,*args,"--helpers","0"); parallel = run(PROFILE,*args)
    assert serial["samples"] == parallel["samples"]
    assert 0 < serial["mesh"]["bump_changed_vertices"] < serial["mesh"]["vertices"]
    for surface in serial["samples"][0]["result"]["surfaces"]:
        assert 0 < surface["mean"] < serial["mesh"]["diagonal"]*.05
        assert surface["distance_samples"] == 48*48*8


def test_shuffle_keeps_part_quality():
    with tempfile.TemporaryDirectory(prefix="woby-detector-order-") as directory:
        root = Path(directory).resolve(); model = root / "parts.obj"
        vertices = [f"v {x} {y} 0" for y in range(3) for x in range(3)]
        lines = [*vertices,"g lower","f 1 2 5","f 1 5 4","f 2 3 6","f 2 6 5",
            "g upper","f 4 5 8","f 4 8 7","f 5 6 9","f 5 9 8"]
        model.write_text("\n".join(lines)+"\n",encoding="utf-8")
        args = ["--model",model,"--runs","1","--operations","quality"]
        original = run(NATIVE,*args); shuffled = run(NATIVE,*args,"--shuffle")
        assert original["mesh"]["parts"] == shuffled["mesh"]["parts"] == 2
        assert original["samples"] == shuffled["samples"]


def test_pre_canceled_operations():
    for exe in (NATIVE,PROFILE,TOKENREF,MAX3):
        if exe is None:
            continue
        result = run(exe,"--grid","32","--runs","1","--cancel-after-ms","0",
            "--operations","auto,quality,intersections,distance,difference")
        assert all(s["status"] == "error" and "canceled" in s["error"].lower() for s in result["samples"])
        assert all(s["cancel_requested_ms"] >= 0 and s["cancel_response_ms"] >= 0 for s in result["samples"])


def test_source_batches_keep_geometry_and_separate_duplicate_domains():
    with tempfile.TemporaryDirectory(prefix="woby-detector-batches-") as directory:
        model = Path(directory).resolve() / "parts.obj"
        model.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "g first\nf 1 2 3\ng second\nf 4 5 6\n",encoding="utf-8")
        args = ["--model",model,"--runs","1","--operations","auto,quality,distance"]
        whole = run(NATIVE,*args); split = run(NATIVE,*args,"--source-batches","2")
        assert whole["mesh"]["triangles"] == split["mesh"]["triangles"] == 2
        assert whole["mesh"]["source_batches"] == 1 and split["mesh"]["source_batches"] == 2
        assert whole["samples"][0]["result"]["detector_counts"][3][0] == 3
        assert split["samples"][0]["result"]["detector_counts"][3][0] == 0
        assert whole["samples"][1:] == split["samples"][1:]
        assert split["samples"] == run(PROFILE,*args,"--source-batches","2")["samples"]


def test_counter_rates_and_unavailable_events():
    from hardware import counter_metrics
    def metrics(**values):
        return counter_metrics({k:{"estimated_events":v} for k,v in values.items()})
    values = metrics(**{"076:00":1000,"0c0:00":2000,"0c2:00":400,"0c3:00":20})
    assert values["ipc_estimate"] == 2
    assert values["branch_mispredict_percent_estimate"] == 5
    assert values["branch_mpki_estimate"] == 10
    values = metrics(**{"076:00":1000,"0c0:00":2000,"064:f0":750,"064:08":250})
    assert values["l2_data_request_miss_percent_estimate"] == 25
    assert values["branch_mpki_estimate"] is None
    assert values["branch_mispredict_percent_estimate"] is None
    assert metrics()["ipc_estimate"] is None


def test_hardware_timeline_respects_long_capture_bucket_width():
    from hardware import timeline_resolution,timeline_bounds
    for width in (1,2,4):
        metadata = {"translation":[{"category":8192},{"category":1,"aggregateInterval":width}]}
        assert timeline_resolution(metadata) == width
        assert timeline_bounds({"start_ms":1000,"end_ms":3000},20,width) == (1020/width,3020/width)
    for metadata in ({},{"translation":[{"category":1,"aggregateInterval":0}]}):
        try:
            timeline_resolution(metadata)
        except ValueError:
            pass
        else:
            raise AssertionError("Unknown timeline resolution was accepted")


def test_power_alignment_across_midnight():
    from power import analyze
    with tempfile.TemporaryDirectory(prefix="woby-detector-power-") as directory:
        root = Path(directory).resolve(); (root / "timechart").mkdir()
        metadata = {"observer_start_utc":"2026-10-17T21:59:59+00:00",
            "timezone_offset_seconds":7200,"observer_start_qpc_ns":0}
        probe = {"clock_origin":{"steady_epoch_nanoseconds":0},
            "events":[{"name":"op.distance","start_ms":500,"end_ms":1500}]}
        for name,value in (("metadata",metadata),("probe",probe)):
            (root / (name+".json")).write_text(json.dumps(value),encoding="utf-8")
        (root / "timechart/power.csv").write_text(
            "Profile Start Time:,Oct-17-2026_23-59-59\nRecordId,Timestamp,Package Power\n"
            "0,23:59:59:600,20\n1,00:00:00:100,30\n2,00:00:00:400,-nan(ind)\n",encoding="utf-8")
        analyze(root)
        result = json.loads((root / "power.json").read_text())
        assert [v["time_ms"] for v in result["series"]] == [600,1100,1400]
        assert result["phases"]["op.distance"]["Package Power"]["mean"] == 25
        assert result["series"][-1]["values"]["Package Power"] is None
        assert result["invalid_samples_by_counter"]["Package Power"] == 1


def test_summary_preserves_partial_capture_and_scoped_memory():
    from summarize import summarize
    from evidence import bucket_quantile
    with tempfile.TemporaryDirectory(prefix="woby-detector-summary-") as directory:
        root = Path(directory).resolve(); case = root / "case"; case.mkdir()
        metadata = {"exit_code":2,"stop_reason":"timeout","profile":None,"observer_start_qpc_ns":0}
        probe = {"samples":[{"operation":"distance","run":0,"status":"ok","result":{}}],
            "events":[{"name":"op.distance","start_ms":1000,"end_ms":2000,"wall_ms":1000,"cpu_ms":2000,
                "average_logical_cpus":2,"commit_begin":0,"commit_end":1024,"process_peak_working_set":2048}],
            "options":{},"instrumented":True,"query_batch_stride":2,"distance_statistics_valid":False,
            "clock_origin":{"steady_epoch_nanoseconds":0},"mesh":{"triangles":128}}
        resources = {"samples":[{"elapsed_s":.5,"rss":8192,"private":9000},{"elapsed_s":1.5,"rss":1024,"private":1500}],
            "external_cpu":[{"interval_s":1,"busy":[{"name":"cl.exe","cpu_s":2}]}]}
        for name,value in (("metadata",metadata),("probe",probe),("resources",resources)):
            (case / (name+".json")).write_text(json.dumps(value),encoding="utf-8")
        row = summarize(root)["rows"][0]
        assert not row["capture_complete"] and not row["distance_statistics_valid"]
        assert row["median_ms"] == 1000 and row["scope_sampled_peak_rss_gib"] == 1024/1024**3
        assert row["compiler_overlap_observed"] and row["external_compiler_cpu_seconds_lower_bound"] == 2
        assert summarize(root)["cases"][0]["resource_interval_median_s"] == 1
        assert bucket_quantile([1,2,5],.5) == [2,3]
        assert bucket_quantile([], .95) is None
        failed = root / "failed-before-load"; failed.mkdir()
        (failed / "metadata.json").write_text(json.dumps(metadata),encoding="utf-8")
        assert summarize(root)["completed_captures_without_probe"][0]["name"] == failed.name


def test_rejection_and_filesystem():
    with tempfile.TemporaryDirectory(prefix="woby-detector-profile-") as directory:
        root = Path(directory).resolve()
        model, output = root / "collapsed.obj", root / "result.json"
        model.write_text("v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3\n", encoding="utf-8")
        subprocess.run([str(PROFILE), "--model", str(model), "--output", str(output),
                        "--operations", "difference", "--runs", "1"], check=True, capture_output=True, timeout=30)
        result = json.loads(output.read_text())
        assert result["samples"][0]["status"] == "error"
        assert "collapsed" in result["samples"][0]["error"]
        assert len([e for e in result["events"] if e["name"] == "op.difference"]) == 1
        invalid = subprocess.run([str(NATIVE), "--model", "relative.obj"], capture_output=True, text=True)
        assert invalid.returncode != 0 and "absolute" in invalid.stderr


def test_portable_evidence_retains_predicates_and_application_scope():
    from evidence import export
    import csv
    import hashlib
    import zipfile
    with tempfile.TemporaryDirectory(prefix="woby-detector-evidence-") as directory:
        base = Path(directory).resolve(); root = base / "captures"; case = root / "case"; case.mkdir(parents=True)
        data = {"classifier_calls":10,"fast_separated":8,"adaptive_false":2,"adaptive_true":0,"rational_fallback":0}
        def event(name,start,end,details):
            return {"name":name,"start_ms":start,"end_ms":end,"wall_ms":end-start,"cpu_ms":end-start,
                "average_logical_cpus":1,"working_set_end":1024,"commit_begin":0,"commit_end":1024,
                "process_peak_working_set":2048,"data":details}
        metadata = {"exit_code":0,"profile":None}
        probe = {"samples":[{"operation":"intersections","run":0,"status":"ok","result":{}}],
            "events":[event("mesh_intersections.inspectIntersections",20,80,data),event("op.intersections",0,100,{})],
            "options":{},"instrumented":True,"mesh":{"triangles":12}}
        resources = {"samples":[{"elapsed_s":.05,"rss":1024,"private":1500}],"external_cpu":[]}
        for name,value in (("metadata",metadata),("probe",probe),("resources",resources)):
            (case / (name+".json")).write_text(json.dumps(value),encoding="utf-8")
        application = root / "app"; application.mkdir()
        (application / "application.json").write_text(json.dumps({"analysis_mode":"whole","status":"complete",
            "records":[{"name":"cached_results_query","run":0,"wall_ms":15,"cpu_ms":0,"result":{"count":1}}]}),encoding="utf-8")
        validation = root / "validation"; validation.mkdir()
        (validation / "validation.json").write_text(json.dumps({"status":"passed"}),encoding="utf-8")
        (validation / "unit-tests-release-final.log").write_text("All tests passed",encoding="utf-8")
        destination = base / "evidence"; export(root,destination)
        rows = json.loads((destination / "predicates.json").read_text())
        assert rows[0]["operation"] == "op.intersections" and rows[0]["classifier_calls"] == 10
        app = json.loads((destination / "application.json").read_text())
        assert app[0]["capture_status"] == "complete" and app[0]["wall_ms"] == 15
        with (destination / "predicates.csv").open(newline="",encoding="utf-8") as stream:
            assert next(csv.DictReader(stream))["rational_fallback"] == "0"
        from evidence import csv_write
        structured = destination / "structured.csv"
        csv_write(structured,[{"bucket":[2**30,None],"shares":{"local":0.25,"missing":None}}])
        with structured.open(newline="",encoding="utf-8") as stream:
            row = next(csv.DictReader(stream))
            assert json.loads(row["bucket"]) == [2**30,None]
            assert json.loads(row["shares"]) == {"local":0.25,"missing":None}
        manifest = json.loads((destination / "evidence-manifest.json").read_text())
        assert manifest["archive_sha256"] == hashlib.sha256((destination / "raw-evidence.zip").read_bytes()).hexdigest()
        with zipfile.ZipFile(destination / "raw-evidence.zip") as archive:
            assert "case/probe.json" in archive.namelist() and "app/application.json" in archive.namelist()
            assert json.loads(archive.read("validation/validation.json"))["status"] == "passed"
            assert archive.read("validation/unit-tests-release-final.log") == b"All tests passed"


def test_control_selection_and_nonempty_equivalence():
    from dashboard import distance_control
    from verify_results import equal_results
    cases = [{"name":"first","capture_complete":True,"compiler_overlap_observed":False,
        "records":[{"operation":"distance","status":"ok"}],
        "events":[{"name":"op.distance","wall_ms":v,"cpu_ms":v*2} for v in (1,2,9)]},
        {"name":"second","capture_complete":True,"compiler_overlap_observed":False,
        "records":[{"operation":"distance","status":"ok"}],
        "events":[{"name":"op.distance","wall_ms":3,"cpu_ms":6}]}]
    combined = distance_control(cases,["first","second"])
    assert combined["runs"] == 4 and combined["median_ms"] == 2.5 and combined["median_cpu_ms"] == 5
    assert distance_control(cases,["missing"]) is None
    cases[1]["compiler_overlap_observed"] = True
    assert distance_control(cases,["first","second"]) is None
    cases[1]["compiler_overlap_observed"] = False; cases[1]["capture_complete"] = False
    assert distance_control(cases,["first","second"]) is None
    assert not equal_results([],{"count":1})
    assert equal_results([{"status":"ok","result":{"count":1}}],{"count":1})
    assert not equal_results([{"status":"error","result":{"count":1}}],{"count":1})


def test_generation_rejects_drift():
    import hashlib
    spec = importlib.util.spec_from_file_location("detector_generate", Path(__file__).with_name("generate.py"))
    generator = importlib.util.module_from_spec(spec); spec.loader.exec_module(generator)
    with tempfile.TemporaryDirectory(prefix="woby-detector-generation-") as directory:
        root = Path(directory).resolve(); (root / "src").mkdir()
        for stem in generator.FUNCTIONS:
            shutil.copy2(ROOT / "src" / (stem + ".cpp"), root / "src")
        generator.generate(root, root / "output")
        manifest = json.loads((root / "output/manifest.json").read_text())
        for stem, entries in manifest.items():
            assert entries["source_sha256"] == hashlib.sha256((root / "src" / (stem+".cpp")).read_bytes()).hexdigest()
            for key, suffix in (("generated_sha256",""),("counted_sha256","_counted"),("tokenref_sha256","_tokenref"),("max3_sha256","_max3")):
                if key in entries:
                    assert entries[key] == hashlib.sha256((root / "output" / (stem+suffix+".cpp")).read_bytes()).hexdigest()
        source = root / "src/mesh_comparison.cpp"
        source.write_text(source.read_text().replace("buildTree(", "renamedTree("), encoding="utf-8")
        try:
            generator.generate(root, root / "changed")
        except ValueError:
            pass
        else:
            raise AssertionError("Changed instrumentation anchor was accepted")


if __name__ == "__main__":
    suite = unittest.TestSuite(unittest.FunctionTestCase(f) for name, f in list(globals().items()) if name.startswith("test_"))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
