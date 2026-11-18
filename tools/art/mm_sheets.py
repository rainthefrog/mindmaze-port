"""Pack MindMaze's pictures into sprite sheets for AI upscaling with Codex (same workflow as Army Men v5).

  python3 mm_sheets.py <data folder>   -> <data>/art/work/hd/sheets/*.png + *.txt, map.json
  (reads the extracted pictures of the data folder)

Sheets are 768x512 at the ORIGINAL size; Codex returns them at 2x (1536x1024). Transparent sprites (palette
index 254 = cyan in the game files) are placed on pure magenta (#FF00FF) with a clear zone around each;
opaque pictures (backgrounds, story pages, panel buttons) fill their own box. Each sheet's .txt lists every
sprite's box in sheet pixels and what it shows. tools/art/mm_cut.py cuts the results back into single images.
"""
import json
import os
import sys

from PIL import Image

ROOT = sys.argv[1]
BAG = ROOT                                       # the data folder
RES = os.path.join(ROOT, "res")
OUT = os.path.join(ROOT, "art", "work", "hd")       # sheets/, out_gen/, map.json
SW, SH = 768, 512
GAP = 8            # clear zone between sprites (sheet pixels at 1x)
MAGENTA = (255, 0, 255)

# ---- what every picture is (from the game: SPEC.md / NOTES.md, and looking at each picture) ----
D = {
    # backgrounds (opaque)
    "titlescr.dib": "MindMaze title screen (shown when the game starts): storybook painting of a castle with towers and red roofs, a red dragon on the left, the jester at the gate, a lady in a window, a sailing ship and hills behind, stone frame with a carved banner reading Mind Maze at the bottom",
    "logonscr.dib": "castle entrance (player sign-in screen): arched wooden castle door between two bronze lion statues and two stone dragons, stone walls, torches, a pennant with a red question mark; lower left a stone tablet with painted serif lettering 'Enter your name if you have never been to MindMaze before! Or choose your name from the list. When you are finished, click on the castle door to enter.'",
    "grating.dib": "looking into the castle through a carved wooden window frame with wooden bars: a hooded figure in a dark cloak on the left, a corridor with arched wooden doors in the middle, a lady in a white wimple and green dress on the right (shown after sign-in)",
    "rmofdoor.dib": "the room of doors: castle corridor of blue and sandstone blocks with four arched wooden doors with brass bands and ring handles, a vaulted ceiling, sunlight, a wooden trapdoor in the floor (the player picks a door to enter)",
    "atrium.dib": "the atrium, the final room shown to a winning player: a scholar in a red robe and red cap beside a golden armillary sphere, two scribes writing at desks, tall bookshelves full of books, a blue parrot on a stand, a white marble statue, gothic windows, black-and-white checkered floor",
    "dunrm.dib": "room walls, dungeon style: an empty room of rough sandstone blocks - back wall, both side walls and a stone-slab floor in perspective",
    "plnwalls.dib": "room walls, plain style: plastered walls with blue-and-gold wainscot panels and draped trim (back wall and both side walls in perspective)",
    # room layers (transparent)
    "ceiling1.dib": "ceiling: dark wooden ceiling beam with ornate carved edge across the top of the room",
    "ceiling2.dib": "ceiling: wooden ceiling rafters with tall vertical posts down the sides of the room (frames the whole room)",
    "ceiling3.dib": "ceiling: plain wooden ceiling edge / beam across the top of the room",
    "carpet1.dib": "floor: large patterned oriental rug in perspective (blue, gold and red ornaments), seen from the room's front",
    "carpet2.dib": "floor: large round ornamental rug in perspective (red, yellow, blue pattern)",
    "flrcovr1.dib": "floor: woven floor covering / rug with geometric border in perspective",
    "chkfloor.dib": "floor: black-and-white checkerboard marble floor in perspective, with the bottom of a blue wall panel behind",
    # doors (transparent)
    "ndoor.dib": "door in the back wall: arched wooden plank door with iron hinges and a ring handle in a stone arch (north door)",
    "sdoor.dib": "small square sign with a red arrow pointing down in a wooden frame - the way back out of the room (south exit marker at the bottom of the room view)",
    "doorrt.dib": "door in the right wall: arched wooden door in a stone frame, seen at an angle (east door)",
    "doorlt.dib": "door in the left wall: arched wooden door in a stone frame, seen at an angle (west door)",
    "doortow.dib": "tower doorway: stone archway opening onto a winding stone staircase going up (the stairs to the next floor)",
    # characters (transparent)
    "alchem.dib": "the alchemist: old bearded man in a hooded robe at a table with flasks and potions, cluttered workshop shelves",
    "asiantra.dib": "Asian traveller: woman in a blue striped kimono with a red-and-green sash, holding a painted fan",
    "duke1.dib": "the duke (close-up): bust of a bearded man in a red cap with a blue-and-yellow macaw parrot on his shoulder",
    "duke2.dib": "the duke (standing): bearded man in a red cap, blue doublet and cape, with the parrot on his arm",
    "duke3.dib": "the duke (small, far away): standing duke with the parrot, full figure",
    "jester1.dib": "the jester popping out of a jack-in-the-box (blue box with stars and moon) on a wooden floor",
    "jester2.dib": "the jester laughing (close-up): head and shoulders, red-and-yellow checked costume and hat, holding a jester's stick with a small head on it",
    "jester3.dib": "the jester hanging upside down, checked costume, holding his jester's stick",
    "jester4.dib": "the jester standing (small): checked costume, holding his stick",
    "king1.dib": "the king standing on the blue jack-in-the-box: crowned king in a blue robe, one leg raised",
    "lady1.dib": "lady at a curtain: woman in a blue-green gown holding a letter, drawing back a heavy purple curtain",
    "maninst.dib": "man in the stocks: a prisoner locked in a wooden pillory next to a stone column",
    "metravel.dib": "Middle-Eastern traveller: bearded man in a blue turban, dark cloak, striped shirt, red sash and curved sword",
    "parrot.dib": "a large blue parrot / raven perched on a wooden stand",
    "queen.dib": "the queen with a long train carried by small servants, crowned queen in a blue-and-white gown",
    "secnldy1.dib": "lady in a white headscarf (close-up, front): woman in a white wimple and teal dress",
    "secnldy2.dib": "lady in a white headscarf (close-up, profile)",
    "servant1.dib": "servant crawling on the floor on a small rug",
    "servant2.dib": "servant kneeling on one knee",
    "sorceres.dib": "the sorceress: hooded woman in a dark robe stirring a big black cauldron over a fire",
    "suitarm1.dib": "suit of armour (close-up): knight's helmet with a gold plume and a halberd / pole axe",
    "suitarm2.dib": "suit of armour standing (small): full plate armour knight with a sword",
    # furniture and decoration (transparent)
    "bookshlf.dib": "tall wooden bookshelf full of books",
    "broom.dib": "a straw broom leaning on the wall",
    "candle1.dib": "small wall candle sconce",
    "candle2.dib": "tall candle on a turned wooden candlestick stand",
    "dtable1.dib": "small gilded side table with a blue top and a covered silver dish",
    "dtable2.dib": "banquet table with a white tablecloth, candlesticks, plates and two chairs",
    "furnitur.dib": "two blue upholstered high-backed armchairs with carved wooden frames",
    "shelf.dib": "small wall shelf with pots",
    "table1.dib": "wooden table with books and a candle",
    "table2.dib": "small wooden stool / low table",
    "trunk.dib": "wooden treasure chest with iron bands",
    "vase1.dib": "small blue vase",
    "vase2.dib": "tall blue vase with a green plant",
    "vase4.dib": "green urn on a small stand",
    "walltre1.dib": "wall decoration: a spear / halberd and a striped pennant hanging from the ceiling",
    "walltre2.dib": "wall decoration: two heraldic banners (black and gold, red and gold)",
    # pictures and awards (transparent)
    "award.dib": "award plaque: plain empty frame (no award yet)",
    "ribbon.dib": "award: red rosette ribbon",
    "medal.dib": "award: gold medal on a ribbon",
    "ring.dib": "award: gold ring",
    "trophy.dib": "award: gold trophy cup",
    # overlays (transparent)
    "banner.dib": "question banner: parchment sheet hanging from two hooks with a large illuminated letter Q (gold, green, red, blue) and a zig-zag cut bottom edge; the question text is written on the empty parchment by the game - keep the parchment EMPTY",
    "dialog.dib": "speech panel: blank parchment sheet with a zig-zag cut bottom edge; the game writes text on it - keep it EMPTY",
}
PIC_AREAS = ["physical science and technology (atom / molecule)", "life science (skeleton)", "geography (globe)",
             "history (hourglass)", "social science (people)", "religion and philosophy (symbols)",
             "art, language and literature", "performing arts", "sports, hobbies and pets"]
