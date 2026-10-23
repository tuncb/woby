"""Validate aggregation of independent point-render rounds."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location("point_navigation_summary",
    Path(__file__).resolve().parents[1]/"experiments/point-navigation/summarize.py")
summary=importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


def campaign():
    rows=[]
    for index,value in enumerate((2,10,30)):
        metric=dict(median=value,p95=value+5)
        phase={key:metric.copy() for key in ("raster_frame_gpu_ms","point_raster_ms","gpu_ms","main_thread_work_ms")}
        events=[dict(camera=[camera],points=dict(submittedCount=count)) for camera,count in ((0,100),(0,1000),(1,200),(2,0))]
        rows.append(dict(round=index,size=4,condition="cloud-only",navigation=dict(events=events),
            summary={key:copy.deepcopy(phase) for key in ("full","navigation","cached")}))
    return dict(status="completed",guard=dict(violations=[]),rounds=rows)


class ReportingTests(unittest.TestCase):
    def test_rounds_have_equal_weight_and_preserve_variation(self):
        row=summary.aggregate(campaign())[0]
        self.assertEqual(row["rounds"],3)
        self.assertEqual(row["full_gpu_ms"],dict(median=10,minimum=2,maximum=30,worst_p95=35,round_medians=[2,10,30]))
        self.assertEqual(row["navigation_submitted_points"],200)

    def test_failed_mixed_duplicate_and_nonfinite_evidence_is_rejected(self):
        for kind in ("failed","guard","mixed","duplicate","nonfinite"):
            data=campaign()
            if kind=="failed": data["status"]="failed"
            elif kind=="guard": data["guard"]["violations"]=["overlap"]
            elif kind=="mixed": data["rounds"][1]["condition"]="transparent-surfaces"
            elif kind=="duplicate": data["rounds"][1]["round"]=0
            else: data["rounds"][0]["summary"]["full"]["raster_frame_gpu_ms"]["median"]=float("nan")
            with self.subTest(kind=kind),self.assertRaises(AssertionError):
                summary.aggregate(data)


if __name__=="__main__":
    unittest.main()
