"""Align uProf frequency/power samples with operation scopes."""
import argparse
from collections import defaultdict
import csv
from datetime import datetime, timedelta, timezone
import json
import math
from pathlib import Path
import statistics


def distribution(values):
    return {"samples":len(values),"mean":statistics.mean(values),"minimum":min(values),"maximum":max(values)} if values else None


def analyze(root):
    metadata = json.loads((root / "metadata.json").read_text())
    probe = json.loads((root / "probe.json").read_text())
    csv_path = next((root / "timechart").rglob("*.csv"))
    lines = csv_path.read_text(encoding="utf-8-sig").splitlines()
    start = next(i for i,line in enumerate(lines) if line.startswith("RecordId,Timestamp,"))
    date = datetime.strptime(next(line.split(",")[1] for line in lines if line.startswith("Profile Start Time:")),"%b-%d-%Y_%H-%M-%S").date()
    epoch = datetime.fromisoformat(metadata["observer_start_utc"])
    zone = timezone(timedelta(seconds=metadata["timezone_offset_seconds"]))
    offset_ms = (probe["clock_origin"]["steady_epoch_nanoseconds"]-metadata["observer_start_qpc_ns"])/1e6
    values_by_phase = defaultdict(lambda:defaultdict(list)); series = []; previous = None
    invalid = defaultdict(int)
    for row in csv.DictReader(lines[start:]):
        if not row.get("Timestamp"):
            continue
        hour,minute,second,millisecond = map(int,row["Timestamp"].split(":"))
        absolute = datetime(date.year,date.month,date.day,hour,minute,second,millisecond*1000,tzinfo=zone)
        if previous is not None and absolute < previous:
            date += timedelta(days=1)
            absolute += timedelta(days=1)
        previous = absolute
        elapsed_ms = (absolute-epoch).total_seconds()*1000-offset_ms
        values = {}
        for key,value in row.items():
            if not key or key in ("RecordId","Timestamp") or not value or not value.strip():
                continue
            number = float("nan") if "nan" in value.lower() else float(value)
            values[key] = number if math.isfinite(number) else None
            if values[key] is None:
                invalid[key] += 1
        series.append({"time_ms":elapsed_ms,"values":values})
        for phase in probe["events"]:
            if phase["name"].startswith("op.") or phase["name"] in ("distance.queries","distance.statistics","quality.statistics"):
                if phase["start_ms"] <= elapsed_ms <= phase["end_ms"]:
                    for key,value in values.items():
                        if value is not None:
                            values_by_phase[phase["name"]][key].append(value)
    result = {"alignment":"Timechart local timestamps mapped to paired observer UTC/QPC, then probe QPC; 100 ms sampling is coarse.",
        "scope":"System-wide package power and per-core effective frequency. Other processes and idle time affect these values.",
        "phases":{name:{key:distribution(values) for key,values in columns.items()} for name,columns in values_by_phase.items()},
        "invalid_samples_by_counter":dict(invalid),"series":series}
    (root / "power.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps({"samples":len(series),"phases":list(values_by_phase)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("capture",type=Path)
    analyze(parser.parse_args().capture.resolve())
