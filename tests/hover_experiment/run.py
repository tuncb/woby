"""Serial full-viewer picking experiments and deterministic correctness sweeps."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import random
import subprocess
import time

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("fps_run", HERE.parent / "render_fps/run.py")
FPS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FPS)


def scenarios(validation=False, pilot=False, suite="core"):
    cases = [dict(case="fit", pointer="sweep"), dict(case="far", pointer="sweep", distance_factor=4),
             dict(case="near", pointer="sweep", distance_factor=.5), dict(case="zoom", pointer="center", motion="zoom"),
             dict(case="orbit", pointer="center", motion="orbit"), dict(case="pan", pointer="center", motion="pan"),
             dict(case="solid", pointer="sweep", solid=True), dict(case="alpha", pointer="sweep", opacity=.4),
             dict(case="depth", pointer="sweep", near_fraction=.01),
             dict(case="center", pointer="center"),
             dict(case="far_center", pointer="center", distance_factor=4)]
    if validation:
        cases=[c for c in cases if c["case"] not in ("center","far_center")]
    if pilot:
        cases = cases[:1]
    elif suite=="core":
        cases=cases[:6]
    elif suite=="extra":
        cases=cases[6:]
    choices = []
    for case in cases:
        for mode in (["cpu", "gpu_cluster", "gpu_flat"] if validation else ["none", "cpu", "gpu_cluster", "gpu_flat"]):
            if not validation and mode=="gpu_flat" and case["case"] not in ("fit","far"):
                continue
            choices.append(dict(solid=False, vertices=True, **{k:v for k,v in case.items() if k != "solid"}) | {
                "solid": case.get("solid", False), "name": f"{case['case']}_{mode}", "picker":mode})
    if not validation and suite!="extra":
        choices.append(dict(name="fit_legacy", case="fit", picker="legacy", solid=False, vertices=True, pointer="sweep"))
        for mode in ("gpu_immediate","gpu_resident"):
            choices.append(dict(name="fit_"+mode,case="fit",picker=mode,solid=False,vertices=True,pointer="sweep"))
    return choices


def main():
    p=argparse.ArgumentParser()
    p.add_argument("--binary",type=Path,required=True)
    p.add_argument("--models",type=Path,default=Path("D:/temp/obj_tests"))
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--rounds",type=int,default=2)
    p.add_argument("--model-filter",default="")
    p.add_argument("--validate",action="store_true")
    p.add_argument("--pilot",action="store_true")
    p.add_argument("--seconds",type=float,default=3)
    p.add_argument("--scenario-filter",default="")
    p.add_argument("--suite",choices=("core","extra","all"),default="core")
    args=p.parse_args()
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=False)
    models=sorted(m for m in args.models.resolve().glob("*.obj") if args.model_filter in m.name)
    choices=scenarios(args.validate,args.pilot,args.suite)
    if args.scenario_filter:
        choices=[s for s in choices if s["name"] in args.scenario_filter.split(",")]
    assert models and choices
    startup=subprocess.STARTUPINFO(); startup.dwFlags=subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow=0
    for iteration in range(args.rounds):
        for model in models if iteration%2==0 else reversed(models):
            ordered=choices.copy()
            if not args.validate:
                random.Random(991+iteration).shuffle(ordered)
            # Prime both variants before the measured scenarios. Excluded from analysis.
            if not args.validate:
                ordered.insert(0,dict(name="prime",case="prime",picker="gpu_cluster",solid=False,vertices=True,pointer="center"))
            stem=output/f"{model.stem}-{iteration+1}"
            config=dict(output=str(stem),binary=str(args.binary.resolve()),scenarios=ordered,validate=args.validate,
                        warmup_seconds=0 if args.validate else 1.5,warmup_frames=2 if args.validate else 20,
                        measure_seconds=0 if args.validate else args.seconds,min_frames=8 if args.validate else 30)
            config_path=stem.with_suffix(".config.json"); config_path.write_text(json.dumps(config,indent=2))
            print(f"START {model.name} round={iteration+1} validate={args.validate}",flush=True)
            started=time.perf_counter()
            with stem.with_suffix(".log").open("w") as log:
                result=subprocess.run([str(args.binary.resolve()),"--file",str(model),"--instance","hover-experiment",
                    "--log-level","warn","--log-file",str(stem.with_suffix(".app.log"))],cwd=output,startupinfo=startup,
                    env=dict(os.environ,WOBY_RENDER_FPS_CONFIG=str(config_path)),stdout=log,stderr=log,timeout=3600)
            if result.returncode:
                raise RuntimeError(f"Viewer exit {result.returncode}: {stem}.log")
            summary=FPS.summarize_run(stem)
            summary.update(model=model.name,round=iteration+1,process_seconds=time.perf_counter()-started,
                           hover_path=str(stem.with_suffix(".hover.json")))
            stem.with_suffix(".summary.json").write_text(json.dumps(summary,indent=2))
            with (output/"runs.jsonl").open("a") as stream:
                stream.write(json.dumps(summary)+"\n")
            for row in summary["scenarios"]:
                print(f"  {row['name']:24} {row['fps']:7.1f} FPS  hover CPU {row['stages']['hover_pick']:.3f} ms  GPU {row['gpu_p50_ms']}",flush=True)
            print(f"DONE {summary['process_seconds']:.1f}s",flush=True)


if __name__ == "__main__":
    main()
