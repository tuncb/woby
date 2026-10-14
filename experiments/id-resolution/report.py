"""Write the ID resolution report from recorded measurements."""
import argparse
from collections import defaultdict
import html
import json
from pathlib import Path
import shutil
import statistics
import zipfile


def median(values):
    return statistics.median(values)


def seconds(value):
    return f"{value / 1000:.3f}"


def table(headings, rows):
    head = "".join(f"<th>{html.escape(str(value))}</th>" for value in headings)
    body = "".join("<tr>" + "".join(f"<td>{html.escape(str(value))}</td>" for value in row) + "</tr>" for row in rows)
    return f"<table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>"


def compare(rows, extract):
    before = [extract(row) for row in rows if row["configuration"] == "baseline"]
    after = [extract(row) for row in rows if row["configuration"] == "optimized"]
    a, b = median(before), median(after)
    return seconds(a), seconds(b), seconds(a - b), f"{(a - b) / a * 100:.1f}%"


def paired_gains(rows, extract):
    rounds = defaultdict(dict)
    for row in rows:
        rounds[row["round"]][row["configuration"]] = row
    return [extract(pair["baseline"]) - extract(pair["optimized"]) for pair in rounds.values()]


def make_report(measure, output, source):
    output.mkdir(parents=True, exist_ok=True)
    native = json.loads((measure / "native-final/summary.json").read_text())
    traced = json.loads((measure / "traced-final/summary.json").read_text())
    provenance = json.loads((measure / "provenance.json").read_text())
    validation = json.loads((measure / "validation.json").read_text())
    replay = [json.loads(line) for line in (measure / "final-replay.jsonl").read_text().splitlines()]
    exploratory = {path.stem: [json.loads(line) for line in path.read_text().splitlines()]
                   for path in sorted(measure.glob("sweep*.jsonl"))}
    regressions = {path.stem: [json.loads(line) for line in path.read_text().splitlines()]
                   for path in sorted(measure.glob("regression-*.jsonl"))}
    trials = [row for rows in [replay, *exploratory.values(), *regressions.values()] for row in rows if "variant" in row]
    if not all(row["exact"] for row in trials):
        raise ValueError("A replay did not match the original resolver")
    result = {"provenance": provenance, "validation": validation, "native": native, "traced": traced,
              "replay": replay, "exploratory": exploratory, "regressions": regressions,
              "exact_trials": len(trials), "variants": sorted({row["variant"] for row in trials})}
    (output / "summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    native_rows = [
        ["CPU import", *compare(native, lambda r: r["cpu_import_ms"])],
        ["First scene frame, CPU completion (bracket midpoint)", *compare(native, lambda r: median(r["native_scene_frame_cpu_complete_bounds_ms"]))],
        ["Full readiness, model.add RPC", *compare(native, lambda r: r["load_rpc_ms"])],
    ]
    phase = lambda name: lambda row: row["phase_times_ms"][name][0]
    trace_rows = [[label, *compare(traced, phase(name))] for name, label in (
        ("mapping_id_resolution", "ID resolution, including UV scan"),
        ("mapping_resolve_total", "Resolution, including table and index initialization"),
        ("mapping_primary_gather", "Primary attribute gather"),
        ("corner_mapping", "Complete corner mapping"))]
    trace_rows += [["First scene frame, GPU completion", *compare(traced, lambda r: r["milestones_ms"]["first_scene_frame_gpu_complete"])],
                   ["All tools ready", *compare(traced, lambda r: r["milestones_ms"]["all_tools_ready"])]]
    native_detail = [[r["round"], r["configuration"], seconds(r["cpu_import_ms"]),
                      "–".join(seconds(x) for x in r["native_scene_frame_cpu_complete_bounds_ms"]), seconds(r["load_rpc_ms"])] for r in native]
    paired = paired_gains(native, lambda r: r["load_rpc_ms"])
    replay_rows = []
    for name in ("original", "production"):
        values = [r["resolve_ms"] for r in replay if r.get("variant") == name]
        replay_rows.append([name, len(values), seconds(median(values)), f"{seconds(min(values))}–{seconds(max(values))}"])
    exploratory_tables = []
    for batch, records in exploratory.items():
        groups = defaultdict(list)
        for record in records:
            if "variant" in record:
                groups[record["variant"]].append(record)
        rows = [[name, len(records), seconds(median(r["resolve_ms"] for r in records)),
                 seconds(median(r["id_ms"] for r in records)) if "id_ms" in records[0] else "—",
                 seconds(median(r.get("uv_validation_ms", 0) for r in records))] for name, records in groups.items()]
        exploratory_tables.append(f"<details><summary>{batch}</summary>" + table(
            ["Variant", "Trials", "Resolve s", "Loop s", "UV scan s"], rows) + "</details>")
    regression_rows = []
    for name, records in regressions.items():
        baseline = median(r["resolve_ms"] for r in records if r.get("variant") == "original")
        optimized = median(r["resolve_ms"] for r in records if r.get("variant") == "production")
        regression_rows.append([name.removeprefix("regression-"), seconds(baseline), seconds(optimized),
                                f"{(baseline - optimized) / baseline * 100:.1f}%", "Exact"])
    readiness_gain = seconds(median(paired))
    id_before, id_after, id_saved, id_percent = compare(traced, phase("mapping_id_resolution"))
    body = f"""<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>BearTrap: serial ID resolution</title>
<style>body{{font:16px/1.55 system-ui,sans-serif;color:#172b40;background:#f5f7fa;margin:0}}main{{max-width:1120px;margin:auto;padding:40px 28px 80px}}h1{{font-size:38px;line-height:1.15}}h2{{margin-top:44px}}table{{border-collapse:collapse;width:100%;font-size:14px;margin:20px 0;background:white}}th,td{{padding:10px 12px;border-bottom:1px solid #dce3eb;text-align:right}}th:first-child,td:first-child{{text-align:left}}th{{background:#eaf0f6}}code{{font:14px ui-monospace,monospace}}a{{color:#075da8}}.lead{{font-size:21px;max-width:900px}}.note{{color:#4c6074}}details{{margin:16px 0}}summary{{cursor:pointer;font-weight:600}}img{{max-width:100%;border:1px solid #dce3eb}}pre{{padding:20px;background:#eaf0f6;white-space:pre-wrap;overflow-wrap:anywhere}}</style>
<main><p class="note">14 October 2026 · Windows · Release · Ryzen 7 5800H</p>
<h1>BearTrap: faster serial ID resolution</h1>
<p class="lead">Serial ID resolution falls from <strong>{id_before} to {id_after} seconds</strong> in matched application traces. This saves {id_saved} seconds, or {id_percent}.</p>
<p>First display and full readiness each advance by about three seconds in those traces. Native production controls show a median paired readiness gain of {readiness_gain} seconds. Global IDs in order of first use, triangle order, group UV flags, and the 12-byte primary table are retained.</p>
<p>Input: 7,108,991,827 bytes; 227,315,958 triangle corners; 37,890,645 positions; 38,414,391 render vertices. The original measurement is in the <a href="../e2e-beartrap-20261014/report.html">end-to-end report</a>.</p>
<h2>Application results</h2><p>These medians use fresh production processes. Each input is read before launch. Baseline and optimized runs alternate. No build, test suite, or profiler runs at the same time.</p>
{table(["Endpoint", "Baseline median s", "Optimized median s", "Difference s", "Reduction"], native_rows)}
<p>The paired full-readiness gains are {', '.join(seconds(x) + ' s' for x in paired)}. Their median is {readiness_gain} s. The difference between configuration medians is a separate statistic. All three pairs improve.</p>
<p class="note">The native first-frame result is bracketed by the performance.begin RPC. Its midpoint is an estimate of CPU frame completion. The instrumented group below has a separate GPU-completion marker. These are separate capture groups; their absolute times must not be combined.</p>
{table(["Round", "Configuration", "CPU import s", "First-frame CPU bracket s", "Full readiness s"], native_detail)}
<h2>Measured ID pass and display markers</h2>
<p>The same instrumented viewer generator is used for both revisions. The ID interval includes the finite-UV scan. Parent intervals include their children.</p>
{table(["Phase or marker", "Baseline s", "Optimized s", "Saved s", "Reduction"], trace_rows)}
<h2>What changed</h2>
<p>The primary table layout stays at 12 bytes per source position, or 433.62 MiB for BearTrap. The resolver reads the source-position count once. It accumulates the invalid-UV bit with an integer OR and tests it once per group.</p>
<p>The caller selects eager or deferred source-index writes and the finite or fallback UV path once. The compiler removes those fixed choices from each corner iteration. This reduces branch and register work in the application loop.</p>
<p>A sequential UV scan checks the float values used by the renderer. If every value is finite, primary ID creation no longer reads UV coordinates through each first-use key. The scan uses no large buffer and polls cancellation on the caller. A scan is skipped if there are more UV records than corners.</p>
<p>If the scan does not pass, the resolver prefetches UV data 32 corners ahead. Lookahead stays within the current shape. Missing UVs and out-of-range prefetch addresses are skipped. Normal and UV seam keys still use the existing secondary table.</p>
<h2>Experiments and decisions</h2>
<p>Table-only prefetch did not improve BearTrap. Padding to 16 bytes, packed attribute pairs, reordered fields, and split arrays gave small gains or regressions. The 8-byte UV-only prototype saved memory but added a layout variant for a small further time gain. The selected design keeps the existing table and gather code.</p>
<p>Prefetching UV data produced the first large gain. A finite-UV scan removed more dependent reads. Continuing to prefetch after a successful scan made the resolver slower, so the final resolver disables that work. A parallel resolver was not required for this result; ordered ID assignment remains serial.</p>
<p>{len(trials)} replay trials, across {len(result['variants'])} named variants, matched every output ID, primary key, seam key, seam ID, and group flag against the frozen original loop. Validation is outside the timed intervals.</p>
{table(["Final replay", "Trials", "Resolve median s", "Range s"], replay_rows)}
<p class="note">Replay includes primary-table and index allocation. It keeps the original output alive for exact comparison. Its cache and memory conditions differ from a fresh viewer. Exploratory batches are controls for individual choices, not application speedup claims. In sweep4, “production” is the intermediate prefetch-only implementation.</p>
{''.join(exploratory_tables)}
<h2>Other inputs and correctness</h2>
{table(["Resolver input", "Original s", "Optimized s", "Reduction", "Output"], regression_rows)}
<p>These trials call the staged resolver directly and bypass application path selection. In replay records, “production” names the current resolver implementation.</p>
<p>Bennu has no normal or UV records. The application therefore uses its existing loader fallback for Bennu. That fallback and its selection are unchanged. The 14.1% resolver reduction above does not establish a Bennu application load-time reduction.</p>
<pre>{html.escape(json.dumps(validation, indent=2))}</pre>
<p>Tests check order at shape, lookahead, and progress boundaries. They cover eager and deferred source indices and float and double attributes. UV cases include missing values, NaNs, infinities, and double-to-float overflow. Other cases check invalid positions, skipped scans, and cancellation on the caller.</p>
<h2>Evidence and reproduction</h2>
<p><a href="summary.json">Numeric summary</a> · <a href="measurements.zip">Raw records, logs, commands, assembly excerpts, and source snapshots</a> · <a href="REPRODUCE.txt">Reproduction commands</a></p>
<p>Source and executable hashes are in the summary. The baseline application is the frozen binary from the original report. Full input data, matching executable files, and PDBs remain in the local build directories.</p>
<p class="note">The timings apply to this input and computer. No hardware counters were collected for the optimized code. The prefetch and finite-scan controls support the inference that UV-read latency was a major cost. First display and full readiness are measured separately.</p>
</main></html>"""
    (output / "report.html").write_text(body, encoding="utf-8")
    shutil.copyfile(measure / "REPRODUCE.txt", output / "REPRODUCE.txt")
    with zipfile.ZipFile(output / "measurements.zip", "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in measure.rglob("*"):
            relative = path.relative_to(measure)
            if "final-bin" not in relative.parts and path.is_file() and path.suffix.lower() in (".json", ".jsonl", ".log", ".txt", ".py", ".png", ".h", ".cpp"):
                archive.write(path, "measurements/" + path.relative_to(measure).as_posix())
        for name in ("src/obj_corner_mapping.h", "tests/obj_prototype_tests.cpp", "CMakeLists.txt"):
            archive.write(source / name, "source/" + name)
        for path in (source / "experiments/id-resolution").glob("*"):
            if path.is_file():
                archive.write(path, "source/experiments/id-resolution/" + path.name)
        for path in (source / "experiments/obj-profile").glob("*"):
            if path.is_file() and path.suffix in (".py", ".cpp", ".h", ".txt"):
                archive.write(path, "source/experiments/obj-profile/" + path.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("measurements", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    make_report(args.measurements.resolve(strict=True), args.output.resolve(), Path(__file__).resolve().parents[2])
