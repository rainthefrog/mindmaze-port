"""Compare the built-in art tools (C, mmart) with the reference Python tools on two data folders.

  python3 tests/art_compare.py <python data> <c data>

Sheets: pixels and notes. Installed pictures (hd/, wide/): pixels, with the number of differing pixels and the
largest channel difference, plus bounds.txt / wide.txt. Needs Pillow and NumPy (developer check, not CI).
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

py, c = sys.argv[1:3]
problems = 0


def px(path):
    return np.asarray(Image.open(path)).astype(int)


def compare_pngs(sub, exact):
    global problems
    a = sorted(os.path.relpath(p, py) for p in glob.glob(os.path.join(py, sub, "*.png")))
    b = sorted(os.path.relpath(p, c) for p in glob.glob(os.path.join(c, sub, "*.png")))
    if a != b:
        problems += 1
        print("%s: file lists differ: only python %s, only C %s" % (sub, sorted(set(a) - set(b))[:5], sorted(set(b) - set(a))[:5]))
    same = close = 0
    worst = (0, "")
    for f in sorted(set(a) & set(b)):
        x, y = px(os.path.join(py, f)), px(os.path.join(c, f))
        if x.shape != y.shape:
            problems += 1
            print("%s: shape %s vs %s" % (f, x.shape, y.shape))
            continue
        d = np.abs(x - y)
        if d.max() == 0:
            same += 1
            continue
        n = int((d.max(axis=-1) if d.ndim == 3 else d > 0).astype(bool).sum())
        close += 1
        if d.max() > worst[0]:
            worst = (int(d.max()), "%s (%d pixels differ)" % (f, n))
        if exact or d.max() > 2:
            problems += 1
            print("DIFF %s: %d pixels, max %d" % (f, n, d.max()))
    print("%-22s %3d files: %3d identical, %3d within +-2%s" % (sub, len(set(a) & set(b)), same, close,
                                                            ("; largest: %d in %s" % worst) if worst[0] else ""))


def compare_text(rel):
    global problems
    pa, pb = os.path.join(py, rel), os.path.join(c, rel)
    if not os.path.exists(pa) and not os.path.exists(pb):
        return
    ta = open(pa, encoding="utf-8").read() if os.path.exists(pa) else None
    tb = open(pb, encoding="utf-8").read() if os.path.exists(pb) else None
    if ta != tb:
        problems += 1
        print("TEXT DIFF %s" % rel)


compare_pngs("art/work/hd/sheets", True)
compare_pngs("art/work/wide/sheets", True)
compare_pngs("art/work/wide/masks", True)
for t in sorted(glob.glob(os.path.join(py, "art/work/*/sheets/*.txt"))):
    compare_text(os.path.relpath(t, py))
compare_pngs("hd", False)
compare_pngs("wide", False)
compare_text("hd/bounds.txt")
compare_text("wide/wide.txt")
print("RESULT:", "%d problems" % problems if problems else "the C tools match the Python reference")
sys.exit(1 if problems else 0)
