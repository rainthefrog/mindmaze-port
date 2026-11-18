# MindMaze (Encarta 2006) - reverse-engineering notes

Reverse-engineering notes for the MindMaze trivia game inside Microsoft Encarta 2006 Premium (US English). They
describe `ENCARTA.EXE` with SHA-256 089adc5db30653e4af2d8a37072052c0bddc80049f9943e25b8d41d95905045f.

Goal: a standalone MindMaze. Pass 1 = Windows exe recreating the original; pass 2 = web app. Questions come from
a pluggable source (original DB, JSON, packs, URL...). Game rules, maze and layout are in `SPEC.md`.
This repository contains no Microsoft content; the notes name addresses and structures only.

## Files
Local working folders (not in the repository) hold the original data; the repository only has code and tools.
- Original data: MINDMAZE.DB / .IDX / .EIT from the Encarta ISO (EE/ENCARTA), untouched.
- MINDMAZE.EIT unpacked (7-Zip `-thxs`): `baggage/*.dib` (8-bit BMPs, cyan 0,255,255 = transparent), `*.wav`,
  `*.MID`, `Area0..8.lst` (Encarta article ids per area; not needed for play).
- ENCARTA.EXE (+ENCARTAR.DLL resources, ENCARTAU.DLL): the MindMaze code is compiled into ENCARTA.EXE.
- `tools/extract/mmdb.py decode|encode` converts the question DB to JSON and back (byte-exact round trip).
- Ghidra pseudocode of ENCARTA.EXE (one function hung Ghidra after 0x4b9dd0 and is skipped) and the
  ENCARTAR.DLL resources (string table as UTF-16) are local working folders too.
- VA <-> file offset: .text 0x401000 @0x1000; .rdata 0x618000 @0x218000; .data 0x6ac000 @0x2ac000.

## Question data (verified)
- DB record: u32 0, u32 qlen, question (cp1252), 4 x {u8 len, text, u32 article id, u16 value}.
  answers[0] = correct. The u16 value is only used to build an Encarta article link (FUN_00522550).
- IDX: u32[36] counts (area-major, 4 levels each), then {u32 question number, u32 DB offset} in that order.
- DB area order (verified by SPEC.md / FUN_00520270 and by sampling questions): 0 Physical Science &
  Technology, 1 Life Science, 2 Geography, 3 History, 4 Social Science, 5 Religion and Philosophy,
  6 Art, Language, and Literature, 7 Performing Arts, 8 Sports, Hobbies, and Pets.
  (The UI string ids 29015-29023 list them in a different order.) "All Areas" = 29014.
- The original DB holds 8,020 questions.

## Game rules found so far
- Global game object `DAT_006c2fe0`: +0xb8 current player index (6 = none), +0xc0 player array, +0xc4 count,
  +0xd0 current question, +0xf4 question dialog, +0x90 sound/music object, +0x104 text panel.
- Player: +0x14 score, +0x18 maze ptr, +0x1c award rank, +0x20 level (0-3, chosen by the player with the
  level buttons, never rises by itself), +0x24 selected area button (0-9), +0xc slot (0-4). Max 5 players.
- Full maze / movement / screens / persistence spec: SPEC.md ([V] = read in code).
- Question (FUN_00522030): answers shuffled (q+0x30 short[4] = original index per button);
  q[0xf] = 66 = seconds; a 1 s timer (0x522660) counts it down to 0; the dialog shows (level+1)*seconds.
- Answer (FUN_0051f750, button 0-3): correct (orig index 0) -> sound 0x21, points = (level+1)*seconds left,
  "Correct!  %u points added to your score." (29041 / singular 29057), score += points.
  Wrong 1st try -> sound 0x22, string 29039 (try-again message), button disabled, timer keeps running.
  Wrong 2nd try -> 29040 "The correct answer is %s.", no points.
- Picking (FUN_005226a0, FUN_00522a30, FUN_00522c10): per player a shuffled list of unanswered questions for
  (area, level); "All Areas" picks a random area from the player's selection. Out of questions -> a message
  saying all questions for the selection were already answered (string in the 29xxx range).
- Ranks (table 0x6b54d0, {name, image, min, max}): 0-2499 No Award (AWARD), 2500 Pupil (RIBBON),
  5000 Student (MEDAL), 10000 Honor Student (RING), 15000 Scholar (TROPHY), 20000+ Master Scholar Of
  MindMaze (TROPHY). Progress shown as x/20000.