for k in range(1, 10):
    D["pic1-%d.dib" % k] = "small framed picture on the wall, wooden frame style 1, picture %d: area of interest icon - %s (black-and-white engraving inside the frame)" % (k, PIC_AREAS[k - 1])
    D["pic2-%d.dib" % k] = "small framed picture on the wall, gilded frame style 2, picture %d: area of interest icon - %s" % (k, PIC_AREAS[k - 1])
for k in (1, 2):
    D["open%d.dib" % k] = "intro story page %d: parchment-coloured page of small printed text (the game's story)" % k
for k in range(1, 7):
    D["end%d.dib" % k] = "ending story page %d: parchment-coloured page of small printed text (one of six endings)" % k
UI = {
    29150: "button strip, 4 states side by side (normal, hover, disabled/faded, pressed): small stack of books - 'look it up' button for answer A",
    29151: "button strip, 4 states: stack of books - look-up button for answer B",
    29152: "button strip, 4 states: stack of books - look-up button for answer C",
    29153: "button strip, 4 states: stack of books - look-up button for answer D",
    29154: "button strip, 4 states (black, red, grey, red): calligraphic numeral 1 - level button",
    29155: "button strip, 4 states: calligraphic numeral 2 - level button",
    29156: "button strip, 4 states: calligraphic numeral 3 - level button",
    29157: "button strip, 4 states: calligraphic numeral 4 - level button",
    29158: "button strip, 4 states (burning, burning, burnt out with smoke, burning): wooden match / torch - map reveal",
    29159: "button strip, 4 states: match", 29160: "button strip, 4 states: match", 29161: "button strip, 4 states: match",
    29162: "button strip, 4 states: match",
    29163: "button strip, 4 states: dark arrow pointing right (next page)",
    29164: "radio button strip, 4 states: empty white circle, filled dark circle, grey circle, white circle",
    29166: "points seal: pale embossed circle with the word 'Points' at the bottom on a beige panel",
}

