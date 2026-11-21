#!/usr/bin/env python3
"""Preview the baked GitHub mark masks on a dark background, exactly like
the app's welcome page would draw them."""
from pathlib import Path
import re
from PIL import Image

inc = Path(__file__).resolve().parents[2] / "src" / "psrdp_github.inc"
text = inc.read_text()

masks = {}
for side in (21, 32, 64):
    m = re.search(r"psrdp_github_%d\[(\d+)\] = \{(.*?)\};" % side, text, re.S)
    data = [int(x) for x in m.group(2).replace("\n", "").split(",") if x.strip()]
    img = Image.new("L", (side, side), 0)
    img.putdata(data)
    masks[side] = img

# Composite on the app's home background color at 4x nearest zoom.
sheet = Image.new("RGB", (3 * 64 * 4 + 40, 64 * 4 + 40), (5, 12, 30))
x = 20
for side in (21, 32, 64):
    img = masks[side].convert("RGB").point(lambda v: v)
    fg = Image.new("RGB", (side, side), (220, 231, 252))
    img = masks[side].split()[0].point(lambda v: v)
    px = Image.new("RGB", (side, side), (5, 12, 30))
    sheet_px = Image.composite(fg, px, masks[side])
    big = sheet_px.resize((side * 4, side * 4), Image.Resampling.NEAREST)
    sheet.paste(big, (x, 20))
    x += side * 4 + 20

out = folder = Path(__file__).resolve().parent / "preview.png"
sheet.save(out)
print(f"wrote {out}")
