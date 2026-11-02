#!/usr/bin/env python3
"""Render the actual C drawing calls for all three display settings (requires Pillow)."""
import argparse
import html
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT.parent / "sdk")
    parser.add_argument("--font", type=Path)
    args = parser.parse_args()
    from PIL import Image, ImageDraw, ImageFont
    fonts = [args.font, Path("/System/Library/Fonts/Menlo.ttc"),
             Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")]
    font = next((p for p in fonts if p and p.is_file()), None)
    if not font:
        parser.error("Pass --font with a monospace TrueType font")
    dest = ROOT / "output/implemented-previews"
    dest.mkdir(parents=True, exist_ok=True)
    frames = {}
    subprocess.run(["make", "ui-test", f"AZ_SDK_INCLUDE={args.sdk.resolve() / 'include'}"],
                   cwd=ROOT, check=True)
    for key in ("ribbon", "card", "sidebar"):
        for state in ("play", "beats", "prepared", "looping", "now", "exit", "selector", "b-selector", "needtracks", "ready", "detecting", "tab", "hidden-tab"):
            filename = dest / f"anchor-{key}-{state}"
            subprocess.run([str(ROOT / "out/render_ui"), str(filename.with_suffix(".json")), state, key], check=True)
            data = json.loads(filename.with_suffix(".json").read_text())
            im = Image.new("RGB", (data["w"], data["h"]), "black")
            draw = ImageDraw.Draw(im)
            svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{data["w"]}" height="{data["h"]}" viewBox="0 0 {data["w"]} {data["h"]}">']
            for c in data["calls"]:
                color = f'#{c["col"]:06x}'
                if c["k"] == 0:
                    face = ImageFont.truetype(str(font), c["px"])
                    draw.text((c["x"], c["y"] + c["px"] / 4), c["s"], font=face, fill=color, anchor="lt")
                    svg.append(f'<text x="{c["x"]}" y="{c["y"] + c["px"]}" font-family="monospace" font-size="{c["px"]}" fill="{color}">{html.escape(c["s"])}</text>')
                else:
                    box = (c["x"], c["y"], c["x"] + c["w"] - 1, c["y"] + c["h"] - 1)
                    draw.rectangle(box, **({"fill": color} if c["k"] == 1 else {"outline": color}))
                    svg.append(f'<rect x="{c["x"]}" y="{c["y"]}" width="{c["w"]}" height="{c["h"]}" fill="{color if c["k"] == 1 else "none"}" stroke="{color}"/>')
            svg.append("</svg>")
            filename.with_suffix(".svg").write_text("\n".join(svg))
            im.save(filename.with_suffix(".png"))
            frames[key, state] = im
    widths = [872, 884, 190]
    sheet = Image.new("RGB", (sum(widths) + 80, 4 * 390 + 60), "#15191e")
    draw = ImageDraw.Draw(sheet)
    title = ImageFont.truetype(str(font), 20)
    x = 20
    for key, w in zip(("ribbon", "card", "sidebar"), widths):
        draw.text((x, 15), key.upper(), font=title, fill="white")
        for row, state in enumerate(("play", "now", "exit", "selector")):
            y = 55 + row * 390
            draw.text((x, y), state.upper(), font=title, fill="#b8c0c8")
            sheet.paste(frames[key, state], (x, y + 30))
        x += w + 20
    sheet.save(dest / "comparison.png")
    states = [("play", "BARS"), ("beats", "BEATS"), ("looping", "B PLAYING / LOOPING"), ("needtracks", "NO TRACKS LOADED")]
    detail = Image.new("RGB", (860, 452), "#15191e")
    labels = ImageDraw.Draw(detail)
    for i, (state, label) in enumerate(states):
        x = 20 + i * 210
        labels.text((x, 12), label, font=ImageFont.truetype(str(font), 12), fill="#dce4ea")
        detail.paste(frames["sidebar", state], (x, 40))
    detail.paste(frames["sidebar", "tab"], (20, 391))
    detail.paste(frames["sidebar", "hidden-tab"], (124, 391))
    labels.text((230, 401), "Open: close triangle / Closed: triangle + ANCHOR", font=ImageFont.truetype(str(font), 14), fill="#dce4ea")
    detail.save(dest / "sidebar-states.png")
    print(dest / "comparison.png")


if __name__ == "__main__":
    main()
