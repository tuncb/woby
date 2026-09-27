"""Fail closed if clustered GPU picking changes the unculled GPU result."""
import argparse
import json
from pathlib import Path
from summarize import enrich


def main():
    p=argparse.ArgumentParser()
    p.add_argument("runs",type=Path)
    p.add_argument("output",type=Path)
    args=p.parse_args()
    enriched=[]
    for line in args.runs.read_text().splitlines():
        run=json.loads(line)
        if not run["metadata"].get("validate"):
            raise ValueError("Expected a validation run")
        result=enrich(run)
        if not result["gpu_culling_validation"] or not all(row["identical"] and row["queries"]>=10 for row in result["gpu_culling_validation"]):
            raise ValueError(f"GPU culling changed a result: {result['model']} {result['gpu_culling_validation']}")
        if result["metadata"]["skipped_requests"]:
            raise ValueError("Validation skipped requests")
        print(result["model"],"GPU culling agrees for",sum(row["queries"] for row in result["gpu_culling_validation"]),"queries",flush=True)
        enriched.append(result)
    args.output.write_text(json.dumps(enriched,indent=2),encoding="utf-8")


if __name__=="__main__":
    main()
