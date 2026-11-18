#!/usr/bin/env python3
"""Build the game's data folder from your own Microsoft Encarta 2006 disc.

  python3 tools/extract/extract.py <source> [-o data]

<source> is the disc image (.iso), a mounted disc / drive, or a folder that holds the five files
(ENCARTA.EXE, ENCARTAR.DLL, MINDMAZE.DB, MINDMAZE.IDX, MINDMAZE.EIT; on the disc they are in EE/ENCARTA).

Needs Python 3.8+ (standard library only) and 7-Zip (7z / 7zz on the PATH, or --7z <path>), which unpacks the
ISO image and the MINDMAZE.EIT archive.

What is written to the output folder (docs/FORMATS.md has the details):
  *.dib          pictures, as stored in MINDMAZE.EIT (8-bit BMP, palette index 254 = transparent)
  *.wav          sound effects, decoded from MS ADPCM to 16-bit PCM
  *.mid          music
  res/<id>.bmp   panel buttons, from the BITMAP resources of ENCARTAR.DLL
  mindmaze.db, mindmaze.idx, questions.json    the questions (binary as on the disc, and as JSON)
  tables.json    room objects, ranks, character texts, file names (ENCARTA.EXE) and UI texts (ENCARTAR.DLL)
  packs/         the open question packs of this repository, with their index
  manifest.json  what was read (file hashes) and which version of this tool wrote the folder

Every input file is checked against the SHA-256 of the reference disc (Encarta 2006 Premium, US English).
An unknown ENCARTA.EXE is refused, because the table addresses are only known for that build.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mmdb  # noqa: E402

TOOL_VERSION = "1.0"

# ---- reference files (Encarta 2006 Premium, US English; README "Supported disc") ----
REFERENCE_ISO_SHA256 = "2eae50a9c76c8ff5d2856fc8679ca045ad610662392772643d1c6e6811be08e8"
FILES = {  # name: (bytes, sha256)
    "ENCARTA.EXE": (2992848, "089adc5db30653e4af2d8a37072052c0bddc80049f9943e25b8d41d95905045f"),
    "ENCARTAR.DLL": (7723728, "5d1705d02996d94b973c634503ce63135aa77ad595e685a29dfd4416857708ef"),
    "MINDMAZE.DB": (1468278, "a28de3741a8cd16ed3959556470437d64d94b190fd79508146f0d60b87913a7e"),
    "MINDMAZE.IDX": (64304, "0f16c38bae3ae23d6ca6a3d10eb4b110e1a39da1765c49f9dce34a4606aa04ba"),
    "MINDMAZE.EIT": (2924179, "b7e3ccec82048f57fc0a7f887569fcc1dce737cd58c113726394ea5287f8fc0e"),
}
# folders on the disc, in order of preference (EE/KIDS has copies of the program files)
DISC_DIRS = ["EE/ENCARTA", "EE/KIDS"]

# Where MindMaze's tables are in each known ENCARTA.EXE (virtual addresses), keyed by the file's SHA-256.
# record layouts: see read_tables().
KNOWN_EXE = {
    "089adc5db30653e4af2d8a37072052c0bddc80049f9943e25b8d41d95905045f": {
        "name": "Encarta 2006 Premium (US English)",
        "group_ptrs": 0x6b54a0,     # u32[6]: first record of doors, floors, ceilings, walls, characters, furniture
        "group_counts": 0x6b54b8,   # u32[6]
        "ranks": 0x6b54d0,          # 6 x {name*, picture*, u32 min, u32 max}
        "speech": 0x6b5880,         # u32[23]: text pointer lists for object ids 7..29
        "speech_counts": 0x6b58e0,  # u32[23]
        "picture_areas": 0x6b425c,  # char*[9]: subject of the wall pictures, ids 31..39
        "music": 0x6b41bc,          # char*[5]
        "sounds": 0x6b41d0,         # char*[35]
    },
}
OBJECT_SIZE = 0x38
STRING_IDS = range(29002, 29070)                         # MindMaze's part of the ENCARTAR.DLL string table
BITMAP_IDS = list(range(29150, 29165)) + [29166]         # panel buttons and the seal
RT_BITMAP, RT_STRING = 2, 6


class ExtractError(Exception):
    pass


def log(msg):
    print(msg, flush=True)


def sha256_file(path, progress=None):
    h = hashlib.sha256()
    done = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 22)
            if not chunk:
                break
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done)
    return h.hexdigest()


# ------------------------------------------------------------------------------------------------
# 7-Zip
def find_7z(explicit=None):
    names = [explicit] if explicit else ["7z", "7zz", "7za", "7zzs"]
    for n in names:
        p = shutil.which(n) or (n if os.path.isfile(n) else None)
        if p:
            return p
    if not explicit:
        for env in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"):
            p = os.path.join(os.environ.get(env, ""), "7-Zip", "7z.exe")
            if os.path.isfile(p):
                return p
    raise ExtractError("7-Zip was not found. Install it (https://www.7-zip.org, or the package 7zip / p7zip-full) "
                       "or give its path with --7z.")


def run_7z(sevenzip, args):
    r = subprocess.run([sevenzip] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def files_under(folder):
    """{upper-case relative path with '/': real path} of the regular files below folder (links are skipped)."""
    out = {}
    for root, dirs, names in os.walk(folder):
        dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(root, d))]
        for n in names:
            p = os.path.join(root, n)
            if os.path.isfile(p) and not os.path.islink(p):
                out[os.path.relpath(p, folder).replace(os.sep, "/").upper()] = p
    return out


# ------------------------------------------------------------------------------------------------
# step 1: the five input files
def locate_in_folder(folder):
    found = {}
    files = files_under(folder)
    for name in FILES:
        cands = [d + "/" + name for d in DISC_DIRS] + [name]
        hit = next((files[c] for c in cands if c in files), None)
        if not hit:                                      # anywhere below, e.g. a copied ENCARTA folder
            hit = next((p for rel, p in sorted(files.items()) if rel.rsplit("/", 1)[-1] == name), None)
        if hit:
            found[name] = hit
    return found


def locate_in_iso(sevenzip, iso, work):
    dest = os.path.join(work, "iso")
    wanted = [d + "/" + n for d in DISC_DIRS for n in FILES]
    code, text = run_7z(sevenzip, ["x", "-y", "-ssc-", "-o" + dest, iso] + wanted)
    if code > 1 and not os.path.isdir(dest):             # 1 = warnings
        raise ExtractError("7-Zip could not read %s:\n%s" % (iso, text[-800:]))
    return locate_in_folder(dest) if os.path.isdir(dest) else {}


def check_inputs(found, strict):
    """Hash the inputs. Returns {name: {path, bytes, sha256, reference}}; raises on a missing file."""
    missing = [n for n in FILES if n not in found]
    if missing:
        raise ExtractError("Not found in the source: %s.\nOn the Encarta 2006 disc these files are in EE/ENCARTA."
                           % ", ".join(missing))
    info, differing = {}, []
    for name, path in found.items():
        size, ref = FILES[name]
        digest = sha256_file(path)
        ok = digest == ref
        info[name] = {"bytes": os.path.getsize(path), "sha256": digest, "reference": ok, "path": path}
        log("  %-13s %9d bytes  %s" % (name, info[name]["bytes"], "matches the reference disc" if ok else "DIFFERS"))
        if not ok:
            differing.append("%s: %d bytes, sha256 %s (reference: %d bytes, %s)" %
                             (name, info[name]["bytes"], digest, size, ref))
    if info["ENCARTA.EXE"]["sha256"] not in KNOWN_EXE:
        raise ExtractError(
            "This ENCARTA.EXE is not a known build:\n  %s\nThe room and text tables are read from fixed addresses "
            "that are only known for Encarta 2006 Premium (US English). Other editions, languages and updated "
            "copies are not supported yet; please open an issue with the lines above." % differing[0])
    if differing:
        msg = "These files differ from the reference disc:\n  " + "\n  ".join(differing)
        if strict:
            raise ExtractError(msg)
        log("WARNING: " + msg + "\n  Continuing; their contents are checked while they are read.")
    return info


# ------------------------------------------------------------------------------------------------
# step 2-3: MINDMAZE.EIT (pictures, sounds, music)
def decode_ms_adpcm(data, name="wav"):
    """A RIFF/WAVE file (MS ADPCM or already PCM) -> (channels, rate, 16-bit PCM bytes)."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ExtractError("%s: not a WAVE file" % name)
    fmt = fact = body = None
    pos = 12
    while pos + 8 <= len(data):
        cid, n = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        chunk = data[pos + 8:pos + 8 + n]
        if cid == b"fmt ":
            fmt = chunk
        elif cid == b"fact" and n >= 4:
            fact = struct.unpack_from("<I", chunk)[0]
        elif cid == b"data":
            body = chunk
        pos += 8 + n + (n & 1)
    if fmt is None or body is None or len(fmt) < 16:
        raise ExtractError("%s: fmt or data chunk missing" % name)
    tag, channels, rate, _, block_align, bits = struct.unpack_from("<HHIIHH", fmt)
    if tag == 1 and bits == 16:
        return channels, rate, bytes(body)
    if tag != 2 or channels not in (1, 2) or len(fmt) < 22:
        raise ExtractError("%s: unsupported WAVE format tag %d" % (name, tag))
    samples_per_block, ncoef = struct.unpack_from("<HH", fmt, 18)
    if len(fmt) < 22 + 4 * ncoef or ncoef < 1 or block_align < 7 * channels:
        raise ExtractError("%s: bad MS ADPCM header" % name)
    coefs = [struct.unpack_from("<hh", fmt, 22 + 4 * i) for i in range(ncoef)]
    adapt = (230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230)

    out = []                                             # interleaved samples
    for start in range(0, len(body), block_align):
        block = body[start:start + block_align]
        if len(block) < 7 * channels:
            break
        p = 0
        pred = []
        for _ in range(channels):
            if block[p] >= ncoef:
                raise ExtractError("%s: bad predictor index" % name)
            pred.append(coefs[block[p]])
            p += 1
        delta = list(struct.unpack_from("<%dh" % channels, block, p))
        p += 2 * channels
        s1 = list(struct.unpack_from("<%dh" % channels, block, p))
        p += 2 * channels
        s2 = list(struct.unpack_from("<%dh" % channels, block, p))
        p += 2 * channels
        out.extend(s2)
        out.extend(s1)
        ch = 0
        for byte in block[p:]:
            for nib in (byte >> 4, byte & 15):
                signed = nib - 16 if nib & 8 else nib
                # prediction / 256 rounded toward zero, as ffmpeg decodes it (an arithmetic shift, which
                # rounds down, gives samples that drift by up to 1% from ffmpeg's)
                v = s1[ch] * pred[ch][0] + s2[ch] * pred[ch][1]
                v = (v >> 8 if v >= 0 else -(-v >> 8)) + signed * delta[ch]
                v = max(-32768, min(32767, v))
                s2[ch], s1[ch] = s1[ch], v
                delta[ch] = max(16, (adapt[nib] * delta[ch]) >> 8)
                out.append(v)
                ch = (ch + 1) % channels
    frames = len(out) // channels
    if fact is not None and 0 < fact <= frames:          # the fact chunk has the true length; the last block is padded
        frames = fact
    return channels, rate, struct.pack("<%dh" % (frames * channels), *out[:frames * channels])


