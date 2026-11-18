"""Compare two data folders (reference = tools/extract/extract.py, other = the built-in C extractor).

  python3 tests/compare_data.py <reference data> <other data>

Every file must be byte-equal, except tables.json (compared as JSON: same tables, any formatting),
manifest.json (written differently by design) and questions.json (only the Python tool writes it).
"""
import json
import os
import sys

ref, other = sys.argv[1:3]
SKIP = {"manifest.json", "questions.json"}


def files(root):
    out = set()
    for d, _, names in os.walk(root):
        for n in names:
            out.add(os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/"))
    return out


a, b = files(ref), files(other)
bad = 0
for f in sorted(a | b):
    if f in SKIP or f.startswith("art/"):
        continue
    if f not in b:
        print("MISSING in %s: %s" % (other, f)); bad += 1; continue
    if f not in a:
        print("EXTRA in %s: %s" % (other, f)); bad += 1; continue
    x = open(os.path.join(ref, f), "rb").read()
    y = open(os.path.join(other, f), "rb").read()
    if f == "tables.json":
        jx, jy = json.loads(x), json.loads(y)
        jx.pop("source", None); jy.pop("source", None)
        same = jx == jy
    else:
        same = x == y
    if not same:
        print("DIFFERENT: %s" % f); bad += 1
n = len([f for f in a if f not in SKIP])
print("compared %d files: %s" % (n, "%d problems" % bad if bad else "all equal"))
sys.exit(1 if bad else 0)
