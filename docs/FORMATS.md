# File formats

Formats of the Encarta 2006 files the extractor reads, and of the `data/` folder the game reads.

## Inputs (Encarta 2006 disc, `EE/ENCARTA/`)

### MINDMAZE.DB / MINDMAZE.IDX (questions)
- DB: records back to back, little endian: `u32 0`, `u32 length`, question text (cp1252), then 4 answers, each
  `u8 length`, text (cp1252), `u32 Encarta article id`, `u16 value`. The **first** answer is the correct one
  (the game shuffles them).
- IDX: `u32[36]` question counts, area-major (area 0 levels 0..3, area 1 levels 0..3, ...), then per question
  in the same order `u32 question number (1-based)`, `u32 DB offset`.
- Area order in the DB: 0 Physical Science & Technology, 1 Life Science, 2 Geography, 3 History, 4 Social
  Science, 5 Religion and Philosophy, 6 Art, Language, and Literature, 7 Performing Arts, 8 Sports, Hobbies,
  and Pets.
- Question texts contain HTML entities and `<it>`/`<i>` italics tags.
- `tools/extract/mmdb.py decode|encode` converts both ways (byte-exact round trip).

### MINDMAZE.EIT (pictures, sounds, music)
An MS Help 2 (ITOL/ITSF) archive; 7-Zip unpacks it (`-thxs`). Contents: `baggage/*.dib` (8-bit BMP files,
palette index 254 = cyan = transparent), `baggage/*.wav` (MS ADPCM, 22050 Hz mono), `baggage/*.mid`,
`Area0..8.lst` (Encarta article ids per area; not used).

### ENCARTA.EXE (tables)
MindMaze is compiled into the Encarta program. The extractor finds these tables by patterns (strings and the
pointers to them), in 2006 and in the older editions' programs too: see the header comments of
`src/extract/mx_tables_pe.c` (2006, 97, 98), `mx_tables_ne.c` (95, 16-bit) and `mx_tables_mac.c` (96 Macintosh),
and `mx_oldext.c` for where each edition keeps its files. Virtual addresses in the 2006 Premium build (image base
0x400000; `.rdata` 0x618000 at file offset 0x218000, `.data` 0x6ac000 at 0x2ac000):

| Address | Contents |
|---|---|
| 0x6b54a0 | `u32[6]` first record of each object group |
| 0x6b54b8 | `u32[6]` object counts: doors, floors, ceilings, walls, characters, furniture |
| 0x6b4550 | room object records, 0x38 bytes each (layout below), the six groups back to back |
| 0x6b54d0 | 6 ranks: `{char *name, char *picture, u32 min, u32 max}` |
| 0x6b5880 / 0x6b58e0 | `u32[23]` pointers to the speech text lists of object ids 7..29 / their counts |
| 0x6b425c | `char *[9]` subjects of the wall pictures (ids 31..39) |
| 0x6b41bc / 0x6b41d0 | `char *[5]` music file names / `char *[35]` sound file names |

Object record: `+0 char *picture`, `+4 i32 x`, `+8 i32 y`, `+0x14 i32[4]` click rectangle l, t, r, b
(relative to x, y; all 0 = not clickable), `+0x24 i32 half`, `+0x28 u8[7]` placement slots, `+0x30 u8 flags`
(1 keeps door occupancy, 2 no floor covering), `+0x32 u16 object id`.

### ENCARTAR.DLL (resources)
- BITMAP 29150-29164, 29166: panel buttons and the seal (DIBs without file header; the extractor adds one).
- STRING 29002-29069: MindMaze's UI texts (US English, language 0x409).

## Output: the `data/` folder

| File | Contents |
|---|---|
| `*.dib` | pictures, lower-case names, unchanged from MINDMAZE.EIT |
| `*.wav` | sound effects, decoded to 16-bit PCM 22050 Hz mono |
| `*.mid` | music |
| `res/<id>.bmp` | panel bitmaps from ENCARTAR.DLL |
| `mindmaze.db`, `mindmaze.idx` | questions, unchanged |
| `questions.json` | the same questions as JSON (`mindmaze-questions-1`) |
| `tables.json` | tables from ENCARTA.EXE and texts from ENCARTAR.DLL (`mindmaze-tables-1`, below) |
| `packs/` | the open question packs + `index.txt` (file, id, name, licence, count; tab-separated) |
| `manifest.json` | extractor version, input file sizes and SHA-256, counts |
| `hd/`, `wide/` | optional HD / widescreen art you made (docs/HD-ART.md) |

### tables.json (`mindmaze-tables-1`)
```
{
 "format": "mindmaze-tables-1",
 "source": "Encarta 2006 Premium (US English)",
 "groups": {"doors": 5, "floors": 4, "ceilings": 3, "walls": 2, "characters": 22, "furniture": 34},
 "objects": [{"name": "X.DIB", "x": 0, "y": 0, "rect": [l, t, r, b], "half": 1,
              "slots": [7 x 0/1], "flags": 0, "id": 0}, ...],          // groups in the order above
 "ranks": [{"name": "...", "image": "X.DIB", "min": 0, "max": 2499}, ...],   // 6
 "speech": [["...", ...], ...],          // 23 lists, index = object id - 7
 "picture_areas": [9 names], "music": [5 file names], "sounds": [35 file names],
 "strings": {"29002": "...", ...}        // UTF-8
}
```
Doors are always 5 (north, south, east, west, stairs); at most 64 furniture objects. Loaded at start by
`src/core/mm_tables.c`, which checks counts and types and reports the first problem.
