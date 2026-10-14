# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil", "pillow"]
# ///
"""Use the stress harness to measure display modes and completed-model removal.

Use --suite render --select NAME. Other arguments are those of tests/stress/run.py.
Set WOBY_INTERACTION_DENSE_RENDER=1 for full point rendering and X-ray edges.
"""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "stress"))
import run as stress


def interaction_suite(state, file_id, stats):
    stress.configure_view(state)
    stress.operation(state, "hide_panes", "pane.set", {"visible": False, "propertiesVisible": False})
    if os.environ.get("WOBY_INTERACTION_DENSE_RENDER") == "1":
        stress.operation(state, "dense_render", "render.set",
                         {"target": "scene", "adaptivePoints": False, "xray": True})
    for name, flags in (
        ("solid", dict(solid=True, triangles=False, vertices=False)),
        ("edges", dict(solid=False, triangles=True, vertices=False)),
        ("vertices", dict(solid=False, triangles=False, vertices=True)),
        ("combined", dict(solid=True, triangles=True, vertices=True)),
    ):
        stress.operation(state, "enable_" + name, "render.set", dict(target=file_id, **flags))
        for repeat in range(3):
            stress.frames(state, f"{name}_{repeat}")
    stress.operation(state, "status_combined", "status")
    stress.operation(state, "remove_combined", "model.remove", {"target": file_id})
    empty = stress.operation(state, "empty_after_combined", "scene.info")
    assert empty["fileCount"] == 0 and empty["triangleCount"] == 0, empty
    loaded = stress.operation(state, "reload_for_solid", "model.add", {"path": state["model"]}, timeout=0)
    stress.wait_annotations(state)
    stress.operation(state, "remove_solid", "model.remove", {"target": loaded["addedIds"][0]})
    empty = stress.operation(state, "empty_after_solid", "scene.info")
    assert empty["fileCount"] == 0 and empty["triangleCount"] == 0, empty


if __name__ == "__main__":
    stress.SUITES["render"] = interaction_suite
    stress.main()
