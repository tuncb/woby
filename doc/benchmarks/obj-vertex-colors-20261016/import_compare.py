# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=7"]
# ///
import json
from pathlib import Path
import statistics
import subprocess
import sys

root = Path(r"C:/Users/tuncb/.t3/worktrees/woby/t3code-9921b770")
sys.path.insert(0, str(root / "experiments/parser-throughput"))
from color_compare import quiet, sha

destination = Path(r"D:/.worktree/woby-vertex-colors-results-9921")
source = Path(r"D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj")
programs = {
    "before_ignored_rgb": destination / "before-import.exe",
    "after_rgb": Path(r"D:/.worktree/woby-parser-build-9921/bin/Release/woby_obj_prototype_benchmark.exe"),
}
power = subprocess.check_output(["powercfg", "/getactivescheme"], text=True).strip()
report = {"method": "Fresh processes, warm input before each run, alternating order, two idle CPU samples <=15%, deferred preparation; fingerprints outside timing.",
          "rounds": 3, "workers": 16, "power_scheme": power,
          "input": {"path": str(source), "bytes": source.stat().st_size, "sha256": sha(source)},
          "programs": {name: {"path": str(path), "sha256": sha(path)} for name, path in programs.items()},
          "samples": []}
fingerprints = {}
for round_index in range(3):
    order = list(programs)
    if round_index % 2:
        order.reverse()
    for name in order:
        with source.open("rb") as stream:
            while stream.read(16 * 1024 * 1024):
                pass
        idle = quiet()
        assert subprocess.check_output(["powercfg", "/getactivescheme"], text=True).strip() == power
        result = json.loads(subprocess.check_output([str(programs[name]), "prototype", str(source), "16", "deferred"], text=True))
        for key in ("fingerprint", "color_fingerprint", "vertex_color_bytes"):
            if key in result:
                expected = fingerprints.setdefault(key, result[key])
                assert result[key] == expected, (name, key)
        report["samples"].append({"round": round_index, "variant": name, "idle_cpu_percent": idle, **result})
        (destination / "import-comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"{round_index+1}/3 {name}: {result['cpu_load_ms']:.2f} ms", flush=True)
report["medians"] = {name: {key: statistics.median(row[key] for row in report["samples"] if row["variant"] == name)
    for key in ("cpu_load_ms", "parse_ms", "prepare_ms", "peak_commit_bytes", "retained_mesh_buffer_capacity_bytes")}
    for name in programs}
(destination / "import-comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
