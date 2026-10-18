"""Run a named set of detector experiments in sequence. Never overlap workloads."""
import argparse
import json
from pathlib import Path
import subprocess


def cases(group, models):
    bus = ["--model",str(models / "uploads_files_2720101_BusGameMap.obj")]
    power = ["--model",str(models / "powerplant.obj")]
    bennu = ["--model",str(models / "bennu_OLA_v21_PTM_very-high.obj")]
    san = ["--model",str(models / "san-miguel.obj")]
    result = []
    def add(name,work,exe="probe",profile=None,affinity=None,timeout=1800,sample_scale=1):
        result.append(dict(name=name,work=work,exe=exe,profile=profile,affinity=affinity,timeout=timeout,sample_scale=sample_scale))
    if group == "hardware":
        for profile in ("data","l2","tlb","frontend","backend","prefetch"):
            add("bus-"+profile,[*bus,"--runs","2","--operations","auto,quality,intersections,distance"],"profile",profile)
        add("bus-timechart",[*bus,"--runs","3","--operations","auto,quality,intersections,distance"],"profile","timechart")
        add("bus-vtune",[*bus,"--runs","2","--operations","auto,quality,intersections,distance"],"profile","vtune")
    elif group == "controls":
        for helpers in (0,1,3,7,8):
            add(f"bus-workers-{helpers}",[*bus,"--runs","3","--helpers",str(helpers),"--operations","auto,degenerates,distance"],"profile")
        add("bus-physical-8",[*bus,"--runs","3","--helpers","7","--operations","auto,degenerates,distance"],"profile",affinity="0,2,4,6,8,10,12,14")
        add("bus-smt-4cores",[*bus,"--runs","3","--helpers","7","--operations","auto,degenerates,distance"],"profile",affinity="0,1,2,3,4,5,6,7")
        add("bus-expanded",[*bus,"--runs","3","--expanded","--operations","auto,quality,distance"])
        add("bus-exact",[*bus,"--runs","3","--exact","--operations","auto,intersections"])
        add("bus-shuffled",[*bus,"--runs","3","--shuffle","--operations","quality,distance"])
        add("bus-unlimited-intersections",[*bus,"--runs","3","--candidate-limit","0","--pair-limit","0","--operations","intersections"],"profile")
        add("bus-local-deformation",[*bus,"--runs","3","--shift","0","--bump",".001","--operations","distance"],"profile")
        for suffix,exe,extra in (("live","probe",[]),("empty","probe",["--empty-token"]),("reference","tokenref",[])):
            add("bus-token-"+suffix,[*bus,"--runs","5","--operations","quality",*extra],exe)
        add("powerplant-token-reference",[*power,"--runs","3","--operations","quality"],"tokenref")
        add("powerplant-token-live",[*power,"--runs","1","--operations","quality"])
        add("bennu-token-live",[*bennu,"--runs","1","--operations","quality"])
        add("bennu-token-reference",[*bennu,"--runs","1","--operations","quality"],"tokenref")
        add("bus-token-reference-branch",[*bus,"--runs","3","--operations","quality"],"tokenref","branch")
        for delay in (50,500,1500,3000):
            add(f"bus-cancel-{delay}",[*bus,"--runs","3","--cancel-after-ms",str(delay),"--operations","auto,quality,intersections,distance"],"profile")
    elif group == "difference":
        for shape in ("grid","sphere"):
            add(shape+"-256-native",["--"+shape,"256","--runs","3","--operations","difference"])
        for width in (32,64,128,256,512,1024):
            add(f"grid-{width}",["--grid",str(width),"--runs","3","--operations","difference"],"profile")
        for segments in (32,64,128,256,512,1024):
            add(f"sphere-{segments}",["--sphere",str(segments),"--runs","3","--operations","difference"],"profile")
        for profile in ("branch","data","tlb"):
            add("grid-256-"+profile,["--grid","256","--runs","3","--operations","difference"],"profile",profile)
            add("sphere-256-"+profile,["--sphere","256","--runs","3","--operations","difference"],"profile",profile)
        for shape in ("grid","sphere"):
            for shift in ("0",".05","2"):
                add(f"{shape}-128-shift-{shift}",["--"+shape,"128","--shift",shift,"--runs","3","--operations","difference"],"profile")
    elif group == "queries":
        for label,model,stride in (("bus",bus,1),("powerplant",power,100),("bennu",bennu,100)):
            for shift in ("0",".00001",".001",".01"):
                add(f"{label}-query-shift-{shift}",[*model,"--runs","1","--shift",shift,"--query-stride",str(stride),"--operations","distance"],"queries")
        add("bus-query-shuffled",[*bus,"--runs","1","--shuffle","--operations","distance"],"queries")
        for label,model,stride in (("bus",bus,1),("powerplant",power,100),("bennu",bennu,100)):
            add(label+"-query-local-deformation",[*model,"--runs","1","--shift","0","--bump",".001","--query-stride",str(stride),"--operations","distance"],"queries")
        for label,model in (("powerplant",power),("bennu",bennu)):
            add(label+"-query-stride-97",[*model,"--runs","1","--query-stride","97","--operations","distance"],"queries")
        for shift in ("0",".001"):
            add("sanmiguel-query-shift-"+shift,[*san,"--runs","1","--shift",shift,"--query-stride","100","--operations","distance"],"queries")
        add("sanmiguel-query-local-deformation",[*san,"--runs","1","--shift","0","--bump",".001","--query-stride","100","--operations","distance"],"queries")
        for label,model in (("bus",bus),("powerplant",power),("bennu",bennu)):
            add(label+"-predicate-counts",[*model,"--runs","1","--operations","intersections"],"queries")
        for shape in ("grid","sphere"):
            add(shape+"-256-predicate-counts",["--"+shape,"256","--runs","1","--operations","intersections,difference"],"queries")
        for shift in ("0",".00001",".01"):
            add("bus-distance-shift-"+shift,[*bus,"--runs","3","--shift",shift,"--operations","distance"],"profile")
    elif group == "batches":
        for count in (1,8,65):
            add(f"bus-batches-{count}",[*bus,"--source-batches",str(count),"--runs","3","--operations",
                "auto,duplicate-points,duplicate-triangles,degenerates,topology,intersections"],"profile")
        for count in (1,8,32):
            add(f"sanmiguel-batches-{count}",[*san,"--source-batches",str(count),"--runs","1","--operations",
                "duplicate-points,duplicate-triangles,degenerates"],"profile")
    elif group == "isolated":
        for label,model,runs in (("bus",bus,3),("powerplant",power,1),("sanmiguel",san,1),("bennu",bennu,1)):
            add(label+"-isolated-detectors",[*model,"--runs",str(runs),"--operations",
                "duplicate-points,duplicate-triangles,duplicates,degenerates,topology"],"profile")
    elif group == "followups":
        for index,order in enumerate((("control","max3"),("max3","control"),("control","max3"),("max3","control"))):
            for variant in order:
                add(f"bus-max3-clean-{variant}-{index}",[*bus,"--runs","1","--operations","distance"],
                    "profile" if variant == "control" else "max3")
        for variant in ("control","max3"):
            add(f"bus-max3-clean-{variant}-branch",[*bus,"--runs","3","--operations","distance"],
                "profile" if variant == "control" else "max3","branch")
        add("bennu-max3-clean",[*bennu,"--runs","1","--operations","distance"],"max3")
        for profile in ("data","l2"):
            add("bus-shuffled-"+profile,[*bus,"--runs","2","--shuffle",
                "--operations","quality,distance"],"profile",profile)
        add("powerplant-all-batches-degenerates",[*power,"--source-batches","21","--runs","1",
            "--operations","degenerates"],"profile")
        add("sanmiguel-all-batches-degenerates",[*san,"--source-batches","2203","--runs","1",
            "--operations","degenerates"],"profile",timeout=1200)
    elif group == "max3":
        add("bus-max3-control",[*bus,"--runs","3","--operations","distance"],"profile")
        add("bus-max3",[*bus,"--runs","3","--operations","distance"],"max3")
        add("bus-max3-branch",[*bus,"--runs","3","--operations","distance"],"max3","branch")
        add("bennu-max3",[*bennu,"--runs","1","--operations","distance"],"max3")
        add("powerplant-max3",[*power,"--runs","1","--operations","distance"],"max3")
    elif group == "qos":
        for suffix,extra in (("default",[]),("high",["--high-qos"])):
            add("bus-qos-"+suffix,[*bus,"--runs","3","--operations","auto,quality,intersections,distance",*extra])
        add("bus-qos-high-timechart",[*bus,"--runs","3","--high-qos",
            "--operations","auto,quality,intersections,distance"],"profile","timechart")
    elif group == "large":
        add("beartrap-native",["--model",str(models / "20190810_BearTrap_Ground_Model.obj"),"--runs","1",
            "--operations","source,distance,difference,intersections,quality,auto"],timeout=2400)
        add("powerplant-stages",[*power,"--runs","1","--operations","auto,quality,intersections,topology-warm,intersections-warm,display"],"profile")
        add("bennu-stages",[*bennu,"--runs","1","--operations","auto,quality,intersections,topology-warm,intersections-warm,display"],"profile")
        add("sanmiguel-stages",[*san,"--runs","1","--operations","auto,quality,intersections"],"profile")
        add("powerplant-hardware",[*power,"--runs","1","--operations","auto,quality,intersections"],"profile","data")
        add("bennu-hardware",[*bennu,"--runs","1","--operations","auto,quality,intersections"],"profile","branch")
        add("sanmiguel-hardware",[*san,"--runs","1","--operations","auto,quality,intersections"],"profile","branch")
        add("bennu-unlimited-intersections",[*bennu,"--runs","1","--candidate-limit","0","--pair-limit","0","--operations","intersections"],"queries")
        add("powerplant-timechart",[*power,"--runs","1","--operations","auto,quality,intersections,distance"],"profile","timechart")
        add("bennu-timechart",[*bennu,"--runs","1","--operations","distance"],"profile","timechart")
        add("bennu-distance-data",[*bennu,"--runs","1","--operations","distance"],"profile","data",sample_scale=10)
        for profile in ("branch","data"):
            add("bennu-difference-"+profile,[*bennu,"--runs","1","--operations","difference"],"profile",profile,sample_scale=5)
        add("bennu-difference-cancel",[*bennu,"--runs","1","--operations","difference","--cancel-after-ms","30000"],"profile")
        for profile in ("branch","data"):
            add("powerplant-distance-"+profile,[*power,"--runs","1","--operations","distance"],"profile",profile,sample_scale=10)
        add("bus-math",[*bus,"--runs","2","--operations","auto,quality,intersections,distance"],"profile","math")
        for shape in ("grid","sphere"):
            add(shape+"-256-math",["--"+shape,"256","--runs","1","--operations","difference"],"profile","math")
        for shape,width,delay in (("grid",256,6900),("sphere",512,4500)):
            add(f"{shape}-{width}-cancel-{delay}",["--"+shape,str(width),"--runs","3",
                "--cancel-after-ms",str(delay),"--operations","difference"],"profile")
        add("bus-native-final",[*bus,"--runs","3","--operations","auto,quality,intersections,distance"])
        add("bus-token-reference-empty",[*bus,"--runs","5","--empty-token","--operations","quality"],"tokenref")
        add("bus-token-empty-branch",[*bus,"--runs","3","--empty-token","--operations","quality"],"profile","branch")
        add("bus-unlimited-predicate-counts",[*bus,"--runs","1","--candidate-limit","0","--pair-limit","0",
            "--operations","intersections"],"queries")
    else:
        raise ValueError(group)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("group",choices=("hardware","controls","difference","queries","large","qos","max3","followups","isolated","batches"))
    parser.add_argument("build",type=Path); parser.add_argument("models",type=Path); parser.add_argument("output",type=Path)
    parser.add_argument("--select",default=""); parser.add_argument("--list",action="store_true")
    args = parser.parse_args(); build = args.build.resolve(); output = args.output.resolve()
    selected = args.select.split(",") if args.select else None
    jobs = [c for c in cases(args.group,args.models.resolve()) if selected is None or c["name"] in selected]
    if args.list:
        print(json.dumps(jobs,indent=2)); return
    for case in jobs:
        if (output / case["name"]).exists():
            print("EXISTS "+case["name"],flush=True); continue
        command = ["uv","run",str(Path(__file__).with_name("run.py")),
            str(build / "bin/Release" / ("woby_detector_"+case["exe"]+".exe")),str(output),
            "--name",case["name"],"--timeout",str(case["timeout"])]
        for key in ("profile","affinity","sample_scale"):
            if case[key]:
                command += ["--"+key.replace("_","-"),str(case[key])]
        command += ["--",*case["work"]]
        completed = subprocess.run(command)
        if completed.returncode:
            print(f'FAILED {case["name"]}: {completed.returncode}; retain evidence and continue',flush=True)


if __name__ == "__main__":
    main()