def pcm_wav(channels, rate, pcm):
    return (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * 2, channels * 2, 16) +
            b"data" + struct.pack("<I", len(pcm)) + pcm)


def unpack_eit(sevenzip, eit, work):
    dest = os.path.join(work, "eit")
    code, text = run_7z(sevenzip, ["x", "-y", "-o" + dest, eit])
    if code > 1 or not os.path.isdir(dest):
        code, text = run_7z(sevenzip, ["x", "-y", "-thxs", "-o" + dest, eit])
    if code > 1 or not os.path.isdir(dest):
        raise ExtractError("7-Zip could not unpack MINDMAZE.EIT (an MS Help 2 / ITOL archive; 7-Zip 9.20 or newer "
                           "reads it):\n%s" % text[-800:])
    return dest


def write_baggage(eit_dir, out):
    """Pictures, sounds and music from the unpacked archive's baggage folder. Returns counts per kind."""
    files = {rel: p for rel, p in files_under(eit_dir).items() if rel.startswith("BAGGAGE/") and rel.count("/") == 1}
    counts = {"dib": 0, "wav": 0, "mid": 0}
    for rel, path in sorted(files.items()):
        name = rel.split("/", 1)[1].lower()
        ext = name.rsplit(".", 1)[-1]
        if ext not in counts or not all(c.isalnum() or c in "-_." for c in name):
            continue
        with open(path, "rb") as f:
            data = f.read()
        if ext == "dib" and data[:2] != b"BM":
            raise ExtractError("%s: not a bitmap" % name)
        if ext == "mid" and data[:4] != b"MThd":
            raise ExtractError("%s: not a MIDI file" % name)
        if ext == "wav":
            channels, rate, pcm = decode_ms_adpcm(data, name)
            if (channels, rate) != (1, 22050):
                log("WARNING: %s is %d Hz with %d channel(s); the game expects 22050 Hz mono" % (name, rate, channels))
            data = pcm_wav(channels, rate, pcm)
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        counts[ext] += 1
    if not counts["dib"] or not counts["wav"]:
        raise ExtractError("MINDMAZE.EIT holds no baggage/*.dib or *.wav files")
    return counts


