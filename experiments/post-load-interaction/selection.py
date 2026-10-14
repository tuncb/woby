# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil", "pillow"]
# ///
"""Measure selected-model UI frames with a saved-view selection fixture.

Use --suite lifecycle so the harness puts the model and scene in one temporary
directory. This uses the normal viewer and API; it does not inject mouse events.
"""
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "stress"))
import run as stress


def selection_suite(state, file_id, stats):
    stress.configure_view(state)
    stress.operation(state, "combined", "render.set",
                     {"target": file_id, "solid": True, "triangles": True, "vertices": True})
    stress.operation(state, "selected_view", "view.create", {"name": "Selected file"})
    stress.operation(state, "cleared_view", "view.create", {"name": "Cleared file"})
    path = Path(state["working"].name).resolve() / "selection.woby"
    stress.operation(state, "save_fixture", "scene.save-as", {"path": str(path)})
    text = path.read_text(encoding="utf-8")
    prefix, selected, cleared = text.split("[[views]]")
    assert 'name = "Selected file"' in selected and 'kind = "file"' in selected
    assert "selection_order = -1" in selected
    selected = selected.replace("selection_order = -1", "selection_order = 0", 1)
    path.write_text("[[views]]".join((prefix, selected, cleared)), encoding="utf-8")
    stress.operation(state, "open_fixture", "scene.open", {"path": str(path), "onDirty": "discard"}, timeout=0)
    stress.wait_annotations(state)
    stress.operation(state, "show_panes", "pane.set", {"visible": True, "width": 320, "propertiesVisible": True})
    views = stress.operation(state, "fixture_views", "view.list")["views"]
    ids = {view["name"]: view["id"] for view in views}
    measurements = []
    for repeat in range(3):
        for name in ("Selected file", "Cleared file"):
            stress.operation(state, f"begin_{repeat}_{name}", "performance.begin")
            stress.operation(state, f"apply_{repeat}_{name}", "view.apply", {"viewId": ids[name]})
            time.sleep(.25)
            record = stress.operation(state, f"end_{repeat}_{name}", "performance.end")
            assert record["events"] and not record["droppedFrames"], record
            measurements.append({"name": name, "repeat": repeat, "recording": record})
    stress.write_json(state["directory"] / "selection-frames.json", measurements)
    file_id = next(item["id"] for item in stress.operation(state, "inventory", "objects.list")["objects"] if item["kind"] == "file")
    stress.operation(state, "select_before_remove", "view.apply", {"viewId": ids["Selected file"]})
    stress.operation(state, "remove_selected_combined", "model.remove", {"target": file_id})
    empty = stress.operation(state, "empty_scene", "scene.info")
    assert empty["fileCount"] == 0 and empty["triangleCount"] == 0, empty


if __name__ == "__main__":
    stress.SUITES["lifecycle"] = selection_suite
    stress.main()
