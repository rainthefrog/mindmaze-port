"""Tests of tools/extract/extract.py on synthetic inputs (no Encarta files, no 7-Zip needed).

  python3 tests/test_extract.py

Covers the parts written for this project: the MS ADPCM decoder, the PE section / resource reader (bitmaps,
string table), the table reader, the question file round trip, and the input checks.
"""
import json
import math
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "extract"))
import extract  # noqa: E402
import mmdb  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL: " + what)


# ---- MS ADPCM: encode a tone with a small encoder that tracks the decoder state, then decode ----
COEFS = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]
ADAPT = (230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230)


def trunc_div256(v):
    return v >> 8 if v >= 0 else -(-v >> 8)


def adpcm_encode(samples, block_align=64):
    per_block = 2 + (block_align - 7) * 2
    body, expect = b"", []
    for start in range(0, len(samples), per_block):
        blk = samples[start:start + per_block]
        blk = blk + [blk[-1]] * (per_block - len(blk))
        c1, c2 = COEFS[1]
        s2, s1, delta = blk[0], blk[1], 64
        out = bytes([1]) + struct.pack("<hhh", delta, s1, s2)
        expect += [s2, s1]
        nibs = []
        for target in blk[2:]:
            pred = trunc_div256(s1 * c1 + s2 * c2)
            code = max(-8, min(7, int(round((target - pred) / delta))))
            v = max(-32768, min(32767, pred + code * delta))
            s2, s1 = s1, v
            nib = code & 15
            delta = max(16, (ADAPT[nib] * delta) >> 8)
            nibs.append(nib)
            expect.append(v)
        out += bytes((nibs[i] << 4) | nibs[i + 1] for i in range(0, len(nibs), 2))
        body += out
    fmt = struct.pack("<HHIIHHHHH", 2, 1, 22050, 22050 * block_align // per_block, block_align, 4, 32, per_block,
                      len(COEFS)) + b"".join(struct.pack("<hh", *c) for c in COEFS)
    riff = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"fact" + struct.pack("<II", 4, len(samples)) + \
        b"data" + struct.pack("<I", len(body)) + body
    return b"RIFF" + struct.pack("<I", len(riff)) + riff, expect[:len(samples)]


