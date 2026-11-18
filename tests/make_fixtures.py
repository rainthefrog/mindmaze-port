"""Write a synthetic data folder for the tests (no Encarta content).

  python3 tests/make_fixtures.py <out dir>

Everything here is invented: flat-colour pictures with the sizes the game's layout expects, made-up room
objects, texts and questions, short beeps and one-note MIDI files. The folder has the same shape as the one
tools/extract/extract.py writes from a real disc, so the game and the tests run on either.
"""
import json
import math
import os
import random
import struct
import sys
import wave

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "extract"))
import mmdb  # noqa: E402

out = sys.argv[1]
os.makedirs(os.path.join(out, "res"), exist_ok=True)
rnd = random.Random(2006)


# ---- pictures: 8-bit BMP, palette index 254 = cyan = transparent ----
def palette():
    pal = []
    for i in range(256):
        h = i / 256.0 * 6.0
        k = int(h)
        f = h - k
        v = 90 + (i * 37) % 130
        r, g, b = [(v, int(v * f), 40), (int(v * (1 - f)), v, 40), (40, v, int(v * f)),
                   (40, int(v * (1 - f)), v), (int(v * f), 40, v), (v, 40, int(v * (1 - f)))][k]
        pal.append((r, g, b))
    pal[0] = (0, 0, 0)
    pal[1] = (214, 204, 170)      # panel colour of the fixtures
    pal[254] = (0, 255, 255)
    pal[255] = (255, 255, 255)
    return pal


PAL = palette()


def bmp(path, w, h, pixel):
    """pixel(x, y) -> palette index, y from the top."""
    row = (w + 3) & ~3
    data = bytearray()
    for y in range(h - 1, -1, -1):
        data += bytes(pixel(x, y) for x in range(w)) + bytes(row - w)
    head = struct.pack("<2sIHHI", b"BM", 54 + 1024 + len(data), 0, 0, 54 + 1024)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 8, 0, len(data), 2835, 2835, 256, 0)
    with open(path, "wb") as f:
        f.write(head + info + b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in PAL) + data)


def colour_of(name):
    return 2 + sum(ord(ch) * (i + 3) for i, ch in enumerate(name)) % 250


