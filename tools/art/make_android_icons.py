"""Android launcher icons from the project's own icon drawing (tools/art/make_icon.py, no Encarta art).

  python3 tools/art/make_android_icons.py app/android/app/src/main/res
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_icon  # noqa: E402

res = sys.argv[1]
DENSITY = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
for name, k in DENSITY.items():
    d = os.path.join(res, "mipmap-" + name)
    os.makedirs(d, exist_ok=True)
    legacy = make_icon.png(make_icon.render(int(48 * k)))
    for f in ("ic_launcher.png", "ic_launcher_round.png"):
        open(os.path.join(d, f), "wb").write(legacy)
    # adaptive icon foreground: 108 dp canvas, the drawing in the middle 72 dp (the launcher crops the rest)
    full, inner = int(108 * k), int(72 * k)
    icon = make_icon.render(inner)
    pad = (full - inner) // 2
    rows = []
    for y in range(full):
        if pad <= y < pad + inner:
            rows.append(bytes(4 * pad) + icon[y - pad] + bytes(4 * (full - pad - inner)))
        else:
            rows.append(bytes(4 * full))
    open(os.path.join(d, "ic_launcher_foreground.png"), "wb").write(make_icon.png(rows))
bg = os.path.join(res, "values", "ic_launcher_background.xml")
if os.path.exists(bg):
    s = open(bg, encoding="utf-8").read()
    import re
    s = re.sub(r"#[0-9A-Fa-f]{6}", "#1F2A44", s)
    open(bg, "w", encoding="utf-8").write(s)
print("android icons written")
