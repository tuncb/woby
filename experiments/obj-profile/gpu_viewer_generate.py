"""Generate a viewer copy with a short inactive-frame period and upload-frame logs."""
from pathlib import Path
import sys
from generate import replace

root, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
text = (root / "src/frame_pacing.cpp").read_text(encoding="utf-8")
text = replace(text, "uint64_t{50'000'000}", "uint64_t{1'000'000}")
(output / "frame_pacing.cpp").write_text(text, encoding="utf-8")
text = (root / "src/main.cpp").read_text(encoding="utf-8")
anchor = "        && elapsedMilliseconds(uploadStart) < 4.0);\n    return {};"
text = replace(text, anchor, "        && elapsedMilliseconds(uploadStart) < 4.0);\n"
    "    spdlog::info(\"research upload_frame bytes={} wall_ms={}\", frameBytes, elapsedMilliseconds(uploadStart));\n    return {};")
(output / "main.cpp").write_text(text, encoding="utf-8")