# ------------------------------------------------------------------------------------------------
# step 4: PE files (sections, resources)
class PE:
    def __init__(self, data, name="PE file"):
        self.d, self.name = data, name
        if data[:2] != b"MZ":
            raise ExtractError("%s: not a Windows program file" % name)
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ExtractError("%s: PE header missing" % name)
        nsec, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != 0x10b:
            raise ExtractError("%s: not a 32-bit program" % name)
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        ndir = struct.unpack_from("<I", data, opt + 92)[0]
        self.rsrc_rva = struct.unpack_from("<I", data, opt + 96 + 8 * 2)[0] if ndir > 2 else 0
        self.sections = []                               # (rva, virtual size, file offset, raw size)
        for i in range(nsec):
            s = opt + opt_size + 40 * i
            vsize, rva, rsize, roff = struct.unpack_from("<IIII", data, s + 8)
            self.sections.append((rva, vsize, roff, rsize))

    def off(self, rva):
        for sec_rva, vsize, roff, rsize in self.sections:
            if sec_rva <= rva < sec_rva + max(vsize, rsize):
                if rva - sec_rva >= rsize:
                    break
                return roff + rva - sec_rva
        raise ExtractError("%s: address 0x%x is not in the file" % (self.name, rva + self.image_base))

    # --- by virtual address (tables in the EXE)
    def u32(self, va):
        return struct.unpack_from("<I", self.d, self.off(va - self.image_base))[0]

    def bytes_at(self, va, n):
        o = self.off(va - self.image_base)
        return self.d[o:o + n]

    def cstr(self, va):
        o = self.off(va - self.image_base)
        end = self.d.index(b"\0", o)
        if end - o > 20000:
            raise ExtractError("%s: unterminated text at 0x%x" % (self.name, va))
        return self.d[o:end].decode("cp1252")

    # --- resources
    def _dir(self, offset):
        """Entries of a resource directory: [(id or name, is_dir, offset)]."""
        base = self.off(self.rsrc_rva)
        named, ids = struct.unpack_from("<HH", self.d, base + offset + 12)
        out = []
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", self.d, base + offset + 16 + 8 * i)
            if name & 0x80000000:
                o = base + (name & 0x7fffffff)
                n = struct.unpack_from("<H", self.d, o)[0]
                name = self.d[o + 2:o + 2 + 2 * n].decode("utf-16-le")
            out.append((name, bool(target & 0x80000000), target & 0x7fffffff))
        return out

    def resources(self, rtype):
        """{id: {language: bytes}} for one resource type."""
        if not self.rsrc_rva:
            raise ExtractError("%s: no resources" % self.name)
        out = {}
        for tname, tdir, toff in self._dir(0):
            if tname != rtype or not tdir:
                continue
            for rid, rdir, roff in self._dir(toff):
                if not rdir:
                    continue
                for lang, ldir, loff in self._dir(roff):
                    if ldir:
                        continue
                    rva, size = struct.unpack_from("<II", self.d, self.off(self.rsrc_rva) + loff)
                    o = self.off(rva)
                    out.setdefault(rid, {})[lang] = self.d[o:o + size]
        return out


