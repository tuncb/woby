"""Check result equivalence across completed experimental controls."""
import argparse
import hashlib
import json
from pathlib import Path


def equal_results(samples, expected):
    return bool(samples) and all(s["status"] == "ok" and s["result"] == expected for s in samples)


def verify(root):
    checks = []
    for model in ("bus","powerplant","bennu"):
        original = json.loads((root / (model+"-token-live/probe.json")).read_text())
        reference = json.loads((root / (model+"-token-reference/probe.json")).read_text())
        expected = original["samples"][0]["result"]
        samples = [*original["samples"],*reference["samples"]]
        if model == "bus":
            for name in ("bus-token-empty","bus-token-reference-empty","bus-token-reference-branch","bus-token-empty-branch"):
                path = root / name / "probe.json"
                if path.exists():
                    samples.extend(json.loads(path.read_text())["samples"])
            for name in ("bus-expanded","bus-shuffled","bus-shuffled-data","bus-shuffled-l2","bus-qos-default","bus-qos-high"):
                path = root / name / "probe.json"
                if path.exists():
                    samples.extend(s for s in json.loads(path.read_text())["samples"] if s["operation"] == "quality")
        equal = equal_results(samples,expected)
        checks.append({"model":model,"operation":"quality","summaries_checked":len(samples),"equal":equal,
            "summary_sha256":hashlib.sha256(json.dumps(expected,sort_keys=True).encode()).hexdigest()})
    baseline = json.loads((root / "bus-workers-0/probe.json").read_text())
    expected = next(s["result"] for s in baseline["samples"] if s["operation"] == "distance")
    samples = []
    for name in ("bus-workers-0","bus-workers-1","bus-workers-3","bus-workers-7","bus-workers-8",
                 "bus-physical-8","bus-smt-4cores","bus-native-final","bus-qos-default","bus-qos-high"):
        path = root / name / "probe.json"
        if path.exists():
            samples.extend(s for s in json.loads(path.read_text())["samples"] if s["operation"] == "distance")
    checks.append({"model":"bus","operation":"distance","control":"workers, affinity, QoS, and final native repeat",
        "summaries_checked":len(samples),"equal":equal_results(samples,expected),
        "summary_sha256":hashlib.sha256(json.dumps(expected,sort_keys=True).encode()).hexdigest()})
    for model,baseline_name in (("bus","bus-max3-control"),("powerplant","powerplant-timechart"),("bennu","bennu-timechart")):
        directories = sorted(root.glob(model+"-max3*"))
        if not directories:
            continue
        baseline = json.loads((root / baseline_name / "probe.json").read_text())
        expected = next(s["result"] for s in baseline["samples"] if s["operation"] == "distance")
        samples = []; captures = []; excluded = []
        for directory in directories:
            metadata_path = directory / "metadata.json"; probe_path = directory / "probe.json"
            metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() else {}
            if metadata.get("exit_code") != 0 or metadata.get("stop_reason") or not probe_path.exists():
                excluded.append(directory.name)
                continue
            current = [s for s in json.loads(probe_path.read_text())["samples"] if s["operation"] == "distance"]
            if not current:
                raise ValueError("Completed maximum-expression capture has no distance result: "+directory.name)
            captures.append(directory.name); samples.extend(current)
        checks.append({"model":model,"operation":"distance","control":"three-value maximum expression",
            "captures_checked":captures,"incomplete_captures_excluded":excluded,
            "summaries_checked":len(samples),"equal":equal_results(samples,expected),
            "summary_sha256":hashlib.sha256(json.dumps(expected,sort_keys=True).encode()).hexdigest()})
    for model in ("bus","powerplant","sanmiguel","bennu"):
        path = root / (model+"-isolated-detectors/probe.json")
        if not path.exists():
            continue
        baseline = json.loads((root / (model+"-native/probe.json")).read_text())
        expected = next(s["result"]["detector_counts"] for s in baseline["samples"] if s["operation"] == "auto")
        categories = {"duplicate-points":[3],"duplicate-triangles":[4],"duplicates":[3,4],"degenerates":[5],"topology":[0,1,2,6,7,8]}
        for operation,indices in categories.items():
            samples = [s for s in json.loads(path.read_text())["samples"] if s["operation"] == operation]
            checks.append({"model":model,"operation":operation,"control":"isolated versus automatic detector counts",
                "summaries_checked":len(samples),"equal":bool(samples) and all(s["status"] == "ok" and all(
                    s["result"]["detector_counts"][i] == expected[i] for i in indices) for s in samples)})
    for model in ("powerplant","sanmiguel"):
        path = root / (model+"-all-batches-degenerates/probe.json")
        if path.exists():
            expected = next(s["result"]["detector_counts"][5] for s in json.loads(
                (root / (model+"-native/probe.json")).read_text())["samples"] if s["operation"] == "auto")
            samples = json.loads(path.read_text())["samples"]
            checks.append({"model":model,"operation":"degenerates","control":"all imported parts as separate source batches",
                "summaries_checked":len(samples),"equal":bool(samples) and all(s["status"] == "ok" and s["result"]["detector_counts"][5] == expected for s in samples)})
    detector_keys = ("boundary_edges","non_manifold_edges","inconsistently_oriented_tris","duplicate_points",
        "duplicate_tris","degenerate_tris","non_manifold_vertices","holes","fins")
    for name,baseline in (("bus-application-whole","bus-native"),("bus-application-per-volume","bus-batches-65")):
        path = root / name / "application.json"
        if path.exists() and (root / baseline / "probe.json").exists():
            data = json.loads(path.read_text())
            expected = next(s["result"]["detector_counts"] for s in json.loads((root / baseline / "probe.json").read_text())["samples"] if s["operation"] == "auto")
            samples = [s for s in data["records"] if s["name"] == "automatic_ready"]
            checks.append({"model":"bus","operation":"automatic","control":name+" versus CPU source batches",
                "summaries_checked":len(samples),"equal":data.get("status") == "complete" and bool(samples) and all(
                    s["detectors"][key]["metadata"].get("count") == expected[i][0] for s in samples for i,key in enumerate(detector_keys))})
    result = {"scope":"Exact equality of recorded result summaries for expression, token, worker and QoS controls; selected detector counts for isolated-operation controls. This is not a dump of every per-face metric.","checks":checks}
    (root / "equivalence.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps(result))
    return all(c["equal"] for c in checks)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("root",type=Path)
    raise SystemExit(not verify(parser.parse_args().root.resolve()))