## Rooms (table at 0x6b4550, 0x38-byte records)
Record: +0 name ptr, +4 x, +8 y, +0xc/+0x10 0, +0x14..+0x20 click rect (l,t,r,b, relative to x,y),
+0x24 flag (1 = transparent?), +0x28..+0x2e 7 slot bytes (1 = occupies slot), +0x30 low byte flags
(1 = uses door slots, 2 = no floor covering), +0x32 high word = object id, +0x34 cached bitmap.
Group pointers 0x6b54a0..: doors 0x6b4550 (5: N, S, right, left, tower), floors 0x6b4668 (4),
ceilings 0x6b4748 (3), walls 0x6b47f0 (2: DUNRM, PLNWALLS), characters 0x6b4860 (22), furniture
0x6b4d30 (34 = 16 pieces + PIC1-1..9 + PIC2-1..9, the area pictures).
Room generation FUN_0052f890(room, force): room[4] = door flags (0x10 = special room: no decor).
room[0] = rand%2 wall set, [1] = rand%4 floor (0x28 = none), [2] = rand%3 ceiling, [3] = character
(0x28 = none; 25 tries for one whose slots fit the doors), then furniture bitmaps room+0x28 (1 item) and
room+0x20 (5 passes x 25 tries), slots checked by FUN_0052fbc0 / reserved by FUN_0052fc60.
Draw one object: FUN_0051c3d0 / FUN_0051c410 (click rect).
Story/scene table 0x6b55e0 (0x2c-byte records): OPEN1/OPEN2 (intro pages), END1..END6 (endings).
Sounds table 0x6b41bc: INTRO, BGLOOP1-3, QUESTION (MIDI), AMB1-23, BIRDS, DOOROPEN, FOOT1-5, KNOCK,
MATCH1-2, RIGHT, WRONG. Music file name pattern "%d-%d.mid" also exists.

## Rebuild (pass 1: Windows exe) - 2026-11-16
- `src/core/`: portable C core (`mm_game.c` maze/rooms/questions/turns/UI ported from the functions named in its
  comments, `mm_questions.c` question sources, `mm_sources.c` source registry, `mm_settings.c` settings screen,
  `mm_rich.c` italic text, `mm_util.c` MSVC rand + BMP + drawing, `mm_tables.c` / `mm_tables.h` table loader) and
  `src/platform/win32/win32_main.c` (window, GDI text, waveOut mixer, MCI MIDI, ini).
- Tables (object records, ranks, speech texts, strings) are read from ENCARTA.EXE / ENCARTAR.DLL at extraction time
  by `tools/extract/extract.py` into `data/tables.json` and loaded at start by `src/core/mm_tables.c`. (Earlier
  builds had a generated header `mm_tables.h` with the data compiled in; it is gone.)
- `make win32` builds `MindMaze.exe` and the data folder `data/` (art, MIDI, panel bitmaps res/, WAVs converted
  ADPCM -> PCM, mindmaze.db/idx, questions.json). `make test` runs the tests.
- Settings: `mindmaze.ini` [MindMaze] (UTF-16): `questions=original | json:<file>`, `reference_url=...%s`
  (replaces "refer to Encarta"), `seed=<n>` (repeatable mazes for testing), Music, Sound, MindmazePlayer<n>.
- Tests: `tests/test_core.c` (includes mm_game.c, stubs the platform). `make test` runs it on synthetic fixtures;
  `make test-data DATA=data` runs it on extracted data. Run tests with a memory cap (2026-11-16 a JSON reader bug
  allocated 4 MB per string; an uncapped test run filled the system drive through Docker/WSL).
- Wine check at 1920x1080: the window is scale 2, client origin (324,125).
- Speed: MCI MIDI runs on a music thread (open/close blocked the game for hundreds of ms on every question);
  redraws only when the clickable target under the pointer changes (clickable_at); blit uses a column table
  (6.9 ms per full frame at 2544x1748, tests/test_core.c benchmark); HD PNGs preload one per tick.
- HD pictures shown above 2x (e.g. 4K, scale 4) are enlarged bilinearly (blit_smooth), not as 2x2 blocks;
  originals stay nearest. Radio buttons (mm_disc) and level numerals (FONT_LEVEL, Palatino Linotype Italic) are
  drawn in code, crisp at any scale (generated output for those tiny bitmaps was too soft).
- Question text: HTML entities decoded at load (mm_questions.c decode_entities, ~60 kinds), <it>/<i> drawn in
  italics by mm_rich.c (banner + answer buttons), other tags shown as plain text; mm_plain() strips tags for
  message boxes / web search. Points number: FONT_POINTS (Times New Roman bold 24 px, larger than the original).