def flat(name, w, h, keyed_corners=False):
    c = colour_of(name)
    c2 = 2 + (c + 3) % 250

    def px(x, y):
        if keyed_corners and (x + y < 6 or (w - 1 - x) + y < 6):
            return 254
        if x == 0 or y == 0 or x == w - 1 or y == h - 1:
            return 0
        return c if (x // 8 + y // 8) % 2 else c2
    bmp(os.path.join(out, name.lower()), w, h, px)


def award(name):
    c = colour_of(name)
    bmp(os.path.join(out, name.lower()), 66, 65, lambda x, y: c if 13 <= x < 53 and 12 <= y < 52 else 254)


# full-size screens and fixed pictures (sizes the layout code relies on)
for n, w, h in [("LOGONSCR.DIB", 636, 437), ("TITLESCR.DIB", 636, 437), ("GRATING.DIB", 636, 301),
                ("RMOFDOOR.DIB", 636, 301), ("ATRIUM.DIB", 636, 301), ("BANNER.DIB", 363, 301),
                ("DIALOG.DIB", 290, 241), ("OPEN1.DIB", 435, 380), ("OPEN2.DIB", 435, 380)] + \
               [("END%d.DIB" % i, 435, 286) for i in range(1, 7)]:
    flat(n, w, h)
for n, w, h in [(29150, 68, 18), (29151, 68, 18), (29152, 68, 18), (29153, 68, 18), (29154, 108, 24),
                (29155, 108, 24), (29156, 108, 24), (29157, 108, 24), (29158, 72, 21), (29159, 72, 21),
                (29160, 72, 21), (29161, 72, 21), (29162, 72, 21), (29163, 72, 17), (29164, 48, 12)]:
    flat("res/%d.bmp" % n, w, h)
bmp(os.path.join(out, "res", "29166.bmp"), 90, 80, lambda x, y: 1)      # its first pixel is the panel colour

# ---- room objects (made up) ----
objects = []


def obj(name, x, y, w, h, slots, rect=None, half=1, flags=0, oid=0, keyed=True):
    flat(name, w, h, keyed)
    objects.append({"name": name, "x": x, "y": y, "rect": list(rect or (0, 0, 0, 0)), "half": half,
                    "slots": slots, "flags": flags, "id": oid})


# doors, in the order the game expects: north, south, east, west, stairs
obj("FXNORTH.DIB", 270, 70, 90, 130, [0, 1, 0, 0, 0, 0, 1], (10, 10, 80, 130), oid=0)
obj("FXSOUTH.DIB", 296, 254, 44, 43, [1, 0, 0, 0, 0, 0, 0], None, half=0, oid=1)
obj("FXEAST.DIB", 520, 80, 86, 170, [0, 1, 0, 1, 0, 0, 0], (4, 6, 76, 168), oid=2)
obj("FXWEST.DIB", 30, 80, 86, 170, [0, 1, 1, 0, 0, 0, 0], (4, 6, 76, 168), oid=3)
obj("FXSTAIRS.DIB", 240, 0, 260, 226, [0, 1, 0, 0, 0, 0, 0], (16, 16, 150, 206), oid=4)
groups = {"doors": 5}
for i in range(4):
    obj("FXFLOOR%d.DIB" % i, 0, 200 + 5 * i, 635, 100 - 5 * i, [0, 0, 0, 0, 0, 1, 0], keyed=False)
groups["floors"] = 4
for i in range(3):
    obj("FXCEIL%d.DIB" % i, 0, 0, 635, 60 + 6 * i, [0, 0, 0, 0, 0, 0, 0], keyed=False)
groups["ceilings"] = 3
for i in range(2):
    obj("FXWALL%d.DIB" % i, 0, 0, 636, 301, [0, 0, 0, 0, 0, 0, 0], half=0, keyed=False)
groups["walls"] = 2
N_CHAR = 9
for i in range(N_CHAR):
    w, h = 80 + 22 * i, 150 + 12 * (i % 5)
    x = rnd.randrange(0, 636 - w)
    slots = [1 if i % 3 else 0, 0 if i % 3 else 1, x < 150, x + w > 480, i % 4 == 1, i % 5 == 2, i % 6 == 3]
    obj("FXCHAR%d.DIB" % i, x, 301 - h - (i % 3) * 10, w, h, [int(s) for s in slots],
        (10, 10, w - 10, h - 10), flags=i % 4, oid=7 + i)
groups["characters"] = N_CHAR
N_FURN = 12
for i in range(N_FURN - 2):
    w, h = 40 + 17 * i, 50 + 11 * (i % 6)
    x = rnd.randrange(0, 636 - w)
    slots = [i % 2, 1 - i % 2, x < 120, x + w > 520, i % 5 == 0, i % 3 == 0, 0]
    obj("FXFURN%d.DIB" % i, x, 280 - h - 9 * (i % 4), w, h, [int(s) for s in slots], flags=i % 2)
obj("FXPIC1.DIB", 156, 80, 53, 70, [0, 1, 0, 0, 0, 1, 0], (3, 4, 47, 67), oid=31)      # clickable pictures
obj("FXPIC2.DIB", 425, 77, 61, 54, [0, 1, 0, 0, 1, 0, 0], (4, 5, 54, 50), oid=35)
groups["furniture"] = N_FURN

RANKS = [("Fixture rank 0", "FXAWARD0.DIB", 0, 2499), ("Fixture rank 1", "FXAWARD1.DIB", 2500, 4999),
         ("Fixture rank 2", "FXAWARD2.DIB", 5000, 9999), ("Fixture rank 3", "FXAWARD3.DIB", 10000, 14999),
         ("Fixture rank 4", "FXAWARD4.DIB", 15000, 19999), ("Fixture top rank", "FXAWARD4.DIB", 20000, 32000)]
for name in sorted({r[1] for r in RANKS}):
    award(name)

speech = [["Fixture character %d says line %d.\\nSecond line, with an accent: café." % (k + 7, i + 1)
           for i in range(1 + k % 3)] for k in range(22)]
speech.append(["Fixture ending text: the test maze is solved."])

AREAS = ["Fixture Area %d" % i for i in range(9)]
strings = {
    29002: "GRATING.DIB", 29003: "LOGONSCR.DIB", 29005: "mindmaze.db", 29006: "RMOFDOOR.DIB", 29007: "BANNER.DIB",
    29008: "DIALOG.DIB", 29009: "Fixture Maze", 29010: "Answer 1", 29011: "Answer 2", 29012: "Answer 3",
    29013: "Answer 4", 29014: "Every Area", 29024: "New player", 29025: "Delete", 29026: "mindmaze.idx",
    29027: "TITLESCR.DIB", 29028: "Pick an area", 29029: "Level", 29030: "0", 29031: "Progress:", 29032: "/20000",
    29033: "Fixture motto", 29034: "66", 29035: "A.", 29036: "B.", 29037: "C.", 29038: "D.",
    29039: "Fixture: wrong answer, one more try.", 29040: "Fixture: wrong again. The correct answer is %s.",
    29041: "Fixture: right, %u points added to the score.", 29042: "Fixture: look this subject up?",
    29043: "Tourist", 29044: "Fixture: choose at least one player from the list.",
    29045: "Fixture: five players at most.", 29046: "Fixture: that name is taken.",
    29048: "Fixture: the question files could not be opened.", 29049: "Fixture: click the stairs to go on.",
    29050: "Fixture: no music.", 29057: "Fixture: right, %u point added to the score.",
    29061: "Start &New Game", 29062: "&Quit Game", 29063: "Turn &Music Off", 29064: "Turn &Music On",
    29065: "Turn &Sound Effects Off", 29066: "Turn &Sound Effects On", 29067: "Fixture Maze", 29068: "New Game",
    29069: "Quit Game"}
for i in range(9):
    strings[29015 + i] = AREAS[i]

MUSIC = ["INTRO.MID", "BGLOOP1.MID", "BGLOOP2.MID", "BGLOOP3.MID", "QUESTION.MID"]
SOUNDS = ["FX%02d.WAV" % i for i in range(35)]
tables = {"format": "mindmaze-tables-1", "source": "synthetic test fixture", "groups": groups, "objects": objects,
          "ranks": [{"name": n, "image": im, "min": lo, "max": hi} for n, im, lo, hi in RANKS],
          "speech": speech, "picture_areas": AREAS, "music": MUSIC, "sounds": SOUNDS,
          "strings": {str(k): v for k, v in sorted(strings.items())}}
with open(os.path.join(out, "tables.json"), "w", encoding="utf-8") as f:
    json.dump(tables, f, ensure_ascii=True, indent=1)        # ASCII + \u escapes: exercises the reader

# ---- questions: MINDMAZE.DB / .IDX format and the same as JSON ----
PER_LIST = 12
questions = []
n = 0
for area in range(9):
    for level in range(4):
        for i in range(PER_LIST):
            n += 1
            text = "Fixture question %d (area %d, level %d): which answer is marked right?" % (n, area, level + 1)
            if i == 1:
                text = "Fixture question %d: salt &amp; pepper, caf&eacute; or <it>Italic Title</it>?" % n
            if i == 2:
                text = "Fixture question %d with cp1252 characters: naïve — yes?" % n
            questions.append({"id": n, "area": area, "level": level, "question": text,
                              "answers": [{"text": "Right %d" % n, "article": 0, "value": 0}] +
                                         [{"text": "Wrong %s %d" % (c, n), "article": 0, "value": 0} for c in "ABC"]})
qjson = os.path.join(out, "questions.json")
with open(qjson, "w", encoding="utf-8") as f:
    json.dump({"format": "mindmaze-questions-1", "note": "answers[0] is the correct answer; area 0-8, level 0-3",
               "areas": AREAS, "questions": questions}, f, ensure_ascii=False, indent=1)
mmdb.encode(qjson, os.path.join(out, "mindmaze.db"), os.path.join(out, "mindmaze.idx"))

# ---- "another disc" for Settings > Add questions from a disc (CHANGE 2026-11-18): the first 100 questions again
# (duplicates, left out when loading) and 50 new ones; the test platform copies it into editions/fx/ ----
disc = [dict(q) for q in questions[:100]]
for k in range(50):
    disc.append({"id": 1000 + k, "area": k % 9, "level": k % 4, "question": "Disc-only fixture question %d?" % k,
                 "answers": [{"text": "Right d%d" % k, "article": 0, "value": 0}] +
                            [{"text": "Wrong %s d%d" % (c, k), "article": 0, "value": 0} for c in "ABC"]})
os.makedirs(os.path.join(out, "discfx"), exist_ok=True)
djson = os.path.join(out, "discfx", "questions.json")
with open(djson, "w", encoding="utf-8") as f:
    json.dump({"format": "mindmaze-questions-1", "areas": AREAS, "questions": disc}, f, ensure_ascii=False, indent=1)
mmdb.encode(djson, os.path.join(out, "discfx", "mindmaze.db"), os.path.join(out, "discfx", "mindmaze.idx"))

# ---- sounds: short 16-bit PCM beeps, 22050 Hz mono; music: short MIDI files ----
for i, name in enumerate(SOUNDS):
    with wave.open(os.path.join(out, name.lower()), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(22050)
        frames = 2205 + 200 * i
        w.writeframes(b"".join(struct.pack("<h", int(6000 * math.sin(k * (300 + 40 * i) * 2 * math.pi / 22050) *
                                                     (1 - k / frames))) for k in range(frames)))
for i, name in enumerate(MUSIC):
    track = b""
    for k in range(8):                                        # eight notes, 480 ticks each
        note = 60 + (i * 3 + k * (2 + i)) % 12
        track += bytes([0, 0x90, note, 80]) + bytes([0x83, 0x60, 0x80, note, 0])
    track += bytes([0, 0xFF, 0x2F, 0])
    with open(os.path.join(out, name.lower()), "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(track)) + track)

with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump({"format": "mindmaze-data-1", "source": "synthetic test fixture"}, f, indent=1)
print("fixtures: %d objects, %d questions -> %s" % (len(objects), len(questions), out))
