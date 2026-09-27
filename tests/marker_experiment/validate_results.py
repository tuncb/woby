"""Verify coverage of the separate large-model GPU audit and save its evidence."""
import argparse
import json
from pathlib import Path


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("runs",type=Path)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    results=[]
    for line in args.runs.read_text().splitlines():
        run=json.loads(line)
        data=json.loads(Path(run["marker_path"]).read_text())
        assert data["metadata"]["compact_ids"]
        assert data["metadata"]["spatial_index_build_ms"]==0
        assert data["metadata"]["spatial_index_bytes"]==0
        assert data["metadata"]["skipped_requests"]==0
        old=Path(run["marker_path"].removesuffix(".marker.json")+".hover.json")
        assert "build_ms" not in json.loads(old.read_text())["metadata"], "Old BVH was built"
        cases=[]
        for i,s in enumerate(run["scenarios"]):
            completed=[c for c in data["completions"] if c["scenario"]==i]
            assert len(completed)==10,(run["model"],s["name"],len(completed))
            assert all(c.get("validated") for c in completed)
            hits=sum(bool(c["id"]) for c in completed)
            if s["settings"]["case"] in ("hidden","zero"):
                assert hits==0
            cases.append(dict(name=s["name"],requests=len(completed),hits=hits,
                              max_winner_id=max(c["id"] for c in completed),
                              max_sample_id=max(int(p[0])+(int(p[1])<<16) for c in completed
                                                for j,p in enumerate(c["samples"])
                                                if s["settings"].get("msaa",True) or j%4==0)))
        assert sum(c["hits"] for c in cases)>0
        results.append(dict(model=run["model"],metadata=data["metadata"],cases=cases,raw=run["marker_path"]))
    assert len(results)==5
    assert max(c["max_sample_id"] for r in results for c in r["cases"])>2**24
    root=Path(__file__).resolve().parents[2]
    fixtures=json.loads((root/"build/marker-fixture-final/validation.json").read_text())
    colors=json.loads((root/"build/marker-color-compact/validation.json").read_text())
    output=dict(models=results,model_requests=sum(c["requests"] for r in results for c in r["cases"]),
                fixtures=fixtures,fixture_requests=sum(c["requests"] for c in fixtures),color_comparisons=colors)
    args.output.write_text(json.dumps(output,indent=2))
    print(f"PASS: {output['model_requests']} real-model requests and {output['fixture_requests']} fixture requests; IDs exceed 24-bit float integer precision")


if __name__=="__main__":
    main()