- Clock at 0: nothing happens, as in the original (decision 2026-11-16).
- Hand cursor over everything clickable (clickable_at; the original only did room hotspots).
- Panel look: see "Panel vs UPSELL_MINDMAZE screenshot (2026-11-16)" below (first pass had TNR 16 name, light
  buttons, #34322D underlines - all replaced).
- Deliberate changes: 29049 "Click the stairs" shown (original bug never shows it); Encarta article links ->
  web search; menus/message boxes drawn in-game.

## Flow audit against the decomp (2026-11-16)
Verified by reading Ghidra pseudocode + objdump disassembly of ENCARTA.EXE; changes in mm_game.c / win32_main.c
cite the FUN_ address. Headless tests (tests/test_core.c test_flows) cover them; Wine run: logon -> grating ->
doors -> room works (no sound device in the container, so music was NOT heard there).
- The wooden stage under the star box in the king's room is part of KING1.DIB (219x258 at 374,26). It is not
  clickable in the original either: KING1's rect (58,27)-(141,237) covers king + box only (speech id 15).
- Room hotspots (FUN_0051c190 0x51c33e..): registered furniture A, furniture B (mode 0), character, doors
  (mode 1) into a list; the click test FUN_0052f840 takes the FIRST rect holding the point. The rebuild used
  last-first with doors last -> FIXED (hot_at). Click jump table 0x51c760/0x51c778 matches room_click.
- Clicking a hotspot while a question is open: MessageBeep(0) (0x51c6fc) -> FIXED (plat_beep).
- Red frame: the only one promised was the selected level's frame (FUN_0051f530 sets the selected button's
  colours 5/6/7 to red 0x972938); it is drawn. The original screenshot (UPSELL_MINDMAZE) shows the same frame and
  NO award picture for a new player: rank 0 draws AWARD.DIB, a plain 66x65 grey square, SRCCOPY at (529,35)
  (FUN_005201a0 via FUN_0051eb00) - the empty area is correct.
- Music (FUN_0051adb0 state machine, WMP controls game+0xa8 music / +0xac question; end events 0x4f52b0 /
  0x4f52f0, dispid 0xbc4, act on NewState 0 or 8): state 1 (logon OnCreate 0x521070) stops all, plays INTRO.MID
  ONCE and sets state 2. Entering the castle (FUN_00521e60) does NOT switch music: INTRO plays on, and its end
  starts a random BGLOOP (FUN_0051b0d0 mode 0 -> FUN_0051b340). A loop's end picks a DIFFERENT loop
  (FUN_0051b4f0) via a "<from>-<to>.mid" transition on the question player (none ship; the rebuild goes straight
  to the new loop). Question: state 3 pauses the loop, plays QUESTION.MID, which restarts on its end; state 4
  stops it and resumes the loop. Music/Sound menu toggles rerun the current state (in state 2 that starts the
  next loop). The rebuild had INTRO looping on the logon, a switch to BGLOOP on entering, and loops could
  repeat -> FIXED (music_update/bg_next/mm_music_ended). The playState tests in the original compare with 1/2
  (MP 6.4 values) on a WMP7 control; the intended meaning (2 = playing, 1 = paused) is ported.
- Ambient timer (panel timer 2): started on entering (+0x24 = 1), killed by every room redraw (state 5) and by
  questions, restarted only when music is on and a loop has started (+0x18 != 0); not re-armed when sound is
  off (0x51b770) -> FIXED. So: no ambient sounds while INTRO still plays after the first move, none with music off.
- Music thread (win32_main.c): a switch closes the old track first; stale "ended" posts are dropped by a
  per-channel generation; pause/resume now works for the question channel too (window deactivation).
- Window deactivated (FUN_004f40f0 / logon 0x520a70): music paused, question clock stopped, answers hidden
  (FUN_005204d0); reactivated: resumed, clock restarted (FUN_00520550) -> ADDED (mm_activate).
- Door with no question left (FUN_0051ccb0 after FUN_00522030 returns 0): message, then the player walks
  through anyway (door not marked open) and the turn passes -> FIXED. Answers are shuffled BEFORE the question
  is picked (rand order) -> FIXED. Second wrong answer: the question (banner, answer panel, music) stays until
  the box is closed (FUN_0051f750) -> FIXED.
- Matches (0x51e250): blocked only by the current player's own lit map (maze+0x2c), timer 1 restarts; timer end
  (0x51e2a0) plays MATCH2 only if the current player's map is lit -> FIXED.