def pick_language(by_lang):
    """US English when present, otherwise the first language."""
    return by_lang.get(0x409) or by_lang[sorted(by_lang, key=str)[0]]


def bitmap_file(dib):
    """A BITMAP resource (a DIB without file header) as a .bmp file."""
    size, width, height, planes, bpp, compression = struct.unpack_from("<IiiHHI", dib)
    if size < 40 or planes != 1:
        raise ExtractError("unsupported bitmap resource header")
    colours = struct.unpack_from("<I", dib, 32)[0]
    if not colours and bpp <= 8:
        colours = 1 << bpp
    offset = 14 + size + 4 * colours + (12 if compression == 3 and size == 40 else 0)
    return struct.pack("<2sIHHI", b"BM", 14 + len(dib), 0, 0, offset) + dib


def string_table(pe, ids):
    """{id: text} for the non-empty strings among ids (RT_STRING blocks hold 16 strings each)."""
    blocks = pe.resources(RT_STRING)
    out = {}
    for block_id in sorted({(i >> 4) + 1 for i in ids}):
        if block_id not in blocks:
            continue
        data = pick_language(blocks[block_id])
        pos = 0
        for k in range(16):
            if pos + 2 > len(data):
                break
            n = struct.unpack_from("<H", data, pos)[0]
            text = data[pos + 2:pos + 2 + 2 * n].decode("utf-16-le")
            pos += 2 + 2 * n
            sid = (block_id - 1) * 16 + k
            if n and sid in ids:
                out[sid] = text
    return out


