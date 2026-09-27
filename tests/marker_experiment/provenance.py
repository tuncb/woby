"""Check reproducibility and record exact research sources, build logs, and binaries."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import shutil
from instrument import instrument

ROOT=Path(__file__).resolve().parents[2]
WORKTREE=Path("D:/.worktree/woby-marker-picking")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    tracked=["CMakeLists.txt","src/main.cpp","src/scene_renderer.cpp"]
    generated=["src/render_fps_probe.h","src/hover_experiment.h","src/point_index.h","src/index_tests.cpp",
               "src/candidates.sc","src/reduce.sc","src/marker_experiment.h","src/marker_hooks.h",
               "src/marker_logic.h","src/marker_logic_tests.cpp"]
    generated += [str(p.relative_to(WORKTREE)).replace("\\","/") for p in (WORKTREE/"src/marker_shaders").glob("*")]
    head=subprocess.check_output(["git","-C",str(WORKTREE),"rev-parse","HEAD"],text=True).strip()
    with tempfile.TemporaryDirectory(prefix="woby marker reproduction ") as directory:
        temp=Path(directory).resolve()
        for name in tracked + [str(p.relative_to(ROOT)).replace("\\","/") for p in (ROOT/"shaders").glob("*") if p.is_file()]:
            target=temp/name
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes(subprocess.check_output(["git","-C",str(ROOT),"show",f"{head}:{name}"]))
        instrument(temp)
        for name in tracked+generated:
            assert (temp/name).read_text()==(WORKTREE/name).read_text(), f"Reproduction mismatch: {name}"
    logs=ROOT/"build/marker-verification"
    logs.mkdir(exist_ok=True)
    for name in ("build-marker-debug.log","test-marker-debug.log","build-marker-release.log"):
        source=WORKTREE/name
        text=source.read_text()
        if name.startswith("build-"):
            assert "warning " not in text.lower() and "error " not in text.lower(), name
        shutil.copyfile(source,logs/name)
    (logs/"instrumentation.patch").write_bytes(subprocess.check_output(["git","-C",str(WORKTREE),"diff","--",*tracked]))
    harness={str(p.relative_to(ROOT)).replace("\\","/"):digest(p) for folder in ("marker_experiment","hover_experiment","render_fps")
             for p in (ROOT/"tests"/folder).glob("*") if p.is_file()}
    release=WORKTREE/"build/vs2026-vcpkg/bin/Release/woby.exe"
    shaders=WORKTREE/"build/vs2026-vcpkg/assets/shaders/dx11"
    report=dict(commit=head,worktree=str(WORKTREE),reproduced_files=tracked+generated,
                compiled_sources={name:digest(WORKTREE/name) for name in tracked+generated},harness=harness,
                binaries={str(release):digest(release)},shaders={p.name:digest(p) for p in shaders.glob("*.bin")},
                gpu=subprocess.check_output(["nvidia-smi","--query-gpu=name,driver_version,memory.total","--format=csv,noheader"],text=True).strip(),
                cpu="AMD Ryzen 7 5800H, 8 cores / 16 logical processors",physical_memory_bytes=68564348928,
                bgfx="1.129.8940-496 port 1",logs=str(logs))
    (ROOT/"doc/marker-picking-provenance.json").write_text(json.dumps(report,indent=2))
    print(f"Reproduced all {len(tracked+generated)} generated/modified compiled source files; build logs warning-free")


if __name__=="__main__":
    main()