GROUPS = [  # (prefix, files, note)
    ("backgrounds", ["logonscr.dib", "grating.dib", "rmofdoor.dib", "atrium.dib", "dunrm.dib", "plnwalls.dib", "titlescr.dib"],
     "Full painted backgrounds, each one fills its box completely (no transparent parts)."),
    ("layers", ["ceiling1.dib", "ceiling3.dib", "carpet1.dib", "carpet2.dib", "flrcovr1.dib", "ceiling2.dib", "chkfloor.dib"],
     "Room layers: ceilings and floor coverings that the game stacks over the room walls. Each is 635-636 px wide and lines up exactly with the walls - keep the outlines and perspective exactly."),
    ("doors", ["ndoor.dib", "sdoor.dib", "doorrt.dib", "doorlt.dib", "doortow.dib"],
     "Doors that the game places into the room walls; they must keep their exact outline to fit the walls."),
    ("characters", sorted(k for k in D if k.split(".")[0] in (
        "alchem", "asiantra", "duke1", "duke2", "duke3", "jester1", "jester2", "jester3", "jester4", "king1", "lady1", "maninst",
        "metravel", "parrot", "queen", "secnldy1", "secnldy2", "servant1", "servant2", "sorceres", "suitarm1", "suitarm2")),
     "Castle characters (hand-painted people of a medieval fairy-tale court). Keep faces, costumes, poses and colours."),
    ("furniture", sorted(k for k in D if k.split(".")[0] in (
        "bookshlf", "broom", "candle1", "candle2", "dtable1", "dtable2", "furnitur", "shelf", "table1", "table2", "trunk",
        "vase1", "vase2", "vase4", "walltre1", "walltre2")),
     "Furniture and decoration placed in the rooms."),
    ("pictures", ["pic1-%d.dib" % k for k in range(1, 10)] + ["pic2-%d.dib" % k for k in range(1, 10)] +
     ["award.dib", "ribbon.dib", "medal.dib", "ring.dib", "trophy.dib"],
     "Small framed wall pictures (one icon per area of interest) and the award pictures."),
    ("overlays", ["banner.dib", "dialog.dib"],
     "Parchment panels that the game writes text onto: keep the writing area EMPTY."),
    ("pages", ["open1.dib", "open2.dib"] + ["end%d.dib" % k for k in range(1, 7)],
     "Story pages made of printed text. TEXT: copy every letter exactly; if the text cannot be kept exactly readable, leave the page unchanged apart from sharpening."),
    ("ui", ["res/%d.bmp" % k for k in sorted(UI)],
     "Control panel buttons. Each strip holds 4 states of the same button side by side: keep the 4 states in their exact places and sizes."),
]


