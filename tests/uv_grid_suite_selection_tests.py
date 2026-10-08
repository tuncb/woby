# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Check independent UV render cases and the existing combined/demo entry points."""
from contextlib import ExitStack, redirect_stdout
from io import StringIO
from pathlib import Path
import tempfile
from unittest import mock

import ctl_uv_grid_smoke as smoke


def check_selection(case, expected, kinds=None, output=False):
    temporary_directory = tempfile.TemporaryDirectory
    with temporary_directory(prefix="woby-uv-suite-") as directory:
        root = Path(directory).resolve()
        with ExitStack() as stack:
            # Keep every synthetic path under this test's unique directory.
            stack.enter_context(mock.patch.object(smoke.tempfile, "TemporaryDirectory",
                side_effect=lambda **kwargs: temporary_directory(dir=root, **kwargs)))
            calls = {name: stack.enter_context(mock.patch.object(smoke, name))
                     for name in ("grid_render", "analysis_demo", "inspection_demo", "layout_orientation", "demo")}
            arguments = [str(root / "viewer.exe")]
            if output:
                arguments.append(str(root / "demo"))
            if case is not None:
                arguments.extend(("--case", case))
            with redirect_stdout(StringIO()):
                smoke.main(arguments)
            for name, call in calls.items():
                assert call.call_count == expected.get(name, 0), (case, name, call.call_count)
                for args, _ in call.call_args_list:
                    assert args[0] == root / "viewer.exe"
                    assert args[1].is_relative_to(root)
                    assert args[2]["SDL_VIDEO_DRIVER"] == "woby-test-no-video-driver"
            if kinds:
                assert calls["layout_orientation"].call_args.kwargs == {"kinds": kinds}
            elif calls["layout_orientation"].called:
                assert not calls["layout_orientation"].call_args.kwargs
        assert not any(root.iterdir()), "Temporary render fixtures were not removed"


def main():
    combined = {"grid_render": 1, "analysis_demo": 1, "inspection_demo": 1, "layout_orientation": 1}
    check_selection(None, combined)
    check_selection("all", combined)
    for case, function in (("grid", "grid_render"), ("analysis", "analysis_demo"),
                           ("inspection", "inspection_demo")):
        check_selection(case, {function: 1})
    check_selection("layout_uv", {"layout_orientation": 1}, kinds=("uv",))
    check_selection("layout_quality", {"layout_orientation": 1}, kinds=("uv_quality",))
    check_selection(None, dict(combined, analysis_demo=2, inspection_demo=2, demo=1), output=True)
    print("UV render suite selection checks passed")


if __name__ == "__main__":
    main()
