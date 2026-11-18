"""Cut Codex results (<data>/art/work/hd/out_gen/<sheet>.png) into single 2x pictures: <data>/hd/<key>.png.

  python3 -I mm_cut.py <mindmaze dir>

Steps per sheet / picture:
- The result is resized to exactly 2x its sheet (the model may return a slightly different size).
- Background key: a pixel's distance to pure magenta (255,0,255). <= KEY_IN is background, >= KEY_OUT is
  picture, in between it is a blend (un-mixed so soft edges keep their own colour). Distance, not "how
  pink", so purples, blues and shadows stay solid.
- Drawing outside the lines is KEPT: painted pixels in the clear zone that connect (8-neighbour) to a
  sprite's box belong to that sprite (a spill between two sprites goes to the nearest connection), and the
  picture grows to include them. Unconnected specks are dropped. hd/bounds.json records per picture where its
  image starts relative to the original box and its size, in 2x pixels: {name: {dx, dy (<= 0), w, h, grown}}.
- The AGENT's drawing decides the outline (CHANGE 2026-11-18; before, the original's silhouette doubled to 2x
  did, which gave a 2 px staircase): only key colour enclosed by the picture inside the original's outline
  (shrunk by 2 px), not joined to the background around it, is a hole and gets the original's colour.
- Outline cleanup (edge_* below, also used by mm_wide_cut.py): faint pink on the 2 px outline band and the soft
  edge take the colour of the clean pixels next to them, then alpha is softened along the outline.
- Colours are matched back to the original (per channel mean and contrast over the solid pixels): the model
  tends to warm everything up.
- Pictures whose original has a flat edge colour (panel buttons, award squares, Points seal) get their
  background repainted in exactly that colour, so they blend into the panel like the originals.
"""
import json
import os
import sys
from collections import deque

import numpy as np
from PIL import Image

ROOT = sys.argv[1]
CODEX = os.path.join(ROOT, "art", "work", "hd")    # map.json, out_gen/ (ROOT = the data folder)
KEY_IN, KEY_OUT = 60.0, 150.0
MAGENTA = np.array([255.0, 0.0, 255.0], np.float32)


# ---------------------------------------------------------------------------------------------- helpers
def original(name):
    """Original picture as RGBA uint8 (alpha 0 where the game treats it as transparent)."""
    if name.startswith("res/"):
        return np.asarray(Image.open(os.path.join(ROOT, "res", name[4:])).convert("RGBA"))
    raw = Image.open(os.path.join(ROOT, name))
    im = raw.convert("RGBA")
    if raw.mode == "P" and raw.getpalette()[254 * 3:254 * 3 + 3] == [0, 255, 255]:
        im.putalpha(Image.frombytes("L", raw.size, bytes(0 if v == 254 else 255 for v in raw.tobytes())))
    return np.asarray(im)


def dilate(m, r):
    out = m.copy()
    for _ in range(r):
        o = out.copy()
        o[1:] |= out[:-1]; o[:-1] |= out[1:]; o[:, 1:] |= out[:, :-1]; o[:, :-1] |= out[:, 1:]
        out = o
    return out


def erode(m, r):
    return ~dilate(~m, r)


def light_pink(rgb):
    """Pale pink / lilac: background blended with magenta at an edge (real purples are much darker)."""
    f = rgb.astype(np.int32)
    return (f[..., 0] > 150) & (f[..., 2] > 150) & (np.minimum(f[..., 0], f[..., 2]) - f[..., 1] > 30)


def tinted(rgb):
    """Magenta tint: red and blue both clearly above green (pink, lilac, purple)."""
    f = rgb.astype(np.int32)
    return np.minimum(f[..., 0], f[..., 2]) - f[..., 1] > 25


def key_alpha(rgb):
    """0..1 share of picture (not background) from the distance to magenta."""
    d = np.sqrt(((rgb.astype(np.float32) - MAGENTA) ** 2).sum(axis=2))
    return np.clip((d - KEY_IN) / (KEY_OUT - KEY_IN), 0.0, 1.0)


def unmix(rgb, a):
    f = rgb.astype(np.float32)
    safe = np.maximum(a, 1e-3)[..., None]
    col = np.clip((f - (1.0 - a)[..., None] * MAGENTA) / safe, 0, 255)
    return np.where((a >= 1.0)[..., None], f, col)


def place(o2, shape, dx, dy, fill):
    """The original (2x) placed in a picture of `shape` whose origin is (dx, dy) <= 0 relative to the box."""
    out = np.full(shape + o2.shape[2:], fill, o2.dtype)
    oy, ox = -dy, -dx
    sub = o2[: shape[0] - oy, : shape[1] - ox]
    out[oy:oy + sub.shape[0], ox:ox + sub.shape[1]] = sub
    return out


