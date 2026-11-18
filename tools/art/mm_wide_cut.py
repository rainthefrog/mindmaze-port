"""Cut the Codex widescreen results (<data>/art/work/wide/out_gen) into wide pictures for the game.

  python3 mm_wide_cut.py <data folder>   -> <data>/wide/<key>.png (2x), <key>_1x.png (1x),
                                                wide.txt ("key left right" extension in 1x px)

A wide picture is Codex's whole painting, never mixed with the game's own picture (the user saw the old picture
drawn over the new one as odd scenes and seams, 2026-11-16):
  - FILL sheets: the painting inside the sheet's wide box (picture + green strips). Every FILL result registers
    to its sheet exactly (checked: bbox and difference against shifted / scaled versions), so the game's click
    areas and the layers still line up. Transparent pictures are keyed on magenta like tools/mm_cut.py; opaque
    ones get their outer 4 rows/columns repeated inwards (they carry magenta from the sheet around them).
  - STRETCH (logonscr): the same crop. Its first result was painted over the whole canvas (another layout);
    check_bounds now warns about any result whose painted area is not the sheet box (regenerate those).
1x versions are the 2x scaled down (premultiplied), transparent ones hard-keyed like the originals.
"""
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mm_cut  # noqa: E402  (key_alpha, unmix, original)

ROOT = sys.argv[1]
WIDE = os.path.join(ROOT, "art", "work", "wide")    # ROOT = the data folder
OUT = os.path.join(ROOT, "wide")                    # the game reads data/wide/<key>.png + wide.txt
# pictures not to cut (none: objects keep their original positions in widescreen, 2026-11-16, so the tall vase
# at the old right edge needs its extension)
SKIP = set()


def check_bounds(R, x0, y0, tw, h, tol=12):
    """The painting must stay in the sheet box: warn when the painted (non-magenta) area goes more than `tol`
    sheet px past it (the first logonscr result filled the whole canvas: 37+ px). Painting less than the box is
    normal for transparent pictures (layers and objects end inside it); a few px into the margin is cropped."""
    a = np.asarray(R.resize((768, 512))).astype(np.int32)
    paint = ~((a[..., 0] > 200) & (a[..., 2] > 200) & (a[..., 1] < 60))
    rows, cols = np.nonzero(paint.any(axis=1))[0], np.nonzero(paint.any(axis=0))[0]
    if not len(rows):
        return "empty result"
    got = (cols.min(), rows.min(), cols.max(), rows.max())
    want = (x0, y0, x0 + tw - 1, y0 + h - 1)
    over = (want[0] - got[0], want[1] - got[1], got[2] - want[2], got[3] - want[3])
    if any(o > tol for o in over):
        return "painted area %s, sheet box %s (sheet px)" % (got, want)
    return ""


def main():
    M = json.load(open(os.path.join(WIDE, "map.json")))
    os.makedirs(OUT, exist_ok=True)
    lines = []
    for name, e in M["sheets"].items():
        f, w, h = e["file"], e["w"], e["h"]
        if f in SKIP:
            continue
        key = f.split(".")[0].lower()
        res = os.path.join(WIDE, "out_gen", name + ".png")
        if not os.path.exists(res):
            print("missing", res)
            continue
        tw = w + M["ext"] * len(e["sides"])
        x0, y0 = e["sheet_x"], e["y"]
        R = Image.open(res).convert("RGB")
        if R.size != (1536, 1024):
            R = R.resize((1536, 1024), Image.LANCZOS)
        opaque = mm_cut.original(f)[..., 3].min() >= 255

        warn = check_bounds(R, x0, y0, tw, h)
        if warn:
            print("WARNING %s: %s - regenerate this sheet (codex/wide/PROMPT.txt BOUNDS rule)" % (name, warn))
        rgb = np.asarray(R).astype(np.float32)[2 * y0:2 * (y0 + h), 2 * x0:2 * (x0 + tw)].copy()
        if opaque:
            E = 4                                   # edge rows/columns mixed with the magenta around the box
            rgb[:E] = rgb[E]
            rgb[-E:] = rgb[-E - 1]
            rgb[:, :E] = rgb[:, E:E + 1]
            rgb[:, -E:] = rgb[:, -E - 1:-E]
            wide2 = np.concatenate([rgb, np.full(rgb.shape[:2] + (1,), 255, np.float32)], axis=2)
        else:
            a = mm_cut.key_alpha(rgb)
            a[a < 0.08] = 0
            wide2 = np.concatenate([mm_cut.unmix(rgb, a), (a * 255)[..., None]], axis=2)

        im2 = Image.fromarray(np.clip(wide2 + 0.5, 0, 255).astype(np.uint8), "RGBA")
        wide1 = np.asarray(im2.convert("RGBa").resize((tw, h), Image.LANCZOS).convert("RGBA")).copy()
        wide1[..., 3] = 255 if opaque else np.where(wide1[..., 3] < 128, 0, 255)
        im2.save(os.path.join(OUT, key + ".png"))
        Image.fromarray(wide1, "RGBA").save(os.path.join(OUT, key + "_1x.png"))
        left = M["ext"] if "L" in e["sides"] else 0
        right = M["ext"] if "R" in e["sides"] else 0
        lines.append("%s %d %d" % (key, left, right))
        print("%-16s %-10s %-7s %dx%d" % (name, key, e["mode"], tw, h))
    # drop outputs of pictures no longer cut (SKIP)
    for k in [p.split(".")[0].lower() for p in SKIP]:
        for suffix in (".png", "_1x.png"):
            p = os.path.join(OUT, k + suffix)
            if os.path.exists(p):
                os.remove(p)
    open(os.path.join(OUT, "wide.txt"), "w").write("\n".join(sorted(lines)) + "\n")
    print(len(lines), "wide pictures ->", OUT)


if __name__ == "__main__":
    main()
