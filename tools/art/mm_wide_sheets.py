"""Widescreen extension sheets for Codex: the pictures that reach the left or right edge of the 636 px room view
get painted further sideways, so the game can show a wider (about 16:9) view.

  python3 mm_wide_sheets.py <data folder>   -> <data>/art/work/wide/sheets/*.png + *.txt, masks/, map.json

One picture per sheet (768x512, original size; Codex returns 2x = 1536x1024). The original picture sits in the
sheet as usual (transparent parts on magenta); the strip to paint is flat pure GREEN (#00FF00), EXT px wide,
on each side where the picture reaches the edge of the view. The original part is only a reference: the
cutter keeps the game's own picture (or its HD version) there and takes only the painted green strips, blended
into the edge.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image  # noqa: E402

import mm_sheets  # noqa: E402  (descriptions, loader)

ROOT = sys.argv[1]
OUT = os.path.join(ROOT, "art", "work", "wide")      # ROOT = the data folder
SW, SH = 768, 512
EXT = 64            # extension per side in original pixels: 636 + 2*64 = 764 wide (about 16:9 with the panel)
VIEW_W = 636
MAGENTA = (255, 0, 255)
GREEN = (0, 255, 0)

# picture -> (x in the room view, kind). Sides are found from the picture itself.
ITEMS = [
    ("logonscr.dib", 0, "backgrounds"), ("grating.dib", 0, "backgrounds"), ("rmofdoor.dib", 0, "backgrounds"),
    ("atrium.dib", 0, "backgrounds"), ("dunrm.dib", 0, "backgrounds"), ("plnwalls.dib", 0, "backgrounds"),
    ("titlescr.dib", 0, "backgrounds"),   # added 2026-11-18 (title screen shown at start): sheet backgrounds_006
    ("ceiling1.dib", 0, "layers"), ("ceiling2.dib", 0, "layers"), ("ceiling3.dib", 0, "layers"),
    ("carpet1.dib", 0, "layers"), ("carpet2.dib", 0, "layers"), ("flrcovr1.dib", 0, "layers"),
    ("chkfloor.dib", 0, "layers"),
    ("duke1.dib", 0, "objects"), ("lady1.dib", 0, "objects"), ("alchem.dib", 203, "objects"),
    ("secnldy2.dib", 363, "objects"), ("suitarm1.dib", 154, "objects"), ("vase2.dib", 586, "objects"),
]

WHAT = {   # what the extension should show (from looking at each picture's edge)
    "backgrounds": "Continue the painting sideways into the green strip(s): the same scene, walls, floor, ceiling, "
                   "light and style, following the existing perspective lines outwards. No new people, doors, "
                   "windows, furniture or text.",
    "layers": "Continue this room layer (ceiling beams / rafters / posts, or the floor covering) into the green "
              "strip(s) along its perspective, so it reaches further sideways. Wherever the layer itself would "
              "not be, paint flat magenta (it is transparent there).",
    "objects": "Continue the part of this picture that is cut off by the edge of the room view into the green "
               "strip (the rest of the figure / object that was outside the frame). Wherever nothing of it would "
               "be, paint flat magenta (transparent).",
}


# STRETCH or FILL per picture (chosen by looking at each edge):
#  - FILL: the picture keeps its size and place (rooms: a wider camera view, so every hotspot and every layer
#    stays where the game puts it); Codex paints what lies beyond the old edge.
#  - STRETCH: the picture has its own ornamental border frame (only LOGONSCR, a framed painting filling the whole
#    window): the frame moves to the new edges and the gap between frame and picture is closed, so the frame is
#    never cut. Measured on LOGONSCR: frame bar 12 px wide on both sides, corner rosettes 34 px wide at the top (28 at the bottom, where the tablet text starts at x 29), top 26 and
#    bottom 44 rows high.
#    TITLESCR (2026-11-18): grey stone frame 23 px wide on the left (x 0-22) and 33 px on the right (x 603-635);
#    one band, split at the inner edges of the stone; the "Mind Maze" banner at the bottom is cut at the same
#    columns, so its gold knotwork continues through the gaps.
# STRETCH bands: (y0, y1, split_left, split_right) in original pixels
STRETCH = {"logonscr.dib": [(0, 26, 34, 636 - 34), (26, 437 - 44, 12, 636 - 12), (437 - 44, 437, 28, 636 - 28)],
           "titlescr.dib": [(0, 437, 23, 603)]}

EXTRA = {   # per-picture TASK text (replaces the generic one)
    "logonscr.dib": "Close the green gaps. The vertical frame bars stay at the new outer edges; the top and bottom "
                    "frame bands (with their rope ornament) continue straight through the gaps at the same height "
                    "and thickness; the corner rosettes stay complete at the new corners. Inside the frame, "
                    "continue the castle wall's stone courses, the paving and the light. The stone tablet with "
                    "lettering at the lower left is NOT widened and gets no new letters: paint stone wall between "
                    "the frame and the tablet. No second door, statue, torch or dragon.",
    "titlescr.dib": "Close the green gaps. The grey stone frame stays at the new outer edges and continues "
                    "straight through the gaps at the top and bottom (same stone texture and cracks). Inside the "
                    "frame, continue the landscape outwards: sky and clouds, hills and mountains, the river, the path "
                    "and trees. No new castle towers, people, dragon, boats or flags. In the bottom banner, continue "
                    "the gold knotwork band through the gaps; the words 'Mind Maze' stay exactly as they are, "
                    "centred, with no new letters.",
    "grating.dib": "The window frame keeps its size and place. Paint more of the dark carved wooden wall around "
                   "the window into the strips: same dark brown wood, the grain lines continue; no second window.",
    "rmofdoor.dib": "Complete the doors cut off at the left and right edges (arch, planks, brass bands, ring "
                    "handle) and continue the stone corridor wall, the vaulted ceiling and the floor beyond them. "
                    "No additional doors.",
    "atrium.dib": "Continue the walls, bookshelves, windows and the black-and-white checkered floor beyond the "
                  "edges. No new people, desks or statues.",
    "dunrm.dib": "Continue the rough sandstone side walls (block courses) and the stone-slab floor outwards.",
    "plnwalls.dib": "Continue the plastered side walls and the stone-slab floor outwards.",
    "ceiling1.dib": "Continue the dark carved ceiling beam along its line until it leaves the picture as the "
                    "GEOMETRY says; magenta below and beyond it.",
    "ceiling3.dib": "Continue the plain wooden ceiling beam along its line until it leaves the picture as the "
                    "GEOMETRY says; magenta below and beyond it.",
    "ceiling2.dib": "Continue the ceiling rafters and the beams along their lines. The upright posts stay where "
                    "they are: no new posts. Magenta where there is no wood.",
    "carpet1.dib": "Continue the rug's outline and border along its edges until the edge leaves the picture as "
                   "the GEOMETRY says; magenta outside the rug.",
    "carpet2.dib": "Continue the rug's outline and border along its edges until the edge leaves the picture as "
                   "the GEOMETRY says; magenta outside the rug.",
    "flrcovr1.dib": "Continue the floor covering's outline and border along its edges until the edge leaves the "
                    "picture as the GEOMETRY says; magenta outside it.",
    "chkfloor.dib": "Continue the black-and-white checkerboard (same squares, same perspective, rows stay "
                    "horizontal) and the blue wall panel strip behind it, at the same height, outwards.",
    "duke1.dib": "Continue the duke's shoulder, cape and doublet that the left edge cut off.",
    "lady1.dib": "Continue the heavy purple curtain with its gold pattern and folds to the left, full height.",
    "alchem.dib": "Continue the alchemist's workshop to the right: stone wall, shelves and table with the same "
                  "kind of flasks; no second person.",
    "secnldy2.dib": "Continue the lady's shoulder, dress and white headscarf that the right edge cut off.",
    "suitarm1.dib": "Continue the armour's shoulder plate and the gold plume that the right edge cut off.",
    "vase2.dib": "Complete the other half of the blue vase and its green plant (the vase is symmetric).",
}


def layout(f, w, h):
    """Row bands with their split columns: [(y0, y1, split_left, split_right)]."""
    s = STRETCH.get(f)
    if not s:
        return "fill", [(0, h, 0, w)]
    return "stretch", list(s)


def edge_sides(im, keyed, x):
    """Which sides of the view the picture reaches with opaque pixels: 'L', 'R'."""
    w, h = im.size
    a = im.split()[3] if keyed else None
    sides = []
    for side, col, at_edge in (("L", 0, x == 0), ("R", w - 1, x + w >= VIEW_W - 1)):
        if not at_edge:
            continue
        if a is None or any(a.getpixel((col, yy)) for yy in range(h)):
            sides.append(side)
    return sides


# Room geometry, measured on PLNWALLS.DIB (wall/floor colour edge per column; DUNRM and the layers share it):
# back wall from x 140 to 490, floor line y 194; the side-wall/floor lines run from the back corners to the view
# edge and on, all perspective lines meet at the vanishing point.
ROOM_VP = (318, 118)
ROOM_LINES = [  # (name, side, (x, y) on the view border, (x, y) at the strip's outer edge) in view coordinates
    ("floor line (where the left side wall meets the floor)", "left", (0, 251), (-EXT, 278)),
    ("floor line (where the right side wall meets the floor)", "right", (635, 250), (635 + EXT, 278)),
]
ROOM_SHEETS = ("dunrm.dib", "plnwalls.dib")


def outline_guides(alpha, side, fit=30):
    """Straight-line continuations of the outline edges that cross the view border (layers).
    Returns [(edge kind, y at border, y at outer strip edge or None, x-distance where it leaves the picture)]."""
    w, h = alpha.size
    a = alpha.load()
    col = (lambda d: d) if side == "L" else (lambda d: w - 1 - d)

    def runs(x):
        r, s = [], None
        for y in range(h):
            o = a[x, y] > 0
            if o and s is None:
                s = y
            if not o and s is not None:
                r.append((s, y - 1))
                s = None
        if s is not None:
            r.append((s, h - 1))
        return r

    out = []
    for s, e in runs(col(0)):
        for kind, y0 in (("top", s), ("bottom", e)):
            if (kind == "top" and y0 == 0) or (kind == "bottom" and y0 == h - 1):
                continue   # runs along the picture's own top/bottom border
            pts, y = [], y0
            for d in range(fit):
                cand = [rr[0] if kind == "top" else rr[1] for rr in runs(col(d))]
                if not cand:
                    break
                yn = min(cand, key=lambda c: abs(c - y))
                if pts and abs(yn - y) > 3:
                    break   # jumped onto another part (post, rafter): stop the fit here
                y = yn
                pts.append((d, y))
            n = len(pts)
            mx = sum(p[0] for p in pts) / n
            my = sum(p[1] for p in pts) / n
            sxx = sum((p[0] - mx) ** 2 for p in pts) or 1
            k = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx
            b = my - k * mx
            yo = b + k * (-EXT)              # outward = negative distance
            leave, edge = None, None
            if yo < 0 or yo > h - 1:
                edge = 0 if yo < 0 else h - 1
                leave = (b - edge) / k if k else None   # distance outward where it hits the picture border
                yo = None
            out.append((kind, y0, yo, leave, edge))
    return out


def _runs(alpha, x):
    a = alpha.load()
    r, s = [], None
    for y in range(alpha.size[1]):
        o = a[x, y] > 0
        if o and s is None:
            s = y
        if not o and s is not None:
            r.append((s, y - 1))
            s = None
    if s is not None:
        r.append((s, alpha.size[1] - 1))
    return r


def fmt(x, y, sx=0, sy=0):
    return "(%d, %d) [2x result: (%d, %d)]" % (round(x + sx), round(y + sy), round(2 * (x + sx)), round(2 * (y + sy)))


def main():
    os.makedirs(os.path.join(OUT, "sheets"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "out_gen"), exist_ok=True)
    mapping = {"sheet_size": [SW, SH], "scale": 2, "ext": EXT, "sheets": {}}
    count = {}
    for f, vx, kind in ITEMS:
        im, keyed = mm_sheets.load(f)
        w, h = im.size
        sides = edge_sides(im, keyed, vx)
        if not sides:
            print("skip (does not reach an edge):", f)
            continue
        tw = w + EXT * len(sides)
        if tw > SW or h > SH:
            raise SystemExit("too big: %s" % f)
        n = count.get(kind, 0)
        count[kind] = n + 1
        sname = "%s_%03d" % (kind, n)
        sheet = Image.new("RGB", (SW, SH), MAGENTA)
        mask = Image.new("L", (SW, SH), 0)                     # 255 = area to paint (green)
        x0 = (SW - tw) // 2
        # tall pictures (logonscr) centred with equal magenta above and below: with a 4 px top margin Codex
        # painted over the whole canvas (2026-11-16), changing the layout
        y0 = (SH - h) // 2 if h > SH - 16 - 64 else 8
        px = x0 + (EXT if "L" in sides else 0)
        mode, bands = layout(f, w, h)
        rgba = im if keyed else im.convert("RGBA")
        cuts = []
        for ya, yb, sl, sr in bands:
            # picture columns [0,sl) stay at the left, [sl,sr) move right by EXT (left side extended),
            # [sr,w) move right by one more EXT (right side extended); the gaps are the green paint area
            if "L" not in sides:
                sl = 0
            if "R" not in sides:
                sr = w
            gl = EXT if "L" in sides else 0
            segs = [(0, sl, 0), (sl, sr, gl), (sr, w, gl + (EXT if "R" in sides else 0))]
            for a, b, sh in segs:
                if b > a:
                    part = rgba.crop((a, ya, b, yb))
                    sheet.paste(part.convert("RGB"), (x0 + a + sh, y0 + ya), part)
            if "L" in sides:
                mask.paste(255, (x0 + sl, y0 + ya, x0 + sl + EXT, y0 + yb))
            if "R" in sides:
                mask.paste(255, (x0 + sr + gl, y0 + ya, x0 + sr + gl + EXT, y0 + yb))
            cuts.append({"y0": ya, "y1": yb, "split_left": sl, "split_right": sr})
        sheet.paste(GREEN, (0, 0, SW, SH), mask)
        strips = []
        if "L" in sides:
            strips.append(("left", x0, y0, EXT, h))
        if "R" in sides:
            strips.append(("right", x0 + tw - EXT, y0, EXT, h))
        desc = mm_sheets.D[f]
        opaque = not (keyed and im.split()[3].getextrema()[0] == 0)
        lines = ["%s.png (%dx%d; positions in sheet pixels, x then y)" % (sname, SW, SH)]
        if mode == "fill":
            lines.append("ACTION: FILL IN DETAILS - the picture keeps its size and place; paint what lies beyond its "
                         "edge into the green strip(s).")
            lines.append("PICTURE: x %d-%d, y %d-%d: %s (%s, %dx%d, %s). Keep it exactly as it is (only redrawn at 2x)."
                         % (px, px + w - 1, y0, y0 + h - 1, desc, f, w, h,
                            "opaque, fills its box" if opaque else "transparent parts on magenta"))
            for side, sx, sy, sw, sh in strips:
                lines.append("PAINT (%s side): the GREEN strip x %d-%d, y %d-%d." % (side, sx, sx + sw - 1, sy, sy + sh - 1))
        else:
            lines.append("ACTION: STRETCH - the picture's own border frame has been moved outwards to the new, wider "
                         "edges; the green gaps between the moved frame and the rest of the picture are the paint "
                         "area. Close them so the picture looks as if it had always been this wide.")
            lines.append("PICTURE: x %d-%d, y %d-%d (now %d px wide): %s (%s, originally %dx%d, opaque)."
                         % (x0, x0 + tw - 1, y0, y0 + h - 1, tw, desc, f, w, h))
            lines.append("PAINT: every GREEN area: a vertical gap %d px wide inside each side of the frame%s." % (
                EXT, ", which reaches further in at the top and bottom where the old corner ornaments were"
                if len(bands) > 1 else ", over the full height"))
        lines.append("TASK: " + EXTRA.get(f, WHAT[kind]))
        if f in ("dunrm.dib", "plnwalls.dib", "atrium.dib", "rmofdoor.dib"):
            lines.append("Extra: the side walls run towards the viewer; in the green strips they come closer, "
                         "so continue their stone courses / panels, floor and ceiling lines outwards along the "
                         "same vanishing lines.")
        # ---- alignment guides (sheet pixels; the 2x result coordinates in brackets) ----
        geo = []
        if kind != "backgrounds" or f in ROOM_SHEETS:
            geo.append("Vanishing point of the room (all receding lines meet here; lines parallel to the back wall "
                       "stay exactly horizontal, upright edges stay exactly vertical): %s."
                       % fmt(ROOM_VP[0] - vx, ROOM_VP[1], px, y0))
        if f in ROOM_SHEETS:
            for name, side, (bx, by), (ox, oy) in ROOM_LINES:
                geo.append("Straight line, %s: crosses the strip border at %s and must reach the strip's outer edge "
                           "at %s." % (name, fmt(bx, by, px, y0), fmt(ox, oy, px, y0)))
        if kind == "layers":
            alpha = im.split()[3]
            for side in sides:
                bx = px if side == "L" else px + w - 1
                sgn = -1 if side == "L" else 1
                for ek, yb, yo, leave, ye in outline_guides(alpha, side):
                    what = "%s edge of the layer on the %s side" % (ek, "left" if side == "L" else "right")
                    if yo is not None:
                        geo.append("Straight line, %s: crosses the border at %s and must reach the strip's outer "
                                   "edge at %s." % (what, fmt(bx, yb, 0, y0), fmt(bx + sgn * EXT, yo, 0, y0)))
                    elif leave is not None and leave > 0:
                        geo.append("Straight line, %s: crosses the border at %s and runs straight to %s, where it "
                                   "leaves the picture: the layer ends there, everything further out is flat magenta."
                                   % (what, fmt(bx, yb, 0, y0), fmt(bx + sgn * leave, ye, 0, y0)))
        if kind == "objects":
            alpha = im.split()[3]
            for side in sides:
                bx = px if side == "L" else px + w - 1
                for s, e in [r for r in _runs(alpha, 0 if side == "L" else w - 1)]:
                    geo.append("The visible outline crosses the border at x %d from y %d to y %d [2x result: x %d, "
                               "y %d-%d]; the continuation starts exactly there, with the same width and direction."
                               % (bx, s + y0, e + y0, 2 * bx, 2 * (s + y0), 2 * (e + y0)))
        if geo:
            lines.append("GEOMETRY (strict; check the result against it):")
            lines += ["- " + g for g in geo]
        sheet.save(os.path.join(OUT, "sheets", sname + ".png"))
        os.makedirs(os.path.join(OUT, "masks"), exist_ok=True)
        mask.save(os.path.join(OUT, "masks", sname + ".png"))
        open(os.path.join(OUT, "sheets", sname + ".txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
        mapping["sheets"][sname] = {"file": f, "mode": mode, "view_x": vx, "sheet_x": x0, "x": px, "y": y0,
                                    "w": w, "h": h, "keyed": keyed, "sides": sides, "bands": cuts,
                                    "strips": [{"side": s, "x": sx, "y": sy, "w": sw, "h": sh}
                                               for s, sx, sy, sw, sh in strips]}
        print("%-16s %-14s sides %s" % (sname, f, "".join(sides)))
    json.dump(mapping, open(os.path.join(OUT, "map.json"), "w"), indent=1)
    print(len(mapping["sheets"]), "sheets")


if __name__ == "__main__":
    main()