def match_colours(rgb, ref, mask):
    """Per channel: shift mean and scale contrast (limited) of rgb to the reference over mask."""
    if mask.sum() < 50:
        return rgb
    out = rgb.astype(np.float32).copy()
    for ch in range(3):
        h = out[..., ch][mask]
        o = ref[..., ch][mask].astype(np.float32)
        sh, so = h.std(), o.std()
        g = float(np.clip(so / sh, 0.85, 1.15)) if sh > 1 else 1.0
        out[..., ch] = (out[..., ch] - h.mean()) * g + o.mean()
    return np.clip(out, 0, 255)


def match_flat_background(rgb, alpha, o2rgb, o2solid, orig):
    """Repaint the HD background in the original's flat edge colour (see module doc)."""
    border = np.concatenate([orig[0, :, :3], orig[-1, :, :3], orig[:, 0, :3], orig[:, -1, :3]])
    cols, counts = np.unique(border.reshape(-1, 3), axis=0, return_counts=True)
    c0 = cols[counts.argmax()]
    if counts.max() < 0.85 * len(border) or (c0 == [0, 255, 255]).all():
        return rgb, alpha, None
    hb = np.concatenate([rgb[0], rgb[-1], rgb[:, 0], rgb[:, -1]])
    chd = np.median(hb, axis=0)
    obg = erode((o2rgb == c0).all(axis=2) & o2solid, 2)              # clear background only (blocky 2x mask)
    bg = obg | (np.abs(rgb - chd).max(axis=2) <= 30) | (key_alpha(rgb.astype(np.uint8)) < 1.0) | light_pink(rgb)
    h, w = bg.shape
    seen = np.zeros_like(bg)
    q = deque([(y, x) for y in range(h) for x in (0, w - 1) if bg[y, x]] +
              [(y, x) for x in range(w) for y in (0, h - 1) if bg[y, x]])
    for y, x in q:
        seen[y, x] = True
    while q:
        y, x = q.popleft()
        for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
            if 0 <= ny < h and 0 <= nx < w and bg[ny, nx] and not seen[ny, nx]:
                seen[ny, nx] = True
                q.append((ny, nx))
    # feathered repaint: a 2 px soft edge instead of the blocky outline of the original mask
    wgt = seen.astype(np.float32)
    for _ in range(2):
        p = np.pad(wgt, 1, mode="edge")
        wgt = (p[:-2, :-2] + p[:-2, 1:-1] + p[:-2, 2:] + p[1:-1, :-2] + p[1:-1, 1:-1] + p[1:-1, 2:] + p[2:, :-2] + p[2:, 1:-1] + p[2:, 2:]) / 9
    wgt = np.maximum(wgt, seen)
    rgb = rgb * (1 - wgt[..., None]) + np.asarray(c0, np.float32) * wgt[..., None]
    alpha = alpha.copy()
    alpha[seen & o2solid] = 1.0
    return rgb, alpha, c0


# ---------------------------------------------------------------------------------------------- outline cleanup
# CHANGE 2026-11-18 (same as edge_* in src/art/mm_art.c): the agent's edge pixels are mixed with a background that
# is not exactly pure magenta, so unmixing leaves stair steps and a pink / purple rim.
def _sum3(v, weights=None, edge=False):
    """3x3 neighbourhood sum (zero padding, or edge-extended); weights: 3x3 array."""
    p = np.pad(v, ((1, 1), (1, 1)) + ((0, 0),) * (v.ndim - 2), mode="edge" if edge else "constant")
    h, w = v.shape[:2]
    s = np.zeros(v.shape, np.float32)
    for dy in range(3):
        for dx in range(3):
            k = 1.0 if weights is None else weights[dy][dx]
            s += p[dy:dy + h, dx:dx + w] * k
    return s


def edge_band(a):
    """Solid pixels (alpha >= 0.5) within 2 px of the clear area (alpha < 0.5)."""
    return dilate(a < 0.5, 2) & (a >= 0.5)


def edge_mark(rgb, a, band):
    """1: faint pink on the band (min(R,B) - G > 8); 3: soft edge (0 < alpha < 1); 0: the rest. Pink / purple away
    from the outline is left alone (real purples)."""
    pink = np.minimum(rgb[..., 0], rgb[..., 2]) - rgb[..., 1]
    return np.where(band & (pink > 8), 1, np.where((a > 0) & (a < 1), 3, 0)).astype(np.uint8)