def write_resources(dll, out):
    pe = PE(dll, "ENCARTAR.DLL")
    bitmaps = pe.resources(RT_BITMAP)
    os.makedirs(os.path.join(out, "res"), exist_ok=True)
    for rid in BITMAP_IDS:
        if rid not in bitmaps:
            raise ExtractError("ENCARTAR.DLL: BITMAP resource %d is missing" % rid)
        with open(os.path.join(out, "res", "%d.bmp" % rid), "wb") as f:
            f.write(bitmap_file(pick_language(bitmaps[rid])))
    strings = string_table(pe, STRING_IDS)
    for needed in (29003, 29034, 29040, 29041, 29044):
        if needed not in strings:
            raise ExtractError("ENCARTAR.DLL: string %d is missing (MindMaze's texts are ids 29002-29069)" % needed)
    return strings


# ------------------------------------------------------------------------------------------------
# step 5: tables from ENCARTA.EXE
def read_tables(pe, addr):
    """MindMaze's data tables as the tables.json dictionary (without the UI strings).

    Room object record, 0x38 bytes: +0 picture name*, +4 x, +8 y, +0x14 click rect l,t,r,b (relative to x,y),
    +0x24 half flag, +0x28 seven placement slots (bytes), +0x30 flags, +0x32 u16 object id.
    """
    group_names = ["doors", "floors", "ceilings", "walls", "characters", "furniture"]
    counts = [pe.u32(addr["group_counts"] + 4 * i) for i in range(6)]
    ptrs = [pe.u32(addr["group_ptrs"] + 4 * i) for i in range(6)]
    if not all(0 < c <= 200 for c in counts):
        raise ExtractError("ENCARTA.EXE: implausible room object counts %r" % counts)
    for g in range(1, 6):
        if ptrs[g] != ptrs[g - 1] + counts[g - 1] * OBJECT_SIZE:
            raise ExtractError("ENCARTA.EXE: the room object groups are not one table")
    objects = []
    for i in range(sum(counts)):
        rec = pe.bytes_at(ptrs[0] + i * OBJECT_SIZE, OBJECT_SIZE)
        x, y = struct.unpack_from("<ii", rec, 4)
        rect = list(struct.unpack_from("<iiii", rec, 0x14))
        objects.append({"name": pe.cstr(struct.unpack_from("<I", rec, 0)[0]), "x": x, "y": y, "rect": rect,
                        "half": struct.unpack_from("<i", rec, 0x24)[0], "slots": list(rec[0x28:0x2f]),
                        "flags": rec[0x30], "id": struct.unpack_from("<H", rec, 0x32)[0]})
    ranks = []
    for i in range(6):
        va = addr["ranks"] + 16 * i
        ranks.append({"name": pe.cstr(pe.u32(va)), "image": pe.cstr(pe.u32(va + 4)),
                      "min": pe.u32(va + 8), "max": pe.u32(va + 12)})
    speech = []
    for k in range(23):
        p, n = pe.u32(addr["speech"] + 4 * k), pe.u32(addr["speech_counts"] + 4 * k)
        if n > 50:
            raise ExtractError("ENCARTA.EXE: implausible speech text count")
        speech.append([pe.cstr(pe.u32(p + 4 * j)) for j in range(n)] if p else [])
    return {
        "format": "mindmaze-tables-1",
        "groups": dict(zip(group_names, counts)),
        "objects": objects,
        "ranks": ranks,
        "speech": speech,
        "picture_areas": [pe.cstr(pe.u32(addr["picture_areas"] + 4 * i)) for i in range(9)],
        "music": [pe.cstr(pe.u32(addr["music"] + 4 * i)) for i in range(5)],
        "sounds": [pe.cstr(pe.u32(addr["sounds"] + 4 * i)) for i in range(35)],
    }


