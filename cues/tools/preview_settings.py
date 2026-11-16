#!/usr/bin/env python3
"""Render the mod's actual SDK draw calls for visual inspection."""
import json
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def main():
    destination = ROOT / "out" / "previews"
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(["make", "out/render_settings"], cwd=ROOT, check=True)
    font = next(path for path in (
        Path("/System/Library/Fonts/Menlo.ttc"),
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"),
    ) if path.exists())
    for width, height, mode, theme in (
        (1216, 535, "groups", "dark"), (1216, 535, "phrase", "day"),
        (1280, 420, "groups", "dark"), (1280, 420, "phrase", "dark"),
        (1280, 420, "absolute", "dark"), (1280, 420, "global", "day"),
        (800, 420, "group", "dark"), (480, 240, "global", "day"),
        (480, 240, "keyboard", "dark"), (240, 180, "phrase", "dark"),
    ):
        base = destination / f"cues-{width}x{height}-{mode}-{theme}"
        subprocess.run([str(ROOT / "out/render_settings"), str(base.with_suffix(".json")),
                        str(width), str(height), mode, theme], check=True)
        data = json.loads(base.with_suffix(".json").read_text())
        image = Image.new("RGB", (width, height), "#181818")
        draw = ImageDraw.Draw(image)
        for call in data["calls"]:
            color = f'#{call["col"]:06x}'
            if call["k"] == 0:
                face = ImageFont.truetype(str(font), call["px"])
                draw.text((call["x"], call["y"] + call["px"] / 4), call["s"],
                          font=face, fill=color, anchor="lt")
            else:
                box = (call["x"], call["y"], call["x"] + call["w"] - 1,
                       call["y"] + call["h"] - 1)
                draw.rectangle(box, **({"fill": color} if call["k"] == 1 else {"outline": color}))
        image.save(base.with_suffix(".png"))
        print(base.with_suffix(".png"))


if __name__ == "__main__":
    main()
