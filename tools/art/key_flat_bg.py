"""Make the flat background of pictures transparent, in place (build step for the HD award pictures).

  python3 -I key_flat_bg.py <png> [<png> ...]

The background is the panel colour (217,214,197); every pixel close to it (distance < TOL) that is connected
to the border becomes transparent, with a 1 px soft edge. The award pictures are 66x65 squares on the panel
colour; the HD versions' square is a little lighter than the panel, which showed as a box in the game.
"""
import sys

import numpy as np
from PIL import Image, ImageFilter

PANEL = (217, 214, 197)     # the game panel colour = the award pictures' background in the originals
TOL = 34                   # the HD backgrounds wander around it (painted texture), up to ~30


def key(path):
    im = Image.open(path).convert("RGBA")
    a = np.asarray(im).astype(np.int32)
    h, w = a.shape[:2]
    bg = np.array(PANEL)
    near = np.sqrt(((a[..., :3] - bg) ** 2).sum(axis=2)) < TOL
    out = np.zeros((h, w), bool)
    stack = [(y, x) for y in (0, h - 1) for x in range(w)] + [(y, x) for x in (0, w - 1) for y in range(h)]
    while stack:
        y, x = stack.pop()
        if out[y, x] or not near[y, x]:
            continue
        out[y, x] = True
        stack += [(y + dy, x + dx) for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)) if 0 <= y + dy < h and 0 <= x + dx < w]
    alpha = Image.fromarray(np.where(out, 0, a[..., 3]).astype(np.uint8)).filter(ImageFilter.BoxBlur(0.6))
    alpha = np.minimum(np.asarray(alpha), a[..., 3]).astype(np.uint8)
    res = np.concatenate([a[..., :3].astype(np.uint8), alpha[..., None]], axis=2)
    Image.fromarray(res, "RGBA").save(path)
    return int(out.sum()), w * h


for p in sys.argv[1:]:
    n, t = key(p)
    print("keyed %s: %d of %d pixels transparent" % (p, n, t))