def load(name):
    if name.startswith("res/"):
        im = Image.open(os.path.join(RES, name[4:])).convert("RGB")
        return im, False
    path = os.path.join(BAG, name)
    raw = Image.open(path)
    keyed = False
    if raw.mode == "P":
        pal = raw.getpalette()
        keyed = pal[254 * 3:254 * 3 + 3] == [0, 255, 255]
        im = raw.convert("RGBA")
        if keyed:   # palette index 254 is transparent, as the game draws them
            a = Image.frombytes("L", raw.size, bytes(0 if v == 254 else 255 for v in raw.tobytes()))
            im.putalpha(a)
    else:
        im = raw.convert("RGBA")
    return im, keyed


def pack(items):
    """Shelf packing, tallest first. Returns [(sheet, [(name, x, y)])]."""
    items = sorted(items, key=lambda t: (-t[2], -t[1]))
    sheets = []
    for name, w, h in items:
        if w + 2 * GAP > SW or h + 2 * GAP > SH:
            raise SystemExit("too big for a sheet: %s %dx%d" % (name, w, h))
        placed = False
        for sh in sheets:
            for shelf in sh["shelves"]:
                if shelf["x"] + w + GAP <= SW and h <= shelf["h"]:
                    sh["cells"].append((name, shelf["x"], shelf["y"]))
                    shelf["x"] += w + GAP
                    placed = True
                    break
            if placed:
                break
            if sh["y"] + h + GAP <= SH:
                sh["shelves"].append({"x": GAP + w + GAP, "y": sh["y"], "h": h})
                sh["cells"].append((name, GAP, sh["y"]))
                sh["y"] += h + GAP
                placed = True
                break
        if not placed:
            sheets.append({"shelves": [{"x": GAP + w + GAP, "y": GAP, "h": h}], "cells": [(name, GAP, GAP)], "y": GAP + h + GAP})
    return [s["cells"] for s in sheets]


def main():
    os.makedirs(os.path.join(OUT, "sheets"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "out_gen"), exist_ok=True)
    mapping = {"sheet_size": [SW, SH], "scale": 2, "sheets": {}}
    magenta_warn = []
    total = 0
    for prefix, files, note in GROUPS:
        imgs = {f: load(f) for f in files}
        for f, (im, keyed) in imgs.items():
            if keyed:
                px = [p for p in im.getdata() if p[3] and p[0] > 200 and p[2] > 200 and p[1] < 80]
                if px:
                    magenta_warn.append("%s: %d near-magenta pixels" % (f, len(px)))
        packed = pack([(f, imgs[f][0].width, imgs[f][0].height) for f in files])
        for n, cells in enumerate(packed):
            sname = "%s_%03d" % (prefix, n)
            sheet = Image.new("RGB", (SW, SH), MAGENTA)
            lines = ["%s.png (%dx%d; positions in sheet pixels, x then y)" % (sname, SW, SH), "NOTE: " + note]
            entry = []
            for f, x, y in sorted(cells, key=lambda c: (c[2], c[1])):
                im, keyed = imgs[f]
                if keyed:
                    sheet.paste(im, (x, y), im)
                else:
                    sheet.paste(im.convert("RGB"), (x, y))
                w, h = im.size
                desc = UI[int(f[4:-4])] if f.startswith("res/") else D[f]
                kind = "transparent sprite on magenta" if keyed else "opaque picture, fills its box"
                lines.append("- x %d-%d, y %d-%d: %s (%s, %dx%d, %s)" % (x, x + w - 1, y, y + h - 1, desc, f, w, h, kind))
                entry.append({"file": f, "x": x, "y": y, "w": w, "h": h, "keyed": keyed})
                total += 1
            sheet.save(os.path.join(OUT, "sheets", sname + ".png"))
            open(os.path.join(OUT, "sheets", sname + ".txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
            mapping["sheets"][sname] = entry
    json.dump(mapping, open(os.path.join(OUT, "map.json"), "w"), indent=1)
    print("%d pictures on %d sheets" % (total, len(mapping["sheets"])))
    for s, e in mapping["sheets"].items():
        print("  %s: %d" % (s, len(e)))
    for w in magenta_warn:
        print("WARNING", w)


if __name__ == "__main__":
    main()
