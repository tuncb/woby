import unittest
import json
from pathlib import Path
import tempfile
from analyze import distribution, view_times, eligible_completions, summarize


class AnalysisTests(unittest.TestCase):
    def test_uses_each_views_source_frame_and_deduplicates(self):
        events = [dict(view_60=.1, view_frame_60=10, frame=40), dict(view_60=.1, view_frame_60=10, frame=41),
                  dict(view_60=900, view_frame_60=9, frame=42), dict(view_60=.3, view_frame_60=11, frame=43)]
        self.assertEqual(view_times(events,60,10,11),dict(n=2,median=.2,p95=.29))

    def test_excludes_tail_and_other_scenarios(self):
        entries=[dict(scenario=s,measured=m,frame=f) for s,m,f in [(2,True,80),(2,True,95),(1,True,80),(2,False,80)]]
        self.assertEqual(eligible_completions(entries,2,100),entries[:1])

    def test_empty_is_not_zero_and_zero_is_valid(self):
        self.assertIsNone(distribution([])["median"])
        self.assertEqual(distribution([0])["median"],0)

    def test_relocated_run_uses_its_own_capture_files(self):
        with tempfile.TemporaryDirectory(prefix="marker analysis ") as directory:
            root=Path(directory).resolve()
            actual=root/"relocated"
            actual.mkdir()
            marker=actual/"tiny.marker.json"
            marker.write_text(json.dumps(dict(events=[],completions=[],metadata={})))
            (actual/"tiny.csv").write_text("scenario,measured,frame\n"+"".join(f"0,1,{i}\n" for i in range(10,40)))
            manifest=actual/"runs.jsonl"
            manifest.write_text(json.dumps(dict(marker_path=str(root/"old"/"tiny.marker.json"),scenarios=[dict(name="fit")]))+"\n")
            result=summarize([manifest])
            self.assertEqual(result[0]["marker_path"],str(marker))
            self.assertEqual(result[0]["scenarios"][0]["readback_ms"]["n"],0)


if __name__=="__main__":
    unittest.main()
