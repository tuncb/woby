"""Export a static figure from the aggregate JSON (requires matplotlib)."""
import argparse
import json
from pathlib import Path
import numpy as np
import matplotlib.pyplot as plt


def main():
    p = argparse.ArgumentParser()
    p.add_argument("summary", type=Path)
    p.add_argument("output", type=Path)
    args = p.parse_args()
    data = json.loads(args.summary.read_text())
    models = [
        ("20190810_BearTrap_Ground_Model.obj", "BearTrap · 75.8M triangles"),
        ("bennu_OLA_v21_PTM_very-high.obj", "Bennu · 17.9M"),
        ("powerplant.obj", "Powerplant · 12.8M"),
        ("san-miguel.obj", "San Miguel · 10.0M"),
        ("uploads_files_2720101_BusGameMap.obj", "BusGameMap · 1.05M"),
    ]
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
    fig, axes = plt.subplots(1, 2, figsize=(14.2, 6.7), sharey=True)
    fig.patch.set_facecolor("#f7f8fa")
    settings = [
        ("Rendering modes", [("solid_fit", "Opaque solid", "#246c9b"),
                              ("solid_edges", "Solid + triangle edges", "#e1a33d"),
                              ("solid_points", "Solid + 4 px markers", "#7156a5")]),
        ("Camera zoom with 4 px markers", [("points4", "Stationary; pointer outside", "#246c9b"),
                                              ("points_zoom_no_hover", "Zoom; pointer outside", "#459e8b"),
                                              ("points_zoom", "Zoom; pointer over scene", "#c74d51")]),
    ]
    centers = np.arange(len(models))
    for axis, (title, choices) in zip(axes, settings):
        axis.set_facecolor("#f7f8fa")
        for offset, (key, label, color) in enumerate(choices):
            values = [data[name]["scenarios"][key]["fps"] for name, _ in models]
            y = centers + (offset-1)*.23
            axis.barh(y, values, height=.19, label=label, color=color, zorder=3)
            for yy, value in zip(y, values):
                display = f"{value:.2f}" if value < 1 else f"{value:.1f}" if value < 100 else f"{value:,.0f}"
                axis.text(value*1.12, yy, display,
                          va="center", fontsize=8.5, color="#273544")
        axis.set_xscale("log")
        axis.set_xlim(.5, 5500)
        axis.set_xticks([1, 10, 100, 1000], ["1", "10", "100", "1,000"])
        axis.set_xlabel("Frames per second · logarithmic scale", labelpad=12)
        axis.set_title(title, loc="left", fontweight="bold", pad=19, fontsize=13)
        axis.grid(axis="x", alpha=.18, which="major", zorder=0)
        axis.tick_params(axis="both", length=0)
        axis.set_yticks(centers, [label for _, label in models])
        for spine in axis.spines.values():
            spine.set_visible(False)
        axis.legend(loc="upper left", bbox_to_anchor=(-.01, -.14), frameon=False, fontsize=9)
    axes[0].invert_yaxis()
    fig.suptitle("Large models: markers and CPU hover picking dominate the slow views", x=.04, y=.99,
                 ha="left", fontsize=18, fontweight="bold", color="#192d40")
    fig.text(.04, .94, "Release · RTX 3070 Laptop / Ryzen 7 5800H · 1280×720 · 4× MSAA · VSync off", color="#526273")
    fig.text(.04, .025, "Hidden-window throughput, not monitor scanout. Core: two runs per model; zoom without hover: one focused control. Full timings accompany the report.",
             color="#526273", fontsize=9)
    fig.subplots_adjust(left=.20, right=.98, top=.85, bottom=.25, wspace=.15)
    fig.savefig(args.output, dpi=150, facecolor=fig.get_facecolor())


if __name__ == "__main__":
    main()
