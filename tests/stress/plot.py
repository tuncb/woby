"""Render the October 2026 baseline figures from its summarized evidence."""

import argparse
import json
from pathlib import Path
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def find_case(cases, campaign, model):
    return next(c for c in cases if c["evidence"].startswith(campaign+"/") and model in Path(c["model"]).name)


def frame(case, name):
    return next(f["fps"] for f in case["frames"] if f["name"] == name)


def operation(case, name):
    return next(o["seconds"] for o in case["operations"] if o["name"] == name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    cases = json.loads(args.summary.read_text(encoding="utf-8"))["cases"]
    args.output.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.size": 11, "axes.spines.top": False, "axes.spines.right": False,
                         "axes.titleweight": "bold", "axes.labelcolor": "#334155", "text.color": "#172033"})
    colors = ["#2563eb", "#db6b20", "#138571", "#9b4dcc"]
    models = [("bennu", "Bennu"), ("powerplant", "Powerplant"), ("san-miguel", "San Miguel"), ("BearTrap", "Bear Trap")]
    fig, axes = plt.subplots(2, 2, figsize=(15, 10), layout="constrained")
    axis = axes[0, 0]
    modes = [("solid", "Solid"), ("edges", "Edges"), ("vertices", "Vertices 4 px"), ("combined", "Combined")]
    x = np.arange(len(modes))
    for (key, label), color in zip(models, colors):
        case = find_case(cases, "render-matrix", key)
        axis.plot(x, [frame(case, mode) for mode, _ in modes], "o-", color=color, label=label, linewidth=2)
    axis.set_xticks(x, [label for _, label in modes])
    axis.set_yscale("log")
    axis.set_yticks([5, 10, 30, 60, 120, 240], ["5", "10", "30", "60", "120", "240"])
    axis.set_ylabel("Completed frames / second (log scale)")
    axis.set_title("Large mesh rendering: overlays dominate")
    axis.grid(axis="y", alpha=.2)
    axis.legend(fontsize=9)

    axis = axes[0, 1]
    for select, label, color in [("10000000_xyz", "10 million points", colors[0]), ("100000000_xyz", "100 million points", colors[1])]:
        selected = [c for c in cases if c["evidence"].startswith("cloud-repeats/") and select in Path(c["model"]).name]
        names = ["vertices_1px", "points_default", "vertices_8px"]
        values = [[frame(c, name) for c in selected] for name in names]
        centers = [statistics.median(v) for v in values]
        errors = [[m-min(v) for m, v in zip(centers, values)], [max(v)-m for m, v in zip(centers, values)]]
        axis.errorbar([1, 4, 8], centers, yerr=errors, color=color, marker="o", capsize=5, linewidth=2, label=label)
    axis.set_yscale("log")
    axis.set_yticks([1, 2, 5, 10, 30, 60], ["1", "2", "5", "10", "30", "60"])
    axis.set_xticks([1, 4, 8])
    axis.set_xlabel("Point size (pixels)")
    axis.set_ylabel("Completed frames / second (log scale)")
    axis.set_title("Point clouds: median and range of two runs")
    axis.grid(axis="y", alpha=.2)
    axis.legend(fontsize=9)

    axis = axes[1, 0]
    labels, loads, analyses = [], [], []
    for key, label in [("BusGame", "Bus map"), *models[:3]]:
        load = find_case(cases, "load-all", key)
        mesh = find_case(cases, "mesh-pilot" if key == "BusGame" else "mesh-matrix", key)
        labels.append(label)
        loads.append(operation(load, "model_add"))
        analyses.append(operation(mesh, "mesh_initial_ready"))
    x = np.arange(len(labels))
    axis.bar(x-.18, loads, .36, label="Import", color="#8ab4f8")
    axis.bar(x+.18, analyses, .36, label="Initial mesh analysis", color="#2563eb")
    for i, value in enumerate(analyses):
        axis.text(i+.18, value+1.2, f"{value:.1f}", ha="center", fontsize=9)
    axis.set_xticks(x, labels)
    axis.set_ylabel("Command-to-ready seconds")
    axis.set_ylim(0, max(analyses)*1.17)
    axis.set_title("Analysis cost is separate from opening cost")
    axis.legend(fontsize=9)
    axis.grid(axis="y", alpha=.2)

    axis = axes[1, 1]
    x = np.arange(len(models))
    load_memory, mesh_memory = [], []
    for key, _ in models:
        load_memory.append(find_case(cases, "load-all", key)["peak_private_bytes"]/2**30)
        mesh_memory.append(find_case(cases, "mesh-matrix", key)["peak_private_bytes"]/2**30)
    axis.bar(x-.18, load_memory, .36, label="Load case", color="#8ab4f8")
    bars = axis.bar(x+.18, mesh_memory, .36, label="Mesh suite", color="#2563eb")
    bars[-1].set_hatch("///")
    bars[-1].set_color("#c26730")
    axis.text(x[-1]+.18, mesh_memory[-1]+.8, "Guard stopped\nanalysis", ha="center", fontsize=9)
    axis.axhline(38, color="#c26730", linestyle="--", linewidth=1, label="Sampled stop threshold")
    axis.set_xticks(x, [name for _, name in models])
    axis.set_ylabel("Whole-case peak private memory (GiB)")
    axis.set_ylim(0, 46)
    axis.set_title("Analysis substantially amplifies memory")
    axis.legend(fontsize=9, loc="upper left")
    axis.grid(axis="y", alpha=.2)
    fig.suptitle("Woby 0.26.0 · large-file baseline · 3–4 October 2026", fontsize=19, fontweight="bold")
    fig.supxlabel("Ryzen 7 5800H · RTX 3070 Laptop 8 GiB · 63.9 GiB RAM · Release Vulkan · ~240 FPS pacing ceiling\nDesktop dimensions were not locked; see the report for viewport, camera and outcome qualifications.", fontsize=9)
    fig.savefig(args.output / "large-file-overview.png", dpi=170)
    fig.savefig(args.output / "large-file-overview.svg")
    plt.close(fig)


if __name__ == "__main__":
    main()