tone = [int(9000 * math.sin(i * 2 * math.pi * 440 / 22050)) for i in range(1000)]
wav, expect = adpcm_encode(tone)
channels, rate, pcm = extract.decode_ms_adpcm(wav, "tone.wav")
got = list(struct.unpack("<%dh" % (len(pcm) // 2), pcm))
check((channels, rate) == (1, 22050), "ADPCM: format")
check(len(got) == 1000, "ADPCM: length follows the fact chunk (%d)" % len(got))
check(got == expect, "ADPCM: decoded samples equal the encoder's reconstruction")
err = max(abs(a - b) for a, b in zip(got, tone))
check(err < 600, "ADPCM: decoded tone is close to the source (max error %d)" % err)
out = extract.pcm_wav(channels, rate, pcm)
check(extract.decode_ms_adpcm(out)[2] == pcm, "PCM WAVE written and read back")
try:
    extract.decode_ms_adpcm(b"RIFF\0\0\0\0WAVEjunk", "bad.wav")
    check(False, "ADPCM: a broken file is refused")
except extract.ExtractError:
    pass


# ---- a synthetic 32-bit PE with a .data section (tables) and a .rsrc section (bitmap + strings) ----
BASE, DATA_RVA, RSRC_RVA = 0x400000, 0x1000, 0x3000


def build_pe(data, rsrc):
    def pad(b, n):
        return b + bytes(-len(b) % n)
    data, rsrc = pad(data, 0x200), pad(rsrc, 0x200)
    opt = bytearray(224)
    struct.pack_into("<H", opt, 0, 0x10b)
    struct.pack_into("<I", opt, 28, BASE)
    struct.pack_into("<I", opt, 92, 16)
    struct.pack_into("<II", opt, 96 + 16, RSRC_RVA, len(rsrc))
    head = bytearray(64)
    head[:2] = b"MZ"
    struct.pack_into("<I", head, 0x3c, 64)
    coff = struct.pack("<HHIIIHH", 0x14c, 2, 0, 0, 0, 224, 0x102)
    first = 0x400
    secs = struct.pack("<8sIIIIIIHHI", b".data", len(data), DATA_RVA, len(data), first, 0, 0, 0, 0, 0) + \
        struct.pack("<8sIIIIIIHHI", b".rsrc", len(rsrc), RSRC_RVA, len(rsrc), first + len(data), 0, 0, 0, 0, 0)
    return pad(bytes(head) + b"PE\0\0" + coff + bytes(opt) + secs, first) + data + rsrc


class Blob:
    """A section being built: add() returns the virtual address of what was added."""
    def __init__(self, rva):
        self.rva, self.b = rva, bytearray()

    def add(self, raw):
        va = BASE + self.rva + len(self.b)
        self.b += raw + bytes(-len(raw) % 4)
        return va

    def text(self, s):
        return self.add(s.encode("cp1252") + b"\0")


d = Blob(DATA_RVA)
counts = [5, 2, 1, 1, 3, 4]
objects = []
for i in range(sum(counts)):
    objects.append({"name": "OBJ%d.DIB" % i, "x": i * 7 - 3, "y": 300 - i, "rect": [i, i + 1, i + 40, i + 50],
                    "half": i % 2, "slots": [(i >> b) & 1 for b in range(7)], "flags": i % 4, "id": i + 2})
name_ptrs = [d.text(o["name"]) for o in objects]
recs = b""
for o, p in zip(objects, name_ptrs):
    r = bytearray(0x38)
    struct.pack_into("<Iii", r, 0, p, o["x"], o["y"])
    struct.pack_into("<iiiii", r, 0x14, *o["rect"], o["half"])
    r[0x28:0x2f] = bytes(o["slots"])
    r[0x30] = o["flags"]
    struct.pack_into("<H", r, 0x32, o["id"])
    recs += r
table = d.add(recs)
starts = [table + 0x38 * sum(counts[:g]) for g in range(6)]
addr = {"group_ptrs": d.add(struct.pack("<6I", *starts)), "group_counts": d.add(struct.pack("<6I", *counts))}
ranks = [{"name": "Rank %d" % i, "image": "RANK%d.DIB" % i, "min": i * 100, "max": i * 100 + 99} for i in range(6)]
addr["ranks"] = d.add(b"".join(struct.pack("<IIII", d.text(r["name"]), d.text(r["image"]), r["min"], r["max"])
                               for r in ranks))
speech = [["Text %d.%d with a dash — and an accent é" % (k, j) for j in range(k % 3)] for k in range(23)]
lists = [d.add(b"".join(struct.pack("<I", d.text(t)) for t in texts)) if texts else 0 for texts in speech]
addr["speech"] = d.add(struct.pack("<23I", *lists))
addr["speech_counts"] = d.add(struct.pack("<23I", *[len(t) for t in speech]))
areas, music, sounds = ["Area %d" % i for i in range(9)], ["M%d.MID" % i for i in range(5)], ["S%d.WAV" % i for i in range(35)]
addr["picture_areas"] = d.add(struct.pack("<9I", *[d.text(s) for s in areas]))
addr["music"] = d.add(struct.pack("<5I", *[d.text(s) for s in music]))
addr["sounds"] = d.add(struct.pack("<35I", *[d.text(s) for s in sounds]))

# resources: BITMAP 29150 (4x2, 8-bit, 2 colours) and STRING block 1814 (ids 29008..29023), language 0x409
dib = struct.pack("<IiiHHIIiiII", 40, 4, 2, 1, 8, 0, 8, 0, 0, 2, 0) + bytes([0, 0, 0, 0, 255, 255, 255, 0]) + bytes(8)
texts = {29009: "Synthetic Maze", 29010: "Line one\nLine two", 29023: "Last — one"}
block = b""
for sid in range(29008, 29024):
    t = texts.get(sid, "")
    block += struct.pack("<H", len(t)) + t.encode("utf-16-le")


def res_dir(entries, sub):
    """One directory level: entries = [(id, payload offset)], sub = 1 if they point at directories."""
    return struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(entries)) + \
        b"".join(struct.pack("<II", i, off | (0x80000000 if sub else 0)) for i, off in entries)


# layout: root(2 entries) | type dirs | name dirs | lang dirs | data entries | data
L = 16 + 8                                           # a directory with one entry
root_len = 16 + 16
off_t = [root_len, root_len + L]
off_n = [root_len + 2 * L, root_len + 3 * L]
off_e = [root_len + 4 * L, root_len + 4 * L + 16]
off_d = [root_len + 4 * L + 32, root_len + 4 * L + 32 + len(dib)]
rsrc = res_dir([(2, off_t[0]), (6, off_t[1])], 1) + \
    res_dir([(29150, off_n[0])], 1) + res_dir([(1814, off_n[1])], 1) + \
    res_dir([(0x409, off_e[0])], 0) + res_dir([(0x409, off_e[1])], 0) + \
    struct.pack("<IIII", RSRC_RVA + off_d[0], len(dib), 0, 0) + struct.pack("<IIII", RSRC_RVA + off_d[1], len(block), 0, 0) + \
    dib + block

pe = extract.PE(build_pe(bytes(d.b), rsrc), "synthetic.exe")
t = extract.read_tables(pe, addr)
check(t["objects"] == objects, "tables: room object records")
check(list(t["groups"].values()) == counts and list(t["groups"])[4] == "characters", "tables: groups")
check(t["ranks"] == ranks and t["speech"] == speech, "tables: ranks and speech texts (cp1252 decoded)")
check(t["picture_areas"] == areas and t["music"] == music and t["sounds"] == sounds, "tables: name lists")
bad = dict(addr, group_counts=addr["ranks"])
try:
    extract.read_tables(pe, bad)
    check(False, "tables: wrong addresses are noticed")
except extract.ExtractError:
    pass
try:
    pe.u32(BASE + 0x900000)
    check(False, "PE: an address outside the file is refused")
except extract.ExtractError:
    pass

bitmaps = pe.resources(extract.RT_BITMAP)
check(list(bitmaps) == [29150] and bitmaps[29150][0x409] == dib, "resources: BITMAP found")
bmp = extract.bitmap_file(bitmaps[29150][0x409])
check(bmp[:2] == b"BM" and struct.unpack_from("<I", bmp, 2)[0] == len(bmp) and struct.unpack_from("<I", bmp, 10)[0] == 14 + 40 + 8,
      "resources: .bmp file header (size, pixel offset)")
check(extract.string_table(pe, range(29002, 29070)) == texts, "resources: string table (empty strings left out)")
try:
    extract.PE(b"not a program", "x")
    check(False, "PE: a non-PE file is refused")
except extract.ExtractError:
    pass

# ---- questions: JSON -> DB/IDX -> JSON round trip ----
with tempfile.TemporaryDirectory() as tmp:
    qs = [{"id": i + 1, "area": i % 9, "level": i % 4, "question": "Question %d — café?" % i,
           "answers": [{"text": "A%d-%d" % (i, k), "article": 1000 + i, "value": k} for k in range(4)]} for i in range(50)]
    p = lambda n: os.path.join(tmp, n)  # noqa: E731
    json.dump({"questions": qs}, open(p("in.json"), "w", encoding="utf-8"))
    mmdb.encode(p("in.json"), p("q.db"), p("q.idx"))
    mmdb.decode(p("q.db"), p("q.idx"), p("out.json"))
    check(json.load(open(p("out.json"), encoding="utf-8"))["questions"] == qs, "questions: round trip")

    # ---- input checks: files are found by name in any letter case; unknown or missing files are refused ----
    os.makedirs(p("disc/ee/Encarta"))
    for name in extract.FILES:
        with open(p("disc/ee/Encarta/" + name.lower()), "wb") as f:
            f.write(b"not the real " + name.encode())
    found = extract.locate_in_folder(p("disc"))
    check(sorted(found) == sorted(extract.FILES), "inputs: found below a disc folder, any letter case")
    try:
        extract.check_inputs(found, False)
        check(False, "inputs: an unknown ENCARTA.EXE is refused")
    except extract.ExtractError as e:
        check("not a known build" in str(e), "inputs: message for an unknown ENCARTA.EXE")
    os.remove(p("disc/ee/Encarta/mindmaze.eit"))
    try:
        extract.check_inputs(extract.locate_in_folder(p("disc")), False)
        check(False, "inputs: a missing file is reported")
    except extract.ExtractError as e:
        check("MINDMAZE.EIT" in str(e), "inputs: the missing file is named")

print("extract tests: %s" % ("%d FAILED" % fails if fails else "all passed"))
sys.exit(1 if fails else 0)