- Logon: start commits a name being typed (FUN_00520ad0 -> FUN_005215d0); New player commits first
  (FUN_005212b0); EM_LIMITTEXT 12; duplicate test case-insensitive (LB_FINDSTRINGEXACT), edit reopened after the
  message; a new name is saved at once as "0,0,name" (FUN_0051bb80) -> FIXED. Player order = list order (OK).
- New Game (0x4f4cc0) does nothing while the logon is shown -> FIXED; New Game / Quit have NO confirmation
  in the original (panel 0x718c/0x718d and menu 0x7185/0x7186 go straight to 0x4f4cc0 / 0x4f4df0). OK.
- Enter (FUN_004f4460): doors screen picks rand()%4 (rand consumed -> FIXED); in a room only the stairs
  (FUN_0051ce10) - the rebuild's "Enter closes speech" was removed.
- Left as is: Enter in the stairs room climbs even with a question open in the original (bug, not copied);
  the per-turn list rebuild when the next player's level differs (FUN_0051f530 from FUN_004f3750, reshuffles
  with srand(ticks)); message boxes are in-game (no window deactivation around them).

## Panel vs UPSELL_MINDMAZE screenshot (2026-11-16)
Compared element by element against the real screenshot stored as a JPEG resource in ENCARTAR.DLL
(JPEG/UPSELL_MINDMAZE), which is the reference for the panel look. How: a temporary test hook (removed) built the
screenshot's state (player Tourist, PLNWALLS + FLRCOVR1 + CEILING1, JESTER4, CANDLE2, WALLTRE1/2, VASE2,
Art/Language/Literature, level 1, score 281, the same map) and the exe was rendered in Wine at 1x with original
art; numeric profiles and least-squares colour fits were run offline.
- Registration (room correlation): the JPEG is the full 636x437 game area scaled by 420/636 = 0.6604 (both axes),
  shot = 0.6604 * game - 0.17 (x) / - 6.78 (y): the top ~7 game px are cut off, nothing else.
- Fonts are in ENCARTAR.DLL RCDATA "FONT" records (key 0x65 height, 0x69 weight, 0x71 pitch/family, 0x72 face
  string 20010 Arial / 20011 Times New Roman / 20015 Tahoma); button looks in RCDATA "SHRL" style records
  29353-29357 (key/colour pairs) set by FUN_0051e630 (vtable +0x14c): 29353 New/Quit, 29354 level, 29355
  answers/refer, 29356 matches, 29357 radios. Label tables: 0x6b4498 rects, 0x6b4508 ids, 0x6b4518 alignment
  (0 left, 1 centre, 2 right), 0x6b4534 fonts.
