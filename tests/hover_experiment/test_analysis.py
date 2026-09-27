"""Analysis contract tests. Temporary paths share one explicit fixture root."""
import json
from pathlib import Path
import tempfile
import unittest
from summarize import distribution, enrich


def test_distribution():
    assert distribution([]) is None
    assert distribution([4,2])["p50"]==3
    assert abs(distribution([4,2])["p95"]-3.9)<1e-12

def test_gpu_view_frames_are_deduplicated_and_transitions_excluded():
    with tempfile.TemporaryDirectory(prefix="woby-hover-analysis-") as directory:
        path=Path(directory).resolve()/"capture.hover.json"
        events=[dict(scenario=0,measured=True,frame=i,picker="gpu_cluster",cpu_ms=.1,points=0,nodes=0) for i in range(10,50)]
        for _ in range(2):
            for i in range(0,80):
                row=dict(scenario=1,measured=False,picker="gpu_times",gpu_frame=500)
                for view in (60,61,62):
                    row["view_"+str(view)]=view-59
                    row["view_frame_"+str(view)]=i
                events.append(row)
        completions=[dict(scenario=0,measured=True,frame=i,latency_ms=4 if i<=41 else 999,latency_frames=3,rank=0) for i in range(10,50)]
        path.write_text(json.dumps(dict(metadata={},events=events,completions=completions)))
        run=dict(model="fixture",round=1,hover_path=str(path),metadata={},scenarios=[dict(name="fit_gpu_cluster",settings=dict(case="fit",picker="gpu_cluster"))])
        result=enrich(run)["scenarios"][0]
        assert result["picking_gpu_ms"]["n"]==24
        assert result["picking_gpu_ms"]["p50"]==6
        assert result["latency_ms"]["max"]==4
        assert result["latency_ms"]["n"]==32

def test_correctness_sequence_rejects_missing_result():
    with tempfile.TemporaryDirectory(prefix="woby-hover-analysis-") as directory:
        path=Path(directory).resolve()/"capture.hover.json"
        completions=[dict(scenario=0,measured=False,rank=i) for i in (1,2)]
        completions += [dict(scenario=1,measured=False,rank=1)]
        path.write_text(json.dumps(dict(metadata={},events=[],completions=completions)))
        run=dict(model="fixture",round=1,hover_path=str(path),metadata=dict(validate=True),scenarios=[
            dict(name="fit_gpu_cluster",settings=dict(case="fit",picker="gpu_cluster")),
            dict(name="fit_gpu_flat",settings=dict(case="fit",picker="gpu_flat"))])
        assert not enrich(run)["gpu_culling_validation"][0]["identical"]


if __name__=="__main__":
    cases=[unittest.FunctionTestCase(value) for name,value in list(globals().items()) if name.startswith("test_")]
    result=unittest.TextTestRunner().run(unittest.TestSuite(cases))
    raise SystemExit(0 if result.wasSuccessful() else 1)