def edge_recolour(rgb, a, nt):
    """nt 1 / 3 pixels take the mean colour of the clean 3x3 neighbours (solid and unmarked, or recoloured in an
    earlier pass); 4 passes, each reading the state of the pass before. Recoloured pixels become nt 2."""
    rgb = rgb.astype(np.float32).copy()
    nt = nt.copy()
    for _ in range(4):
        todo = (nt == 1) | (nt == 3)
        if not todo.any():
            break
        clean = ((nt == 2) | ((nt == 0) & (a >= 1))).astype(np.float32)
        cnt = _sum3(clean)
        s = _sum3(rgb * clean[..., None])
        upd = todo & (cnt > 0)
        rgb[upd] = s[upd] / cnt[upd][:, None]
        nt[upd] = 2
    return rgb, nt


def edge_soften(a, band):
    """3x3 [1 2 1] blur of alpha on the band and the soft edge only (body solid, clear area clear)."""
    k = [[1, 2, 1], [2, 4, 2], [1, 2, 1]]
    blur = _sum3(a.astype(np.float32), k, edge=True) / 16
    return np.where(band | ((a > 0) & (a < 1)), blur, a).astype(np.float32)


def outside_key(part_rgb):
    """Key colour (key_alpha < 1) joined (4-neighbour) to the border of the part: the background around it."""
    key = key_alpha(part_rgb) < 1.0
    out = np.zeros_like(key)
    out[0], out[-1], out[:, 0], out[:, -1] = key[0], key[-1], key[:, 0], key[:, -1]
    while True:
        nxt = out
        for _ in range(16):                       # one px per step, so it never jumps over the picture
            nxt = dilate(nxt, 1) & key
        if (nxt == out).all():
            return out
        out = nxt


def owners(drawn, boxes):
    """Owner cell per pixel: inside a box = that cell; drawn pixels outside all boxes = the cell they connect
    to (multi-source BFS from the box edges, so a shared spill goes to the nearest connection); else -1."""
    h, w = drawn.shape
    own = np.full((h, w), -1, np.int32)
    for i, (x0, y0, x1, y1) in enumerate(boxes):
        own[y0:y1, x0:x1] = i
    inbox = own >= 0
    q = deque()
    for i, (x0, y0, x1, y1) in enumerate(boxes):
        for y in range(y0, y1):
            for x in (x0, x1 - 1):
                if drawn[y, x]:
                    q.append((y, x))
        for x in range(x0, x1):
            for y in (y0, y1 - 1):
                if drawn[y, x]:
                    q.append((y, x))
    while q:
        y, x = q.popleft()
        o = own[y, x]
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                ny, nx = y + dy, x + dx
                if 0 <= ny < h and 0 <= nx < w and drawn[ny, nx] and own[ny, nx] < 0 and not inbox[ny, nx]:
                    own[ny, nx] = o
                    q.append((ny, nx))
    return own


# ---------------------------------------------------------------------------------------------- per picture
def finish(part_rgb, keyed, mine, orig, dx, dy):
    """part_rgb: HD pixels of the picture's bounds; mine: pixels that belong to it; orig: original RGBA."""
    shape = part_rgb.shape[:2]
    o2 = np.repeat(np.repeat(orig, 2, 0), 2, 1)
    o2rgb = place(o2[..., :3], shape, dx, dy, 0).astype(np.float32)
    o2solid = place(o2[..., 3] > 0, shape, dx, dy, False)
    a = key_alpha(part_rgb) if keyed else np.ones(shape, np.float32)
    rgb = unmix(part_rgb, a)
    inner = erode(o2solid, 2)
    raw = key_alpha(part_rgb)
    # CHANGE 2026-11-18: the outline is the agent's own. Before, the whole eroded original silhouette was made
    # solid and the outline band took the original's colours (a 2 px staircase). Now only key colour enclosed by
    # the picture (not joined to the background around it) is a hole and gets the original's colour.
    hole = inner & (raw < 1.0)
    if keyed:
        hole &= ~outside_key(part_rgb)
        a = np.where(hole, 1.0, a)
    rgb[hole] = o2rgb[hole]
    rgb = match_colours(rgb, o2rgb, inner & ~hole & (raw >= 1.0))
    rgb[hole] = o2rgb[hole]
    rgb, a, flat = match_flat_background(rgb, a, o2rgb, o2solid, orig)
    if keyed:                                                          # pink edge bleed in spills
        a = np.where(~o2solid & light_pink(part_rgb), 0.0, a)
    # no NEW magenta tint (pink, lilac, purple outlines) where the original has none nearby: magenta bleed
    newtint = tinted(np.clip(rgb, 0, 255)) & (a > 0) & ~dilate(tinted(o2rgb) & o2solid, 3)
    if flat is not None:                                               # UI / awards: the flat background colour
        rgb[newtint] = flat
    else:
        # CHANGE 2026-11-18: the colour of the clean picture pixels next to it (was: the original's colour, or
        # cut off, which made the edge jagged), plus the outline cleanup of keyed pictures (edge_*)
        if keyed:
            band = edge_band(a)
            nt = edge_mark(rgb, a, band)
        else:
            band = np.zeros(shape, bool)
            nt = np.zeros(shape, np.uint8)
        nt[newtint] = 1
        rgb, nt = edge_recolour(rgb, a, nt)
        left = nt == 1                                                 # nothing clean nearby: original colour / clear
        rgb[left & o2solid] = o2rgb[left & o2solid]
        a = np.where(left & ~o2solid, 0.0, a)
        if keyed:
            a = edge_soften(a, band)
    a = np.where(mine, a, 0.0)
    if not keyed:
        return np.round(rgb).astype(np.uint8)
    out = np.zeros(shape + (4,), np.uint8)
    out[..., :3] = np.round(rgb).astype(np.uint8)
    out[..., 3] = np.round(a * 255).astype(np.uint8)
    out[out[..., 3] == 0] = 0
    return out


