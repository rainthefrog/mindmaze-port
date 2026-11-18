"""Encarta 2006 MindMaze question files (MINDMAZE.DB / MINDMAZE.IDX) <-> JSON.

  python3 -I mmdb.py decode <MINDMAZE.DB> <MINDMAZE.IDX> <questions.json>
  python3 -I mmdb.py encode <questions.json> <MINDMAZE.DB> <MINDMAZE.IDX>

Format (fully decoded, round-trip checked by `decode` on the original files):
  DB  = records back to back, little endian:
          u32 0
          u32 question length, question text (cp1252)
          4 x answer: u8 length, text, u32 Encarta article id, u16 value (meaning unknown)
        The FIRST answer is the correct one (the game shuffles them).
  IDX = u32[36] question counts, area-major: area 0 levels 0..3, area 1 levels 0..3, ... (9 areas)
        then per question in that same area/level order: u32 question number (1-based), u32 DB offset.
"""
import json
import struct
import sys

AREAS = 9
AREA_NAMES = ["Physical Science & Technology", "Life Science", "Geography", "History",
              "Social Science", "Religion and Philosophy", "Art, Language, and Literature",
              "Performing Arts", "Sports, Hobbies, and Pets"]
LEVELS = 4


def decode(db_path, idx_path, out_path):
    db = open(db_path, "rb").read()
    idx = open(idx_path, "rb").read()
    vals = struct.unpack_from(f"<{len(idx) // 4}I", idx)
    counts = vals[:AREAS * LEVELS]
    pairs = vals[AREAS * LEVELS:]
    questions = []
    i = 0
    for area in range(AREAS):
        for level in range(LEVELS):
            for _ in range(counts[area * LEVELS + level]):
                number, off = pairs[i], pairs[i + 1]
                i += 2
                qlen = struct.unpack_from("<I", db, off + 4)[0]
                pos = off + 8
                text = db[pos:pos + qlen].decode("cp1252")
                pos += qlen
                answers = []
                for _ in range(4):
                    alen = db[pos]
                    a = db[pos + 1:pos + 1 + alen].decode("cp1252")
                    pos += 1 + alen
                    art, val = struct.unpack_from("<IH", db, pos)
                    pos += 6
                    answers.append({"text": a, "article": art, "value": val})
                questions.append({"id": number, "area": area, "level": level, "question": text,
                                  "answers": answers})
    questions.sort(key=lambda q: q["id"])
    json.dump({"format": "mindmaze-questions-1",
               "note": "answers[0] is the correct answer; area 0-8 (see areas), level 0-3",
               "areas": AREA_NAMES,
               "questions": questions},
              open(out_path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"{len(questions)} questions -> {out_path}")


def encode(json_path, db_path, idx_path):
    questions = json.load(open(json_path, encoding="utf-8"))["questions"]
    db = bytearray()
    offsets = {}
    for q in sorted(questions, key=lambda q: q["id"]):
        offsets[q["id"]] = len(db)
        t = q["question"].encode("cp1252")
        db += struct.pack("<II", 0, len(t)) + t
        for a in q["answers"]:
            s = a["text"].encode("cp1252")
            if len(s) > 255:
                raise ValueError(f"answer too long in question {q['id']}")
            db += bytes([len(s)]) + s + struct.pack("<IH", a.get("article", 0), a.get("value", 0))
    counts = [0] * (AREAS * LEVELS)
    order = []
    for area in range(AREAS):
        for level in range(LEVELS):
            group = [q for q in questions if q["area"] == area and q["level"] == level]
            counts[area * LEVELS + level] = len(group)
            order += group
    idx = struct.pack(f"<{len(counts)}I", *counts)
    for q in order:
        idx += struct.pack("<II", q["id"], offsets[q["id"]])
    open(db_path, "wb").write(db)
    open(idx_path, "wb").write(idx)
    print(f"{len(questions)} questions -> {db_path}, {idx_path}")


if __name__ == "__main__":
    if sys.argv[1] == "decode":
        decode(*sys.argv[2:5])
    elif sys.argv[1] == "encode":
        encode(*sys.argv[2:5])
    else:
        sys.exit(__doc__)