def check_tables(tables, out):
    """Every picture, sound and music file the tables name must have been extracted."""
    names = [o["name"] for o in tables["objects"]] + [r["image"] for r in tables["ranks"]] + \
        tables["music"] + tables["sounds"]
    missing = sorted({n for n in names if not os.path.isfile(os.path.join(out, n.lower()))})
    if missing:
        raise ExtractError("Named in ENCARTA.EXE but not found in MINDMAZE.EIT: " + ", ".join(missing))


# ------------------------------------------------------------------------------------------------
def install_packs(out):
    """Copy the repository's open question packs (packs/*.json) and write packs/index.txt for the settings screen."""
    src = os.path.join(HERE, "..", "..", "packs")
    if not os.path.isdir(src):
        return 0
    dest = os.path.join(out, "packs")
    os.makedirs(dest, exist_ok=True)
    rows = []
    for name in sorted(os.listdir(src)):
        if name == "LICENSES.md":
            shutil.copyfile(os.path.join(src, name), os.path.join(dest, name))
        if not name.endswith(".json"):
            continue
        shutil.copyfile(os.path.join(src, name), os.path.join(dest, name))
        if name == "online_map.json":
            continue
        with open(os.path.join(src, name), encoding="utf-8") as f:
            d = json.load(f)
        h = d.get("pack", {})
        pid = h.get("id") or os.path.splitext(name)[0]
        rows.append("\t".join([name, pid, h.get("name", pid), h.get("license", ""), str(len(d.get("questions", [])))]))
    with open(os.path.join(dest, "index.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")
    return len(rows)


def extract(source, out, sevenzip=None, strict=False, iso_hash=True):
    source = os.path.abspath(source)
    if not os.path.exists(source):
        raise ExtractError("No such file or folder: %s" % source)
    os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="extract-", dir=out)
    try:
        sevenzip = find_7z(sevenzip)
        manifest = {"format": "mindmaze-data-1", "tool": "mindmaze-port extract.py " + TOOL_VERSION,
                    "source": {"kind": "folder"}}
        log("1/6 Reading %s" % source)
        if os.path.isdir(source):
            found = locate_in_folder(source)
        else:
            found = locate_in_iso(sevenzip, source, work)
            manifest["source"] = {"kind": "image", "bytes": os.path.getsize(source)}
            if iso_hash:
                log("  hashing the image (skip with --no-iso-hash) ...")
                digest = sha256_file(source)
                manifest["source"].update(sha256=digest, reference=digest == REFERENCE_ISO_SHA256)
                log("  image sha256 %s: %s" % (digest, "the reference image" if digest == REFERENCE_ISO_SHA256 else
                                               "not the reference image (fine if the files below match)"))
        info = check_inputs(found, strict)
        manifest["files"] = {n: {k: v for k, v in i.items() if k != "path"} for n, i in sorted(info.items())}

        log("2/6 Unpacking MINDMAZE.EIT (pictures, sounds, music)")
        counts = write_baggage(unpack_eit(sevenzip, info["MINDMAZE.EIT"]["path"], work), out)
        log("  %(dib)d pictures, %(wav)d sounds (ADPCM -> PCM), %(mid)d music files" % counts)

        log("3/6 Panel bitmaps and UI texts from ENCARTAR.DLL")
        with open(info["ENCARTAR.DLL"]["path"], "rb") as f:
            strings = write_resources(f.read(), out)
        log("  %d bitmaps, %d texts" % (len(BITMAP_IDS), len(strings)))

        log("4/6 Tables from ENCARTA.EXE")
        with open(info["ENCARTA.EXE"]["path"], "rb") as f:
            exe = f.read()
        build = KNOWN_EXE[info["ENCARTA.EXE"]["sha256"]]
        tables = read_tables(PE(exe, "ENCARTA.EXE"), build)
        tables["source"] = build["name"]
        tables["strings"] = {str(k): v for k, v in sorted(strings.items())}
        check_tables(tables, out)
        with open(os.path.join(out, "tables.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(tables, f, ensure_ascii=False, indent=1)
        log("  %d room objects, %d ranks, %d speech texts" %
            (len(tables["objects"]), len(tables["ranks"]), sum(len(s) for s in tables["speech"])))

        log("5/6 Questions")
        shutil.copyfile(info["MINDMAZE.DB"]["path"], os.path.join(out, "mindmaze.db"))
        shutil.copyfile(info["MINDMAZE.IDX"]["path"], os.path.join(out, "mindmaze.idx"))
        try:
            mmdb.decode(os.path.join(out, "mindmaze.db"), os.path.join(out, "mindmaze.idx"),
                        os.path.join(out, "questions.json"))
        except (struct.error, IndexError, UnicodeDecodeError) as e:
            raise ExtractError("MINDMAZE.DB / MINDMAZE.IDX could not be read (%s)" % e)

        log("6/6 Question packs and manifest")
        manifest["packs"] = install_packs(out)
        manifest["counts"] = dict(counts, bitmaps=len(BITMAP_IDS), strings=len(strings), objects=len(tables["objects"]))
        with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=1)
        log("Done: %s" % os.path.abspath(out))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="Build MindMaze's data folder from your own Encarta 2006 disc.")
    ap.add_argument("source", help="Encarta 2006 disc image (.iso), mounted disc, or a folder with the five files")
    ap.add_argument("-o", "--out", default="data", help="output folder (default: data)")
    ap.add_argument("--7z", dest="sevenzip", help="path of the 7-Zip program (default: found on the PATH)")
    ap.add_argument("--strict", action="store_true", help="stop if any input file differs from the reference disc")
    ap.add_argument("--no-iso-hash", action="store_true", help="do not hash the whole image (saves reading 3 GB)")
    a = ap.parse_args()
    try:
        extract(a.source, a.out, a.sevenzip, a.strict, not a.no_iso_hash)
    except ExtractError as e:
        sys.exit("ERROR: %s" % e)


if __name__ == "__main__":
    main()
