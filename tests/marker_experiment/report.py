"""Produce tables from the recorded experiment; narrative conclusions reviewed separately."""
import json
from pathlib import Path
import statistics

ROOT=Path(__file__).resolve().parents[2]
NAMES={"20190810_BearTrap_Ground_Model.obj":"BearTrap","bennu_OLA_v21_PTM_very-high.obj":"Bennu",
       "powerplant.obj":"Powerplant","san-miguel.obj":"San Miguel","uploads_files_2720101_BusGameMap.obj":"Bus"}


def main():
    runs=json.loads((ROOT/"doc/marker-picking-results.json").read_text())
    def rows(model,case,mode):
        return [s for r in runs if r["model"]==model for s in r["scenarios"]
                if s["settings"].get("case")==case and s["settings"].get("picker")==mode]
    def median(model,case,mode,get):
        values=[get(r) for r in rows(model,case,mode)]
        values=[v for v in values if v is not None]
        return statistics.median(values) if values else None
    def fmt(value,digits=2):
        return f"{value:.{digits}f}" if value is not None else "—"
    cluster=json.loads((ROOT/"doc/marker-cluster-reference.json").read_text())
    lines=["# Visible-marker GPU hover experiment", "", "Recorded 2026-09-27. RTX 3070 Laptop (8 GiB), NVIDIA 546.30, Ryzen 7 5800H, 64 GiB RAM. NVIDIA D3D11; pinned bgfx 1.129.8940-496 port 1.", "",
           "The prototype writes marker identities in the existing marker draw, looks up the visible samples around the cursor, and draws the highlight in the same GPU frame. Coordinate text updates asynchronously from completed readbacks. It builds no additional spatial index.", "",
           "![FPS, camera motion, GPU passes, and coordinate age](marker-picking-experiment.png)", "",
           "## Fitted-view throughput", "", "Opaque 4 px markers, 1280×720 window, 4× MSAA. Medians of two independently ordered Release runs. GPU highlight includes the offscreen color route, ID attachment, cursor lookup, and highlight. Async adds completed coordinate text and delayed readback.", "",
           "| Model | No picking FPS | GPU highlight FPS | Highlight + async text FPS | FPS retained with async text |", "|---|---:|---:|---:|---:|"]
    for m,n in NAMES.items():
        fps=[median(m,"fit",mode,lambda r:r["fps"]) for mode in ("none","id_resident","id_async")]
        lines.append(f"| {n} | {' | '.join(fmt(v,1) for v in fps)} | {fps[2]/fps[0]*100:.1f}% |")
    lines += ["", "## Cost breakdown", "", "These are medians of each run's per-view GPU medians. Scene-only numbers cover view 1. The routed control adds offscreen rendering and color resolve/composite; the ID-buffer control also writes IDs, without lookup or highlight. GPU times overlap with CPU work and should not be added to CPU times.", "",
              "| Model | Direct scene ms | Routed scene ms | Scene with IDs ms | Cursor lookup µs | Composite µs | Highlight µs |", "|---|---:|---:|---:|---:|---:|---:|"]
    for m,n in NAMES.items():
        values=[median(m,"fit",mode,lambda r:r["views"]["1"]["median"]) for mode in ("none","routed","id_buffer")]
        values += [median(m,"fit","id_async",lambda r,v=v:r["views"][str(v)]["median"])*1000 for v in (2,3,4)]
        lines.append(f"| {n} | {' | '.join(fmt(v,3 if i<3 else 1) for i,v in enumerate(values))} |")
    lines += ["", "## Coordinate age", "", "Highlighting consumes the GPU result directly and does not incur this CPU round trip. Coordinate age is request issue to CPU observation, not physical mouse-to-display latency. The coordinate panel is drawn on a subsequent application frame. The delayed variant postpones the readTexture request by three application-frame advances; the pinned D3D11 backend's Map can still block internally. Immediate readback is a control, not a guarantee of a nonblocking path.", "",
              "| Model | Async coordinate age ms | Async age frames | Immediate age ms | Immediate FPS |", "|---|---:|---:|---:|---:|"]
    for m,n in NAMES.items():
        values=[median(m,"fit","id_async",lambda r:r["readback_ms"]["median"]),median(m,"fit","id_async",lambda r:r["readback_frames"]["median"]),
                median(m,"fit","id_immediate",lambda r:r["readback_ms"]["median"]),median(m,"fit","id_immediate",lambda r:r["fps"])]
        lines.append(f"| {n} | {' | '.join(fmt(v,1) for v in values)} |")
    lines += ["", "## Zoom and camera movement", "", "Each entry is no-picking FPS → marker-ID picking with async text. Queries run every frame, including camera movement. Far uses 4× fitted distance; near uses 0.5×. Motion repeats over two seconds. Fixed-center far/near queries complement the fitted-view pointer sweep.", "",
              "| Model | Far | Near | Zoom | Orbit | Pan |", "|---|---:|---:|---:|---:|---:|"]
    for m,n in NAMES.items():
        cells=[f"{median(m,c,'none',lambda r:r['fps']):.1f} → {median(m,c,'id_async',lambda r:r['fps']):.1f}" for c in ("far","near","zoom","orbit","pan")]
        lines.append(f"| {n} | {' | '.join(cells)} |")
    lines += ["", "## Additional controls", "", "One run each, so these are exploratory. Each entry is no picking → ID picking with async text. All models also exercise hidden and zero-opacity cases in correctness validation.", "",
              "| Model | Solid + markers | Alpha 0.4 | MSAA off | 16 px markers | 1920×1080 | Scene pane |", "|---|---:|---:|---:|---:|---:|---:|"]
    for m,n in NAMES.items():
        cells=[]
        for case in ("solid","alpha","single","large","1080p","pane"):
            a=median(m,case,"none",lambda r:r["fps"]); b=median(m,case,"id_async",lambda r:r["fps"])
            cells.append(f"{fmt(a,1)} → {fmt(b,1)}")
        lines.append(f"| {n} | {' | '.join(cells)} |")
    losses=[100*(1-median(m,"alpha","id_async",lambda r:r["fps"])/median(m,"alpha","none",lambda r:r["fps"])) for m in NAMES]
    lines += ["", f"**Transparency is the major exception:** at marker opacity 0.4, the ID path loses {min(losses):.0f}–{max(losses):.0f}% of FPS against its matching transparent no-picking control. This happens on all five models. The compact attachment substantially improves opaque rendering over the wide-format pilot, but it does not make blended marker rendering cheap. These controls measure the combined route/ID/highlight/readback cost; they do not isolate the cause of the transparency penalty.", "",
              "Turning MSAA off and changing marker size can have larger effects than picking itself. Higher resolution is not uniformly slower here: markers have a fixed pixel size, so changing resolution also changes their overlap. Likewise, zooming out packs more markers into the same pixels. The zoom and resolution observations are consistent with a coverage/overdraw bottleneck; they are not a bandwidth-counter measurement.", ""]
    lines += ["## Earlier clustered-compute reference", "",
              "One additional fitted-view run per model with the same Release executable. This reference searches projected vertex centers, can select occluded vertices, and returns an ID asynchronously. It does not draw the new GPU highlight or coordinate panel. Its FPS is context, not an equal-feature comparison. GPU query time below excludes scene rendering; the marker-ID lookup time above excludes producing its ID attachment.", "",
              "| Model | Points (millions) | Clustered-compute FPS | GPU query ms | Spatial-index construction s | Extra GPU buffers MiB |", "|---|---:|---:|---:|---:|---:|"]
    for r in cluster:
        m=r["metadata"]; s=r["scenarios"][0]
        lines.append(f"| {NAMES[r['model']]} | {m['points']/1e6:.2f} | {s['fps']:.1f} | {s['picking_gpu_ms']['p50']:.3f} | {m['build_ms']/1000:.2f} | {m['gpu_extra_bytes']/2**20:.1f} |")
    lines += ["", "The marker-ID path eliminates this spatial-index construction and makes lookup independent of total point count once the scene has produced the ID image. It moves work into raster output instead; total FPS does not improve over the old reference. On BearTrap the old path also retains about 180 MiB of CPU spatial-index data, which this marker-ID path does not construct.", ""]
    frames=sum(s["frames"] for r in runs for s in r["scenarios"] if s["name"]!="prime")
    scenarios=sum(s["name"]!="prime" for r in runs for s in r["scenarios"])
    lines += ["", "## Construction and storage", "",
              "No spatial index, reordered point list, or cluster bounds are constructed. The renderer's existing mesh and point-ID buffers remain necessary. Per-request CPU snapshots contain only the submitted draw transforms/ranges, not point coordinates. The current experiment accepts one imported model; production model replacement/lifetime handling needs integration.", "",
              "The compact RGBA8 ID target uses four bytes per sample. At the measured 1280×687 scene viewport and four samples, ID payload is 13.4 MiB. Offscreen color, resolved color, depth, and IDs total 43.6 MiB in texture payload, excluding driver alignment/compression and existing swapchain/mesh resources. At 1920×1047 it grows with pixel count. Eight decoded-result slots copy only 16 bytes each. The audit buffers are used only for validation.", "",
              "An initial RGBA32F attachment stored ID, depth, and draw ordinal in 16 bytes per sample. In its BearTrap pilot, no picking gave 9.5 FPS, offscreen routing 9.4 FPS, and the ID attachment alone 5.4 FPS. This exposed ID attachment output as a major cost and motivated the compact format. The wide pilot is retained separately and excluded from the compact timing tables.", "",
              "## Selection policy", "",
              "Select the nearest covered pixel center within three pixels of the cursor, then break ties by point ID. Inspect every MSAA sample; IDs are never averaged. Normal scene depth testing determines visible coverage. Surface fragments clear the ID wherever they pass that depth test, including translucent surfaces drawn over earlier markers. At partial opacity, the last depth-passing fragment owns the ID; this is a defined draw-order policy for blended pixels, not a unique perceptual ownership rule. Fully transparent markers do not produce IDs. Repeated instances sharing the same point IDs need an additional instance identifier before production integration.", "",
              "This deliberately changes the old geometric hover policy: fully hidden vertices cannot be selected, and distance is to visible raster coverage rather than the projected vertex center. Exact agreement with CPU BVH/geometric compute is neither expected nor a correctness criterion.", "",
              "## Validation and method", "",
              "![Early GPU highlight and later asynchronous coordinates](marker-picking-preview.png)", "",
              f"{len(runs)} Release timing processes, {scenarios} measured scenarios, {frames:,} measured frame intervals, excluding priming. Hidden full viewer, VSync off, 4× MSAA except the explicit off control. Each scenario warms at least 1.5 seconds and 20 frames, then measures at least 3 seconds and 30 frames. Two core rounds reverse model order and shuffle scenario order; extra controls have one round. No concurrent viewer, build, or test workload ran during timing. Clocks/thermals were not locked.", "",
              "642/642 Debug CTest tests passed, including four added marker-logic tests and four graphics tests. Debug and Release builds have no compiler warnings. Four marker-analysis tests pass. The Release GPU fixture passes 240 requests over 24 cases, including model translation during in-flight readback and translucent surfaces drawn over markers. Early/late captures verify GPU highlighting before CPU coordinates arrive. Ten image comparisons confirm color routing/ID output preserves the scene within one 8-bit channel level. Full-model audits pass 700 requests over 70 cases, including real IDs above 16 million, with no skipped requests.", "",
              "Per-view timestamps use the individual view's GPU source-frame number, deduplicate repeated samples, and discard eight-frame boundaries. Coordinate summaries discard the last eight issued frames to avoid timing contamination from the following scenario. Resource creation is outside measurement; resources remain resident in controls. The coordinate panel is present only in modes that return coordinates, and instrumentation overhead is part of all runs. Native input and physical display latency are not measured. Vulkan and DX12 were not benchmarked.", "",
              "## Recommendation", "",
              "This is a workable design for visible opaque markers: keep the selected identity and highlight on the GPU, and let coordinate text arrive later. The GPU can render the highlight without waiting for a CPU readback, using the existing D3D11 backend. That removes the readback delay from highlighting; it does not remove frame rendering, command queue, or display latency. BearTrap still renders at about 9 FPS, so this alone cannot make its interaction feel fast.", "",
              "Before production integration, investigate the transparency penalty and agree on ownership of blended pixels. A useful next experiment is a cursor-local ID pass compared with this full-viewport attachment, measuring the extra geometry cost as well as saved attachment traffic. Another is reuse of rendered color/IDs while camera and scene are unchanged, so hover updates need not redraw a huge static model. Neither optimization is implemented or measured here. Production work also needs model lifetime/cancellation and repeated-instance identity handling.", "",
              "## Artifacts", "",
              "- [Numerical results](marker-picking-results.json) and [CSV](marker-picking-results.csv)",
              "- [Source/binary provenance](marker-picking-provenance.json)",
              "- [Validation summary](marker-picking-validation.json)",
              "- [Clustered-compute reference](marker-cluster-reference.json); raw runs: `build/marker-cluster-control`.",
              "- [Reproduction harness](../tests/marker_experiment/README.md)",
              "- Raw compact timing runs: `build/marker-core` and `build/marker-extra`; model audits: `build/marker-qa-compact`.",
              "- Initial wide-format pilot: `build/marker-wide-pilot`; source snapshot: `build/marker-wide-source`.",
              "- Build/test logs and instrumentation diff: `build/marker-verification`.",
              "- Worktree: `D:/.worktree/woby-marker-picking`; production source in the main checkout is unchanged.", ""]
    (ROOT/"doc/marker-picking-experiment.md").write_text("\n".join(lines),encoding="utf-8")


if __name__=="__main__":
    main()
