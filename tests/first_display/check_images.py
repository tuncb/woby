# /// script
# dependencies = ["Pillow>=11"]
# ///
"""Check that the measured solid-view models rendered; create a review sheet."""
import hashlib
import json
from pathlib import Path
import sys
from PIL import Image, ImageDraw

root, destination = map(Path, sys.argv[1:])
files = sorted(root.glob("*.tga"))
assert len(files) in (10, 15)
records, images = [], []
for path in files:
    with Image.open(path) as original:
        # Exclude the controls, menu, corner buttons and bottom status overlay.
        assert original.size == (1280, 720)
        viewport = original.convert("RGB").crop((475, 90, 1280, 670))
        data = viewport.tobytes()
        colored = sum(max(pixel) - min(pixel) > 40 and max(pixel) > 80
                      for pixel in zip(data[0::3], data[1::3], data[2::3]))
        assert colored > 100, (path, "No colored model geometry")
        records.append({"file": path.name, "colored_geometry_pixels": colored,
                        "viewport_sha256": hashlib.sha256(viewport.tobytes()).hexdigest()})
        if "-2.json" in path.name:
            viewport.thumbnail((500, 535))
            images.append((path.name.split("-2.json")[0], viewport.copy()))
sheet = Image.new("RGB", (1500, 1150), "#20242a")
draw = ImageDraw.Draw(sheet)
for index, (name, picture) in enumerate(images):
    x, y = (index % 3) * 500, (index // 3) * 575
    draw.text((x + 10, y + 10), name, fill="white")
    sheet.paste(picture, (x, y + 35))
sheet.save(destination)
(root / "image-checks.json").write_text(json.dumps(records, indent=2) + "\n")
print(json.dumps(records, indent=2))