def main():
    mp = json.load(open(os.path.join(CODEX, "map.json")))
    sw, sh = mp["sheet_size"]
    s = mp["scale"]
    out = os.path.join(ROOT, "hd")                  # the game reads data/hd/<key>.png + bounds.txt
    os.makedirs(out, exist_ok=True)
    bpath = os.path.join(CODEX, "bounds.json")
    bounds = json.load(open(bpath)) if os.path.exists(bpath) else {}
    done = grown = 0
    for sheet, cells in sorted(mp["sheets"].items()):
        path = os.path.join(CODEX, "out_gen", sheet + ".png")
        if not os.path.exists(path):
            continue
        res = Image.open(path).convert("RGB")
        if res.size != (sw * s, sh * s):
            res = res.resize((sw * s, sh * s), Image.LANCZOS)
        rgb = np.asarray(res)
        boxes = [(c["x"] * s, c["y"] * s, (c["x"] + c["w"]) * s, (c["y"] + c["h"]) * s) for c in cells]
        drawn = key_alpha(rgb) > 0
        for i, c in enumerate(cells):                                  # opaque pictures do not spill
            if not c["keyed"]:
                x0, y0, x1, y1 = boxes[i]
                drawn[y0:y1, x0:x1] = False
        own = owners(drawn, boxes)
        for i, c in enumerate(cells):
            x0, y0, x1, y1 = boxes[i]
            name = c["file"].replace("res/", "res_").rsplit(".", 1)[0]
            orig = original(c["file"])
            if c["keyed"]:
                ys, xs = np.nonzero((own == i) & drawn)
                bx0 = int(min(x0, xs.min())) if len(xs) else x0
                by0 = int(min(y0, ys.min())) if len(ys) else y0
                bx1 = int(max(x1, xs.max() + 1)) if len(xs) else x1
                by1 = int(max(y1, ys.max() + 1)) if len(ys) else y1
            else:
                bx0, by0, bx1, by1 = x0, y0, x1, y1
            mine = own[by0:by1, bx0:bx1] == i
            img = finish(rgb[by0:by1, bx0:bx1], c["keyed"], mine, orig, bx0 - x0, by0 - y0)
            Image.fromarray(img).save(os.path.join(out, name + ".png"))
            g = (bx0, by0, bx1, by1) != (x0, y0, x1, y1)
            bounds[name] = {"dx": bx0 - x0, "dy": by0 - y0, "w": bx1 - bx0, "h": by1 - by0, "grown": g}
            grown += g
            done += 1
        print("%s: %d pictures" % (sheet, len(cells)))
    json.dump(bounds, open(bpath, "w"), indent=1, sort_keys=True)
    # the game's list: "key dx dy" per picture (offsets of pictures that grew past their box)
    with open(os.path.join(out, "bounds.txt"), "w", newline="\n") as f:
        f.write("".join("%s %d %d\n" % (k, v["dx"], v["dy"]) for k, v in sorted(bounds.items())))
    # award pictures: their flat beige square (slightly lighter than the panel) -> transparent
    awards = [os.path.join(out, n + ".png") for n in ("award", "ribbon", "medal", "ring", "trophy")]
    awards = [p for p in awards if os.path.exists(p)]
    if awards:
        import subprocess
        subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "key_flat_bg.py")] + awards,
                       check=True)
    print("%d pictures cut into %s, %d grew past their box (bounds.txt); start the game and press F2" % (done, out, grown))


if __name__ == "__main__":
    main()