- Matching already: panel colour #D9D6C5 (shot D8D5C4), room/panel boundary (no line, y 301), map geometry, frame,
  unvisited cells (1 px #EBEAE1), visited outline + door gaps, stairs cell, match bitmaps, level squares and the 1 px
  red selected frame (fit), "Choose Area of Interest"/"Level" (Tahoma 11 bold, positions).
- Fixed (each change has a comment in mm_game.c / win32_main.c citing the screenshot or FUN_ / RCDATA):
  underlines 1 px BLACK and 276 / 35 px long (FUN_0051eb00 default pen, LineTo end excluded; were #34322D, 1 px
  longer); all panel text black (bold strokes in the shot reach luminance 5); match buttons get a 1 px #A09683
  frame (fit per side); radio = bitmap 29164 geometry: 12 px circle at button +1,+1 (centre x+7), 1 px #A09683 ring,
  6 px black dot (was 11 px, 4.4 px dot), label text at x+18 (was 16); level numerals: Palatino italic -25 (digits
  17 px like the bitmaps, was -22), black, placed by ascent on baseline by+21 centred on x 469 (new
  plat_text_px_ascent); Progress line Arial 12 (RCDATA 4916), top-aligned, "Progress:" centred (was Tahoma 11,
  vertically centred = 3-4 px low); name Times New Roman 18 (RCDATA 4931) top-aligned at y 4 (was 16 px centred);
  New Game / Quit: face = panel colour, 1 px black frame (fit; was #F2F1EA face, #2E2B25 frame; panel_button(),
  logon/message-box buttons unchanged); question mode (not in the shot, from FUN_0051edb0 WM_SETFONT): answers
  Times New Roman 14 (FONT_ANSWER/_I, was Tahoma 11), letters A.-D. Times New Roman 18 (was Tahoma bold).
- Current-room marker (requested change): FUN_0051da70 has NO Ellipse call (disassembly: 5 FillRects) - a 6x5
  #972938 block at (x+3,y+3) with its corners cleared. Now mm_ellipse() (mm_util.c, 4x4 supersampled AA) with the
  same box: centre (x+6, y+5.5), radii 3 x 2.5, smooth at 2x/4x (checked in Wine at 1920x1080).
- Still different: in Wine every font is a substitute (no Tahoma/Arial/Times/Palatino), so text widths/shapes
  differ only there ("Art, Language, and Literat", "Physical Scienc", "281/2000(" are clipped in Wine; the
  original's Tahoma text fits: in the shot the longest area name is 144 px in a 147 px box). Hover looks of
  New/Quit (style 29353 suggests grey #8E8E8E face + #E3E3E3 text on hover) and the exact button text baseline
  (shot ~1-2 px higher than DT_VCENTER) are not decoded - the button paint code was not found; red hover text kept.
  Radio vertical centre may be 0.5 px higher than the original (fit was split between +1 and +2). Intentional: HD
  art, hand cursor, bigger points number, AA radios / font numerals, JSON questions, debug cheats.

## Art tools (HD pictures)
HD pictures are generated art made from the original pictures. They are local working data (not in the
repository); the tools and the game's loader are.
- `tools/art/mm_sheets.py <mindmaze>` writes upscale sheets (768x512, magenta background, 8 px clear zone) plus a
  `.txt` (box + description per picture) and `map.json`. 28 sheets / 106 pictures: backgrounds, layers, doors,
  characters, furniture, pictures, overlays, ui, pages.
- `tools/art/mm_cut.py <mindmaze>` cuts the generated sheets into per-picture PNGs (2x, alpha). Paint outside a
  sprite's box is KEPT (deliberate): drawn pixels in the clear zone connected to the box grow the picture;
  `bounds.json` {name: dx, dy (<=0), w, h, grown} in 2x px relative to the original box. Pictures whose original
  has a flat edge colour (panel buttons, award squares, Points seal) get their HD background repainted in exactly
  that colour (match_flat_background); pink fringes on the ui strips take the original pixel.
- Cut rules (2026-11-16 cleanup, after an audit found holes and a warm cast): key by DISTANCE to magenta
  (purples/blues stay solid); the original's outline decides solidity (inside it, shrunk 2 px, always opaque;
  key colour left there is filled from the original); outline pixels mixed with magenta take the original
  colour; per-picture colour match (mean + limited contrast) to the original; "no new pink" (light pink
  where the original has none -> original colour / clear; widened to any magenta tint - purple outlines),
  background repaint feathered 2 px (no stair-steps from the 2x original mask). `tools/art/mm_audit.py <dir>`
  re-checks everything (holes, semi-transparent, extra, colour drift, alignment) and writes side-by-side compare
  sheets.
- Game: the build copies the HD PNGs to `data/hd` + bounds.txt ("key dx dy"). mm_util.c mm_draw_img /
  mm_draw_img_part draw the HD picture (WIC PNG decode in win32_main.c) at 2x with the offset, alpha-blended;
  hotspots/layout always use the original sizes. Toggle: menu "Use HD Art" / F2, ini `hd_art`.
- Icon (2026-11-17): `tools/art/make_icon.py` -> `src/platform/win32/mindmaze.ico`; a detailed icon for
  256/128/64 and a simple one (white background keyed transparent by border flood fill) for 48/32/24/16; the .ico
  is written by hand (PNG entries). Fallback: square crop of the sign-in picture. Encarta's own TICON_MINDMAZE
  lives in the .AKC data files (unreadable format); ENCARTAR.DLL has only globes.

## Widescreen (2026-11-16: sheets, extensions, cutter, in the game)
- Plan: the view grows from 636 to 636 + 2*64 = 764 logical px (about 16:9 with the panel); room origin moves
  right by 64; the panel (code-drawn) just gets wider. Pictures that reach a side edge of the 636 view need
  painted extensions; everything else is placed as before.
- `tools/art/mm_wide_sheets.py <dir>` writes `wide/sheets/*.png+.txt` and `wide/map.json`; results go to
  `wide/out_gen/` (2x). One picture per sheet; the strip to paint is flat green #00FF00 (64 px at 1x) on the
  side(s) where the picture has opaque pixels on the view edge (found from the alpha, so the list is automatic).
  19 sheets: 6 backgrounds (logonscr, grating, rmofdoor, atrium, dunrm, plnwalls; both sides), 7 layers
  (ceiling1-3, carpet1-2, flrcovr1, chkfloor; both sides), 6 objects (duke1, lady1 left; alchem, secnldy2,
  suitarm1, vase2 right). mm_sheets.py runs main() only as a script (mm_wide_sheets imports its descriptions and
  loader).
- 2026-11-16 update: STRETCH vs FILL per picture. Only LOGONSCR stretches (it has its own ornamental frame:
  12 px bars, corner rosettes 34 px wide at the top / 28 at the bottom, 26 / 44 rows; the frame moves to the new
  edges, green gaps inside it). Everything else FILLs (picture keeps size and place = wider camera, so hotspots and
  layers stay put). Sheets carry GEOMETRY lines: room vanishing point (318,118) and the side-wall/floor lines
  (0,251)->(-64,278), (635,250)->(699,278), measured on PLNWALLS; layer outline edges fitted on the alpha
  (first 30 columns, stops when the edge jumps >3 px) and extrapolated, or the point where they leave the
  picture; object border crossings. masks/<sheet>.png = paint area; map.json has mode and per-row-band split
  columns for the cutter. The generation prompt had strict alignment rules and a measured check with max one
  regeneration.
- 2026-11-16 results in (19/19, out_gen/CHECK.txt: geometry FAIL on most, but every result registers to
  its sheet at identity (no scale/shift beats it) and room composites line up: floor lines, beams, rugs,
  checkerboard continue across the seams).
- Cutter `tools/art/mm_wide_cut.py <dir>` -> `wide/hd/<key>.png` (2x), `<key>_1x.png` (1x, hard alpha),
  wide.txt "key left right". Extension = painted result inside the mask; keyed with mm_cut.key_alpha/unmix; per
  side a (dy,dx) shift up to +-8 px (2x) found by matching the repaint of the centre to the centre along the seam
  (a shift at the search limit = no match, kept at 0); per side/row colour offset (band 12 px, 41-row smoothing);
  cross-fade 24 px (2x) / 8 px (1x) into the centre. Opaque pictures: outer 4 rows/columns of the result repeated
  inwards (they carry magenta from the sheet: a pink line along the bottom of the view). mm_cut.py runs main()
  only as a script. (This first version was simplified in the later rounds below.)
- Game (CHANGE): MMCanvas.ox = left margin in logical px; mm_fill/disc/ellipse/blit_k/blit_smooth and the win32
  text functions add it, so the 636 view keeps its coordinates and extensions draw at -64..-1 / 636..699.
  mm_draw_img uses the wide picture (HD or 1x by the HD toggle) at x - left when widescreen is on; panel and
  screen background fill the full width, panel contents stay centred; message boxes/stories centred as before.
  Toggle: menu "Widescreen" / F3, ini `widescreen` (default on when data/wide exists); plat_view_changed()
  resizes the window (keeps the scale if it fits; maximized windows keep their size); mouse x maps back through
  the margin (floor division). Wide pictures preload after the HD ones (mm_wide_preload_step).
  The build copies the wide pictures to `data/wide`.
- Verified 2026-11-16 in Wine (1920x1080, scale 2): logon (stretched) -> grating -> room of doors -> rooms in
  wide HD and wide 1x (F2), F3 off/on resizes the window 1528 <-> 1272 px; headless tests pass. Not checked:
  real Windows, maximized/fullscreen, every wall/layer/object combination (seams are faint at 2x on the plain
  plaster wall and at the atrium's right window).

- 2026-11-16 (second round, after a test run; reported problems: drawing the original background on top of the
  new ones gave odd scenes, the intro screen was cut off, full screen did not fill the screen). Changes:
  - FILL pictures are now the generated painting only, in the box (2x; 1x = it scaled down), no game picture under
    or over it: all FILL results register to their sheets exactly (bbox and diff checks), so nothing to stitch.
  - logonscr: the generator painted it over the whole 1536x1024 canvas (frame 16 px higher, an extra brick row and
    paving under the tablet, bottom frame ~110 px lower), so a crop cut the bottom frame off. Now: HD logon in the
    middle with the frame moved out (sheet layout), gaps from the generated rows found per 24-row block (Viterbi
    path: match error along the gap + 0.25/px change + 0.03/px away from 0; result -16 at the top frame, 0 through
    the middle, up to ~+136 at the bottom frame), short 6 px (2x) / 2 px (1x) fade so the tablet text stays.
  - Display (win32_main.c): canvas drawn at the next whole scale up and shown with HALFTONE StretchBlt at the
    largest proportional size; within 3% of the window's shape it fills the window. F11 / Alt+Enter / menu
    "Full Screen": borderless full screen on the window's monitor (menu hidden). Mouse maps through the shown
    size. Verified in Wine 1920x1080: full screen fills the whole screen, clicking level 2 selects it, F11 back
    restores the window.

- 2026-11-16 (third round; reported: the main screen still showed both the new and the old background, and
  objects on the sides of the room looked wrong in the middle of the room). Changes:
  - tools/art/mm_wide_cut.py simplified: every wide picture is the generated painting only. logonscr = the whole
    generated canvas fitted to 1528x874 (15% flatter; door, pennant, tablet still under their click areas - Wine:
    the door click enters the castle). The HD-centre / stitching / row-alignment code is gone.
  - Side objects: tried moving side furniture out by 64 px (first by a centre rule, then an explicit list); the
    placement was judged off in both rounds and the original positions were asked for (2026-11-16). Reverted: no
    shifting; vase2 is no longer extended (SKIP in the cutter): at the new edge it is cut by the view like in the
    original. Furniture has no click areas, so only drawing moves. Doors (DOORLT/DOORRT) are not moved: they are
    painted into the side walls' perspective and have fixed click areas (DOOR_HOT).
  - Verified in Wine: logon, rooms (broom, wall tree and banners at the view edges), question banner.

- 2026-11-16 (fourth round): logonscr regenerated (sheet now centred, y 37-473, after the first result filled
  the whole canvas; the frame was also drawn across the path: in the original the bottom band stops at
  the pillar bases, x 180-452 has no frame). Two versions were generated; version A (ledge railing runs the full
  width) was picked. The cutter now crops logonscr like every sheet; check_bounds() warns when a result paints
  more than 12 sheet px past its box.

## Question sources, seen list, online top-up, custom questions (2026-11-17, CHANGE)
- Settings screen on the logon page ("Settings..." under the player list; src/core/mm_settings.c): sources on/off
  (Original MindMaze 8,020 / each pack in data/packs / My questions / Online top-up), "Skip questions a player
  has already seen" (default on), "Forget seen (N)", "Add a question..." form (question <= 300 chars, correct +
  3 wrong answers <= 40 chars each, area < >, level 1-4; Tab / Enter / Ctrl+V paste). Done applies (reload);
  at least one source must stay on; a selection that loads nothing falls back to the original.
- 2026-11-17: "Widescreen view (F3)" checkbox on the settings screen (calls mm_cmd(CMD_WIDE), same as F3 / the
  menu; plat_view_changed now also updates the menu check). Verified in Wine: unticking resizes the window to the
  original aspect at once.
- src/core/mm_sources.c: registry (original, pack:<id> from data/packs/index.txt written by the build, custom,
  online, file = an old questions=json: setting), ini "sources" (comma ids), "skip_seen". User files next to the
  exe: custom_questions.json, online_questions.json (same mindmaze-questions-1 format), seen.txt
  ("player<TAB>key"). Online top-up: when a list has < 10 unseen questions (list_build), one GET (WinINet
  thread, plat_http_get) to Open Trivia DB or The Trivia API (alternating per area/level) for a category of that
  area (data/packs/online_map.json) and the level's difficulty, 30 questions; added to the bank
  (mm_questions_add, lists only grow so the picker's indices stay valid) and saved to online_questions.json.
  >= 6 s between requests; an area/level whose fetch added nothing waits 10 minutes.
- src/core/mm_questions.c: load spec "a|b|c" (original, json:, user:), key = FNV-1a of the lower-cased
  letters/digits of the question text; a question already loaded from an EARLIER source is dropped (a source
  keeps its own repeats: the original has 40 texts twice). mm_questions_add, mm_questions_count_src, generation
  counter.
- mm_game.c picker: list_build uses the nearest level when (area, level) is empty (packs have no level 4);
  with skip seen it lists only unseen questions (all seen -> the whole list again); list_take rebuilds when
  questions were added or (skip seen) when the list wraps; start_question marks the question seen. With only the
  original source and skip seen off the picker is as before (test_flows runs that way).
- Tests: test_sources (two sources + duplicates, seen list, start over, added question, level fallback).
- Verified in Wine 2026-11-17: settings screen, source toggle, form typing/Tab. Not yet: online fetch.

## Question packs (2026-11-17)
- Packs are `data/packs/*.json` (mindmaze-questions-1 + "pack" header), built by `tools/packs/` (one script per
  source, shared helper mmpack.py: entity/HTML cleanup, 4 distinct answers, answer <= 40 chars, question <= 300,
  no true/false or all/none-of-the-above, dedupe within the pack and against the original questions;
  check_packs.py prints the 9x4 area/level tables). Raw downloads are cached in tools/packs/cache/ (re-runs do not
  hit the services).
  - opentdb.json: Open Trivia DB, 4,043 questions, CC BY-SA 4.0 (difficulty easy/medium/hard -> levels 1-3).
    Shipped.
  - opentriviaqa.json: OpenTriviaQA (GitHub uberspot), 29,973, CC BY-SA 4.0 (no difficulty: levels spread by a
    stable hash). Shipped.
  - triviaapi.json: The Trivia API, 4,803, CC BY-NC 4.0 (non-commercial; levels 1-3). NOT shipped in the
    repository (only its importer tool); build it locally with the tool.
  - Wikidata pack: NOT built. tools/packs/wikidata.py exists but its run did not finish; packs that fail were
    dropped (decision 2026-11-17). Left as is.
  - packs/online_map.json (tools/packs/online_map.py): area -> Open Trivia DB category ids / The Trivia API
    categories, level -> difficulty; read by the game's online top-up.
- The build copies the packs to `data/packs` and writes index.txt (file, id, name, licence, count).
- Verified 2026-11-17: check_packs.py OK for the three packs; the game build lists them (index.txt); one live GET
  to each API returns the fields mm_sources.c parses (opentdb: question / correct_answer / incorrect_answers;
  triviaapi v2: question.text / correctAnswer / incorrectAnswers). The game's own online fetch (WinINet) has not
  run yet: needs real Windows (or a Wine run with "Online top-up" on).

- 2026-11-17 report: two scenes in a row had the same props. Not a decoration bug: FUN_0051d390 decorates every
  room once at maze creation (FUN_0052f890, no srand in between; the only srand is FUN_00522a30 in the question
  list builder), rooms never change. test_mazes now counts connected neighbours: 77,523 pairs over 2,000 seeds,
  0.04% same furniture, 0.00% identical. Likely the same room twice: after two wrong answers the player stays
  (on_wrong2_box: no move, as the original); the turn passes, and with one player the next turn is the same room.

- 2026-11-17 report: neighbouring rooms with the same people, and props lying over the door. Both are the
  original's behaviour:
  - FUN_0052f890 picks the character with rand() % 22 per room, no check against neighbours: test_mazes counts
    5.3% of connected neighbours with the same character.
  - Furniture over side doors: after the character is chosen the original clears the door occupancy when the
    character record's flag bit 0 is clear (disassembly 0x52fa2b testb $0x1,(%eax); jne 0x52fa3f; else zero the
    7-byte occupancy at 0x52fa30-0x52fa3b), also for rooms without a character (record 0x28 -> PIC1-3, flags 0).
    Furniture is then placed ignoring the doors and drawn after them (FUN_0051c190). test_mazes: 55.3% of rooms
    with a side door get furniture on that door's slot.
  - Decision 2026-11-17: fix both, permanently, no option (CHANGE in decorate(): characters of decorated
    connected neighbours are avoided, fallback from 3 upwards skips them; door occupancy is kept). test_mazes
    asserts 0 neighbours with the same character and 0 rooms with furniture on a side door's slot (2,000 seeds).

- 2026-11-17 awards: panel draws mm_ranks[award].image at (529, Y+35) when no question is open; the award changes
  only when the player climbs the stairs (stairs() -> rerank(), FUN_0051c940 -> FUN_0051c9f0, as the original),
  with "Congratulations! You now have a new award!". test_awards draws the panel for all 6 ranks (1x, original
  pictures) and checks every opaque pixel of AWARD/RIBBON/MEDAL/RING/TROPHY.DIB lands on the canvas. HD versions
  exist (data/hd/award, ribbon, medal, ring, trophy.png); HD drawing of them not checked separately.
- 2026-11-17 (report: awards cut off and not centred): the award pictures are 66x65 squares on the panel colour;
  at (529, Y+35) their bottom reached Y+100 (Progress at Y+90) and the HD squares (a lighter beige) showed as a
  box. CHANGE: the original picture's background is keyed when drawn and the picture's own bounds are centred in
  rows Y+26..Y+89 on x 562; the build keys the HD award PNGs (tools/art/key_flat_bg.py: panel colour +-34, flood
  from the border, 1 px soft edge). test_awards checks ranks 1-5: drawn, centred, clear of the Progress line.

## Status (2026-11-17)
- Web port (pass 2) still planned.
- Not yet run: the online top-up fetch on real Windows.
