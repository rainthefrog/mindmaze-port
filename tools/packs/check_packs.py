"""Checks every packs/*.json question pack (2026-11-17): format header, 4 non-empty answers, valid area
and level, non-empty question, unique ids; prints the 9x4 area x level table per pack.

    python3 -I tools/packs/check_packs.py
"""
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmpack  # noqa: E402


def check(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    errs = []
    if d.get("format") != "mindmaze-questions-1":
        errs.append("format")
    p = d.get("pack") or {}
    for k in ("id", "name", "url", "license", "attribution", "created", "count"):
        if k not in p:
            errs.append("pack." + k)
    if len(d.get("areas", [])) != 9:
        errs.append("areas")
    qs = d.get("questions", [])
    if p.get("count") != len(qs):
        errs.append("count %r != %d" % (p.get("count"), len(qs)))
    ids = set()
    for q in qs:
        bad = []
        if not isinstance(q.get("id"), int) or not 0 < q["id"] < 2 ** 31 or q["id"] in ids:
            bad.append("id")
        ids.add(q.get("id"))
        if q.get("area") not in range(9):
            bad.append("area")
        if q.get("level") not in range(4):
            bad.append("level")
        if not isinstance(q.get("question"), str) or not q["question"].strip():
            bad.append("question")
        a = q.get("answers")
        if not isinstance(a, list) or len(a) != 4 or any(not (x.get("text") or "").strip() for x in a):
            bad.append("answers")
        if bad:
            errs.append("q %r: %s" % (q.get("id"), ",".join(bad)))
    print("%s: %d questions, %s" % (os.path.basename(path), len(qs), "OK" if not errs else "ERRORS"))
    for e in errs[:20]:
        print("  ", e)
    print(mmpack.format_table(mmpack.table(qs), d.get("areas")))
    return not errs


def main():
    ok = True
    for path in sorted(glob.glob(os.path.join(mmpack.PACKS, "*.json"))):
        if os.path.basename(path) == "online_map.json":
            continue
        ok &= check(path)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
