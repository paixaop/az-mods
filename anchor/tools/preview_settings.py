#!/usr/bin/env python3
"""Render production Quick controls draw calls in full-width and docked menu areas."""
import json
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "output/implemented-previews"


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    subprocess.run(["make", "out/render_settings"], cwd=ROOT, check=True)
    font = next(p for p in (Path("/System/Library/Fonts/Menlo.ttc"),
                            Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")) if p.exists())
    for w, h, mode, theme in ((1280,420,"card","dark"), (1280,420,"card","day"),
                              (1280,420,"cue","dark"), (1280,420,"sidebar","dark"),
                              (800,300,"card","dark"), (480,240,"phrase","dark"),
                              (480,240,"incoming","day"), (1280,800,"card","dark")):
        name = DEST / f"quick-controls-{w}x{h}-{mode}-{theme}"
        subprocess.run([str(ROOT/"out/render_settings"),str(name.with_suffix(".json")),
                        str(w),str(h),mode,theme],check=True)
        data = json.loads(name.with_suffix(".json").read_text())
        image = Image.new("RGB", (w,h), "#181818")
        draw = ImageDraw.Draw(image)
        for c in data["calls"]:
            color = f'#{c["col"]:06x}'
            if c["k"] == 0:
                face = ImageFont.truetype(str(font),c["px"])
                draw.text((c["x"],c["y"]+c["px"]/4),c["s"],font=face,fill=color,anchor="lt")
            else:
                box = (c["x"],c["y"],c["x"]+c["w"]-1,c["y"]+c["h"]-1)
                draw.rectangle(box,**({"fill":color} if c["k"]==1 else {"outline":color}))
        image.save(name.with_suffix(".png"))
        print(name.with_suffix(".png"))


if __name__ == "__main__":
    main()
