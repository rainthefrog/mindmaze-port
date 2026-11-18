# MindMaze (Encarta 2006) - rebuild spec

Game rules, data structures and layout of MindMaze, the trivia game inside Microsoft Encarta 2006 Premium (US
English), as read from `ENCARTA.EXE` with SHA-256 089adc5db30653e4af2d8a37072052c0bddc80049f9943e25b8d41d95905045f.

Companion to `RE-NOTES.md` (question DB, picking, scoring, rank table, room object table). This file covers maze,
movement, rooms, screens, flow, persistence and layout. Tags: **[V]** = read in code (address given),
**[G]** = guess / not fully traced. Addresses are ENCARTA.EXE VAs. Colours are `PALETTERGB` (0x02BBGGRR) as
written in code; given here as `#RRGGBB`.

## 0. Corrections to RE-NOTES.md
- **DB area order** [V] (button map `FUN_00520270`, picture ids, question content - area 0 questions are about
  bombsights, tractors, chemistry): `0 Physical Science & Technology, 1 Life Science, 2 Geography, 3 History,
  4 Social Science, 5 Religion & Philosophy, 6 Art/Language/Literature, 7 Performing Arts,
  8 Sports/Hobbies/Pets`. An earlier note ("0 Life Science") was wrong.
- Player `+0x24` is the selected **area button** (0..9), not a flag. `+0x1c` = award rank (0..5) [V].
- Player level (`+0x20`) is **chosen by the player** with the 4 level buttons; it never rises by itself
  [V `FUN_0051f530`].

## 1. Data structures

### Game object (CView, ctor `FUN_004f2f60`, global `DAT_006c2fe0`) [V]
| off | meaning |
|---|---|
| +0x8c | COM object created in OnCreate (`FUN_004f45c0`) |
| +0x90 | sound object (below) |
| +0x94 | CFile of mindmaze.db (opened lazily by `FUN_004f3490`, string 29005) |
| +0xa4/+0xa8/+0xac | 3 hidden media-player ActiveX controls (ctrl ids 0x7276 effects, 0x7277 music, 0x7278 question music) |
| +0xb8 | current player index 0..4, 6 = none |
| +0xbc CPtrArray players (+0xc0 data, +0xc4 size = 5) | |
| +0xd0 | current question (0 = none) |
| +0xd4 | short: door direction whose question is pending |
| +0xd8 | entrance door chosen in the "room of doors" (0..3) - written only, never read [V by xref search] |
| +0xe0/+0xe4/+0xe8 | saved music/sound flags while the view is hidden (`FUN_004f5360`) |
| +0xf4 | control panel window (`FUN_0051e2e0`, 0x32d0 bytes) |
| +0xf8 | logon window (`FUN_00520860`) |
| +0xfc | grating window (`FUN_00521890`) |
| +0x100 | room-of-doors window (`FUN_00521bb0`) |
| +0x104 | room view window (`FUN_0051c030`) |

### Player (0x28 bytes, ctor `FUN_0051b880(name, slot)`) [V]
`+4` per-player question list object (0x678 bytes, see RE-NOTES), `+8` name (CString), `+0xc` slot 0..4,
`+0x10` settings index n of `MindmazePlayer<n>` (0 = never saved), `+0x14` score, `+0x18` Maze*,
`+0x1c` award rank 0..5, `+0x20` level 0..3, `+0x24` area button 0..9 (0 = All Areas). New player: all 0.

### Maze (0x44 bytes, ctor `FUN_0051d390(w, h, rooms)`; always called `(10, 10, 40)`) [V]
| off | meaning |
|---|---|
| +4, +8 | width 10, height 10 |
| +0xc, +0x10 | current x, y |
| +0x14, +0x18 | start x, y |
| +0x1c, +0x20 | stairs (tower) room x, y |
| +0x24 | first room |
| +0x28 | `Room* grid[w*h]`, index `y*w + x`, NULL = no room |
| +0x2c | map reveal flag (match lit) |
| +0x30..+0x40 | 5 ints: match i still available (all 1 at creation) |

### Room (0x34 bytes, zeroed at creation) [V]
| off | meaning |
|---|---|
| +0x00 | wall set 0..1 (DUNRM, PLNWALLS) |
| +0x04 | floor 0..3 or 0x28 = none |
| +0x08 | ceiling 0..2 or 0x28 = none |
| +0x0c | character 0..21 or 0x28 = none |
| +0x10 | door bits (8-byte bitset, index = door record): bit0 N (y-1), bit1 S (y+1), bit2 E (x+1, "right"), bit3 W (x-1, "left"), bit4 stairs/tower |
| +0x18 | opened-door bits, same bit numbering (byte) |
| +0x20 | furniture bitset A (8 bytes, bit = furniture index 0..33) |
| +0x28 | furniture bitset B (drawn in front of the character) |
| +0x30 | visited (shown on map) |

Direction numbers used everywhere: `0 = N, 1 = S, 2 = E, 3 = W` (= door bit / door record index).

## 2. Maze generation `FUN_0051d390` [V]
```
w=h=10; R=40; pct = R*100/(w*h) = 40          # computed once
start = (rand()%w, h-1)                         # bottom row, random column
grid[start] = new Room; cur = start; steps = 0
while R > 0:
    free = neighbours of cur inside grid with no room (N:y-1, S:y+1, W:x-1, E:x+1)
    n = |free|
    if n > 0 and (n > 2 or steps < 5 or rand()%100 >= pct):
        repeat d = rand()%4 until d in free     # 0 N,1 S,2 E,3 W
        new room at cur+d; cur.doors |= bit(d); new.doors |= bit(opposite(d))
        cur = new; R -= 1; steps += 1
    else:                                       # dead end or 40% chance after 5 steps when n<=2
        repeat (x,y) = (rand()%w, rand()%h) until grid[x,y] != NULL
        cur = (x,y); steps = 0
# total rooms = 41 (start room + 40)
if cur.x == start.x or cur.y == start.y:       # last walker room too close: use first room in row-major order
    cur = first non-NULL grid cell scanning y=0.., x=0..
while cur.doors & N: cur = room to the north   # stairs room never has a north door
cur.doors |= 0x10; tower = cur
cur = start; start.visited = 1
for each room (row-major): FUN_0052f890(room, force = (room is start))
maze+0x2c = 0; matches[0..4] = 1
```
Player starts in `start` [V]. A new maze is built for a player on each stairs climb (`FUN_0051ba10`) [V].
Number of floors is unlimited; the game ends by score, not by floor count (section 6).

## 3. Room decoration `FUN_0052f890(room, force)` [V]
Object record table: see RE-NOTES (0x38-byte records; slots = 7 bytes at +0x28; flags byte +0x30; id word +0x32).
Counts [V 0x6b54b8]: doors 5, floors 4, ceilings 3, walls 2, characters 22, furniture 34.
```
if doors & 0x10:  wall = 0 (DUNRM); floor = ceil = char = 0x28; done (no furniture)
wall = rand()%2; floor = rand()%4; ceil = rand()%3
occ[7] = 0; if doors&4: occ[3]=1; if doors&8: occ[2]=1
occ[4] = (doors&0x10)!=0; occ[6] = occ[4] || (doors&1)
ndoors = popcount(doors & 0x1f)
if ndoors < 2 and !force: char = 0x28
else:
    char = 0x28; 25 tries: c = rand()%22; if no slot conflict(c, occ): char = c (stop)
    if none: 25 tries with occ' = {1,0,0,0,0,0,0} (only slot 0 busy); if still none: char = 3 (DUKE3)
C = charTable[char]            # NB: for char == 0x28 the code reads record 0x28, i.e. furniture[18]
                                # (PIC1-3: slots 0100010, flags 0) - reproduce for identical layouts
if C.flags & 2: floor = 0x28
if !(C.flags & 1): occ = all 0 # door occupancy dropped
occ |= C.slots
if occ[0] == 0:                 # front slot free: one item into bitset B (+0x28)
    occ[1] = 1
    25 tries: f = rand()%34; if no conflict(f, occ): occ |= F.slots; B |= bit(f); stop
repeat 5 times:                 # up to 5 items into bitset A (+0x20)
    occ[0] = 1; occ[1] = 0
    25 tries: f = rand()%34; if no conflict and bit f not yet in A: occ |= F.slots; A |= bit(f); stop
```
`conflict(rec, occ)` = exists i<7 with `occ[i]==1 && rec.slots[i]==1` (`FUN_0052fbc0`); mark = `FUN_0052fc60`.
Slot meaning [G, from the records]: 0 foreground/front (SDOOR), 1 back wall, 2 left wall, 3 right wall, 4 tower,
5 floor, 6 back-wall centre (NDOOR).

CHANGE (decision 2026-11-17, `decorate()` in the rebuild): characters of already decorated connected neighbours
are avoided (fallback from 3 upwards skips them), and the door occupancy is kept when the character record's flag
bit 0 is clear, so furniture never lies on a side door's slot. The original does neither (see RE-NOTES).

## 4. Room view, drawing and hotspots
Room view window 636x301 at (0,0) of the 636x437 MindMaze area [V `FUN_004f3be0`]. All bitmaps are drawn with
palette index 0xFE transparent (cyan) [V `FUN_0052f390` -> `FUN_00417f60(...,0xfe)`]. Draw call `vtbl+0x158
(bitmap, {x,y,w,h})`: w/h 0 = bitmap size, x/y -1 = centre, -2 = right/bottom align [V `FUN_0052f390`].

Draw order `FUN_0051c190(room)` [V]:
1. If current player's award == 5: draw `ATRIUM.DIB` at (0,0), single hotspot id 0x1E over the whole view, stop.
2. wall[room.wall]; floor (if != 0x28); ceiling (if != 0x28).
3. every door with its bit set (record x,y from table; NDOOR 273,72; SDOOR 296,254; DOORRT 521,79; DOORLT 29,80;
   DOORTOW 241,0).
4. furniture bitset A; character (if != 0x28); furniture bitset B; then SDOOR again if bit1 set (foreground).
5. Hotspots: furniture A and B with mode 0, character with mode 1, doors with mode 1 (`FUN_0051c410`):
   - rect = `(x+rect.l, y+rect.t, |rect.r-rect.l|, |rect.b-rect.t|)` from record +0x14..+0x20 (record-relative);
   - mode 0: only if rect.l != 0 and rect.t != 0 (so only PIC1-x/PIC2-x furniture is clickable);
   - mode 1 with empty rect: whole bitmap, half width if record +0x24 == 1.
   Hotspot id = record word +0x32. Cursor over a hotspot = hand (`DAT_006c2c84`), else arrow [V `FUN_0052e530`, `0x51c900`].
   The click test (`FUN_0052f840`) takes the first rect in registration order that holds the point.

Click dispatch `0x51c680` (room vtbl+0x16c), only if a player is current [V]:
- question open (`game+0xd0`) -> `MessageBeep`, nothing else.
- id 0..3 -> door click `FUN_0051ccb0(dir)`.
- id 4 -> stairs `FUN_0051c940`.
- id 7..28 -> character speech `FUN_0051ce70(id)`.
- id 0x1E (atrium) -> the closing speech text (speech id 29, a congratulation on completing the quest), sets
  `room+0x8c = 1`.
- id 31..39 -> picture: MessageBox asking whether to see an article about the area (Yes = open an Encarta article
  picked at random from `Area<n>.lst`, `FUN_0051cae0`/`FUN_00530810`). Area name table 0x6b425c, index id-31:
  31 Life Science, 32 Geography, 33 Physical Science and Technology, 34 Social Science, 35 Religion and Philosophy,
  36 Art Language and Literature, 37 Performing Arts, 38 Sports Games Hobbies and Pets, 39 History.
  PIC1-k / PIC2-k (k = 1..9) both show DB area k-1. Nothing is granted.
- ids 5, 6, 29 and others: no action.

Left click on the room view (`0x51c7a0`, WM_LBUTTONDOWN) [V]: if a speech panel (not a question) is shown it is
closed and nothing else happens; if it was the atrium text (`+0x8c`), the ending story opens (section 6).

Speech text `FUN_005308c0(id)` [V]: random entry of a per-id table: US English (LCID & 0xFC00 == 0x400) table
0x6b5880 / counts 0x6b58e0, otherwise 0x6b5940 / counts 0x6b59a0 (both indexed id-7, char* arrays; 4-5 texts per
character, 1 for id 29). Several characters share texts (DUKE1-3, JESTER1-4, SERVANT1-2, SECNLDY1-2, SUITARM1-2).
Extract with: `ptr = u32(table + 4*(id-7)); n = u32(counts + 4*(id-7)); text[i] = cstr(u32(ptr+4*i))`.
`\n` in the texts is a literal backslash-n.

Text overlay on the room view (`FUN_0051ced0(text, mode)`, painted by `FUN_0051d0d0`) [V]: picture blitted at
(67, 0) over the room, text drawn with DrawText(DT_WORDBREAK), transparent background, font resource 0x7D01.
| mode | picture | size | text rect (view coords) | used for |
|---|---|---|---|---|
| 0 | DIALOG.DIB (29008) | 289x240 | (99,44)-(329,216) | character speech, atrium |
| 1 | BANNER.DIB (29007) | 362x300 | (194,95)-(385,253) | question text |

## 5. Movement, doors, questions
There is no facing/turning: the view always looks "north" (back wall = N door, foreground = S door, right wall = E,
left wall = W) [V: no facing state anywhere in maze/room].

Door click `FUN_0051ccb0(dir)` [V]:
```
if game.question or !(room.doors & bit(dir)): return
if room.opened & bit(dir):              # already opened before
    move(dir); next_turn()
else:
    q = new question for player (area button, level)   # FUN_00521fa0 + FUN_00522030 (RE-NOTES)
    if q started: game.d4 = dir         # out of questions -> message, no turn change
```
(The original, when no question is left, shows the message, then lets the player walk through anyway without
marking the door open, and the turn passes; the rebuild follows this, see RE-NOTES "Flow audit".)

Question start `FUN_00522030` [V]: answers shuffled, text shown in BANNER overlay, panel switches to answer
mode (`FUN_0051fcd0(panel,q)`), seconds = 66, 1 s timer (`0x522660`) decrements to 0 and redraws the points
label = `(level+1)*seconds`. At 0 nothing happens; the answer then gives 0 points. Music state 3 (QUESTION.MID).

Answer `FUN_0051f750(panel, button)` [V]:
- correct: sound RIGHT; points = `(level+1)*seconds_left`; MessageBox 29041 (29057 if points == 1); score += points;
  `room.opened |= bit(dir)`; `move(dir)`; new room `.opened |= bit(opposite(dir))` (`FUN_0052fd10`:
  0->S,1->N,2->W,3->E); `next_turn()`; answer buttons re-enabled.
- wrong, first try: sound WRONG; MessageBox 29039 (try again); that button disabled; timer keeps running; same
  player.
- wrong, second try: sound WRONG; MessageBox 29040 with the correct answer; question closed; no move, no points,
  door stays shut (can be retried later with a new question); `next_turn()`.
No knock and no door-open sound is ever played (see sounds) [V: all `FUN_0051b660` call sites listed].

`move(dir)` `FUN_0051d900` [V]: N y-1 / S y+1 / E x+1 / W x-1 if a room exists; play FOOT1..FOOT5 at random;
old and new room `.visited = 1`; panel repaint.

`next_turn()` `FUN_004f3670` [V]: `game.b8` = next non-empty slot after the current one (wrap after slot 4,
`FUN_004f3da0`); draw that player's current room; refresh panel for that player (`FUN_004f3750`: match buttons
enabled from `maze+0x30..`, if `maze+0x2c` set -> play MATCH2 and clear it, level/area radio buttons, name, score).
The code then means to show 29049 "Click the stairs to continue your quest." when the new player stands in the
stairs room, but the test reads absolute addresses 0x1c/0xc and only runs when the maze pointer is NULL - the
message is **never shown** in the original [V 0x4f36d0]. CHANGE: the rebuild shows it.

Stairs (door id 4, DOORTOW, only in the tower room) `FUN_0051c940` [V]: same player keeps the turn;
award re-ranked (`FUN_0051c9f0`, below); a new 10x10 maze replaces the player's maze; all 5 match buttons
re-enabled; room redrawn. Enter key in the room view does the same when the current room has bit 0x10
(`FUN_004f4460` -> `FUN_0051ce10`).

Matches (5 buttons left of the map, cmd 29158..29162 -> `0x51f440` -> `0x51e250`) [V]: if match i unused and no
match burning: play MATCH1, `maze+0x2c = 1` (map shows all rooms), match i used (button disabled), panel timer 1 =
8000 ms. Timer 1 (`0x51f320` -> `0x51e2a0`): play MATCH2, `maze+0x2c = 0` of the player current at that moment. A flag
still set when a player's turn begins is cleared (with MATCH2) by the panel refresh `FUN_004f3750`.

## 6. Progression, awards, ending
- Rank table 0x6b54d0 (RE-NOTES). `FUN_0051c9f0` [V], only called on a stairs climb: new = rank whose [min,max]
  holds the score (max of rank 5 is 32000; above that no change); if changed: set `+0x1c`, MessageBox
  "Congratulations!  You now have a new award!" (title "Mindmaze").
- Award picture of the current player is drawn in the panel at (529,35) when no question is open (`FUN_005201a0`).
  CHANGE (2026-11-17): the rebuild keys the picture's flat background and centres the picture's own bounds in
  rows Y+26..Y+89 on x 562 (the original square reaches Y+100 and overlaps the Progress line).
- Score never decreases during play. Progress text "Progress: <score>/20000".
- **End**: once award == 5 (score >= 20000 at a climb) the player's room view shows ATRIUM.DIB instead of rooms.
  Click -> DIALOG with the closing speech (text id 29, a congratulation on completing the task); next click closes
  it and opens the ending story = **one of END1..END6 at random** (story record 1, random flag) [V `FUN_0052fec0`].
  Closing any story window (`FUN_00530090`) calls `FUN_004f3840`: if the current player's award is 5 his score and
  award are reset to 0, all players are saved and freed, the room/panel/grating windows are destroyed and the logon
  screen is shown again [V].
- Multiplayer [V]: 1..5 players, each with own maze, position, matches, level, area, score; strict round-robin by
  slot; the turn passes after every move (answered or through an open door) and after a second wrong answer;
  not after the first wrong answer, a stairs climb, matches, or clicking characters/pictures.

## 7. Screens and flow
MindMaze area is 636x437 [V]: logon/ grating/ room-of-doors/ room view are 636x301 (logon 636x437) at (0,0);
the control panel is 636x136 at (0,301).

1. OnCreate `FUN_004f45c0` [V]: title "MindMaze" (29009); open mindmaze.idx (29026; failure -> 29048); create the
   3 media players and the sound object; load music/sound settings (Encarta settings 0x11c music, 0x11d sound);
   `srand(GetTickCount())`; `FUN_004f3840` -> logon.
2. Logon (`FUN_00520860`, OnCreate `0x520cc0`) [V]: background LOGONSCR.DIB (29003). Hotspots: id 0
   (253,29,123x200) = start game (same as OK/Enter, `FUN_00520ad0`); id 1 (135,21,64x100) = intro story OPEN1 ->
   OPEN2. Buttons "New player" (29024) at (470,277)-(550,294), "Delete" (29025) at (556,277)-(613,294);
   multi-select listbox (ctrl 0x7274, font 0x1339) at (471,297)-(612,399), filled with every saved player
   (`MindmazePlayer1..n` until the first missing value, skipping "deleted") then "Tourist" (29043) added last.
   New player: inserts "--" at the top and an edit box at (471,297)-(595,312); Enter commits; duplicate -> 29046.
   Start: 0 selected -> 29044, more than 5 -> 29045; else create players in selection order (slots 0..n-1),
   load saved score/award by name, then `FUN_004f3970`: current = first player, create panel and the grating.
   Music state 1 (INTRO.MID) while the logon is shown.
   Details from the flow audit: the name edit is limited to 12 characters (EM_LIMITTEXT); the duplicate test is
   case-insensitive (LB_FINDSTRINGEXACT) and the edit is reopened after the message; a new name is saved at once
   as "0,0,name" (`FUN_0051bb80`); Start commits a name being typed (`FUN_00520ad0` -> `FUN_005215d0`) and New
   player commits first (`FUN_005212b0`).
3. Grating (GRATING.DIB 636x301) [V]: any click or Enter -> room of doors.
4. Room of doors (RMOFDOOR.DIB 636x301) [V]: hotspots 0 (0,0,125x300), 1 (349,51,76x191) and (197,76,59x108),
   2 (501,32,118x245), 3 (200,232,237x69); click or Enter (random 0..3) -> `FUN_00521e60`: store door, create
   the room view, draw the current player's start room, start ambient sounds. All doors lead to the same place.
5. Room view + panel (sections 4-5).
6. Stories (`FUN_0052fd80` + `FUN_0052fec0(index)`), popup captioned window, table 0x6b55e0 (0x2c each) [V]:
   | idx | pages | client size | Next button rect (ctrl 29350) | picked |
   |---|---|---|---|---|
   | 0 | OPEN1, OPEN2 | 432x380 | (397,342)-(415,359) | in order |
   | 1 | END1..END6 | 433x285 | (390,243)-(408,260) | one at random |
   Paint (`FUN_005301e0`) blits the page at (0,0); the Next button is hidden on the last page; Next = page+1;
   the window is closed with the caption close box.
TITLESCR.DIB (29027) is not referenced by any code immediate [G: unused here].

Menu built on message 0x1400 (`0x4f4f90`) [V]: "MindMaze" (29067), Turn Music Off/On (29063/29064),
Turn Sound Effects Off/On (29065/29066), Start New Game (29061), Quit Game (29062). Panel buttons New Game
(29068) / Quit Game (29069). New Game = re-navigate Encarta to the MindMaze feature (`0x4f4cc0`, restarts at
logon) [G], Quit = navigate to the Encarta home screen (`msencarta://view=ERSHomescreenView`, `FUN_004f4df0`) [V].
Neither has a confirmation. New Game does nothing while the logon is shown. Toggles store the setting
(`FUN_00406ce0(0x11c|0x11d, on)`) and update the sound object [V].

Keys [V `FUN_004f41c0`, while a question is open]: A..D = answer 1..4 (cmd 29010..29013), 1..4 = "refer to
Encarta" for answer 1..4 (cmd 29150..29153 -> MessageBox 29042, Yes = open the article of that answer, timer paused
while the box is open). Accelerator commands 25113 = Enter action (`FUN_004f4460`), 25116..25119 = level 1..4,
25114/25115 = New/Quit [V ids; key bindings G].

Debug cheats on the panel [V `FUN_00520060`, `0x51ffc0`]: Ctrl+double-left-click = jump to the stairs room;
Shift+double-left-click = score +2001; Ctrl+double-right-click = open every door of the maze; Shift+double-right-
click = toggle map reveal.

## 8. Sound and music [V `FUN_0051adb0`, `FUN_0051b660`, `0x51b770`]
Name table 0x6b41bc: [0] INTRO.MID, [1..3] BGLOOP1..3.MID, [4] QUESTION.MID, then WAV table at 0x6b41d0 indexed by
sound id: 0..22 AMB1, AMB10..AMB19, AMB2, AMB20..AMB23, AMB3..AMB9 (string order), 23 BIRDS, 24 DOOROPEN,
25..29 FOOT1..5, 30 KNOCK, 31 MATCH1, 32 MATCH2, 33 RIGHT, 34 WRONG. Files load from `mindmaze.eit*baggage/<name>`.
Used ids: 25..29 move, 31/32 match, 33/34 answers, ambient 0..22. BIRDS, DOOROPEN, KNOCK are never played.
- Ambient: after entering the castle, timer 2 every `8 + rand()%30` s plays AMB `rand()%23` (needs sound on).
  The timer is killed by every room redraw (state 5) and by questions, and restarted only when music is on and a
  loop has started; it is not re-armed when sound is off (`0x51b770`).
- Music object (+0x14 state): 1 = stop all, play INTRO.MID; 2 = next background: BGLOOP `rand()%3+1`
  (`FUN_0051b0d0` mode 0); 3 = pause background, play QUESTION.MID on the second player, stop ambient;
  4/6 = stop question music, resume background; 5 = pause (window deactivated, also around every room redraw).
- "%d-%d.mid" (`FUN_0051b4f0`) = transition file "<from>-<to>.mid" between BGLOOPs; only reached in mode 1..3,
  set by FUN_0051b340 (mode = loop number), so every loop end tries one; no such files ship.
- [V 2026-11-16, see RE-NOTES "Flow audit"] State 1 sets state 2 right after starting INTRO.MID (plays once);
  the end events 0x4f52b0 / 0x4f52f0 (WMP event 0xbc4) call FUN_0051b0d0 in state 2 (next loop, always a
  different one) and rerun state 3 (QUESTION.MID loops). Entering the castle does not change the music.

## 9. Persistence [V `FUN_0051bcb0`, `FUN_0051bb80`, `FUN_0051bf40`, `FUN_0051be60`]
Value name `MindmazePlayer%u` (n = 1, 2, ...), string value `"%u,%u,%s"` = score, award rank, name; or `"deleted"`.
Read through Encarta's settings COM object (global 0x6c0f90, vtbl+0x24, must return VT_BSTR) [G: registry
under the Encarta user key]. Enumeration n = 1.. stops at the first missing value. Saving reuses the player's n,
else the first missing or "deleted" n. Level, area, maze and position are **not** saved. "Tourist" (29043) is never
loaded, saved or deleted. Players are saved when a game ends or a new game starts (`FUN_004f3e30`).
Delete button writes "deleted" for each selected name.

## 10. Control panel layout (636x136 at y=301) [V tables 0x6b4280..0x6b4534, `FUN_0051edb0`]
Rects are (x, y, w, h) in panel coordinates.
- Match buttons x5: (6, 8+24i, 20, 23), cmds 29158..29162.
- Map frame line rect (29,7)-(169,127), pen #A09683; map `FUN_0051da70(maze, 30, 8, 162, 126)`: 10x10 cells of
  13x11 px with 1 px gaps. Colours: background #D9D6C5; unvisited cell = 1 px #EBEAE1 outline; visited or revealed
  cell = 2 px #A09683 outline inset 1 px; the stairs room is **always** drawn, with a 2 px #973829 outline; door
  openings = 5x2 / 2x5 gaps in the background colour at N (x+4..9, top), S (x+4..9, bottom), W (left, y+3..8),
  E (right, y+3..8); current room = #973829 block (x+3..9, y+3..8) with its 4 corner pixels cleared.
  (CHANGE: the rebuild draws the current-room marker as an anti-aliased ellipse in the same box.)
- Idle mode (no question): label 29028 "Choose Area of Interest" (174,6,150,15); 10 area radio buttons
  (cmd 29014..29023, text = string id) col 1 x=174 w=100: All Areas, Life Science, Geography, History, Physical
  Science; col 2 x=282 w=165: Social Science, Religion and Philosophy, Art Language and Literature, Performing
  Arts, Sports Hobbies and Pets; rows y = 27, 46, 65, 84, 103, h=15. Label 29029 "Level" (455,6,33,15); 4 level
  buttons (455, 27/53/79/105, 29x26), cmd 29154..29157; underline lines y=21 from x=174..450 and 454..489.
  Name (488,4,148,20); "Progress:" (492,90,55,20); score (551,90,35,20); "/20000" (586,90,40,20); award picture
  at (529,35).
- Question mode: area buttons, level buttons and the 5 labels above are hidden; shown: letters "A." .. "D."
  (175, 10/40/70/100, 17x18), answer buttons (195, 10/40/70/100, 265x18, text = shuffled answers), "refer" buttons
  (465, same y, 18x18), points label 29034 (550,35,30,20) = (level+1)*seconds, picture resource 0x71EE at (522,17).
  Name stays visible.
- Buttons New Game (492,108,69,19), Quit Game (563,108,69,19).
- Area button -> DB area [V `FUN_00520270`]: 1->1, 2->2, 3->3, 4->0, 5->4, 6->5, 7->6, 8->7, 9->8, 0 = All Areas.

## 11. Still unknown
- Exact key bindings of accelerators 25113..25119 (only the command ids are known).
- (answered 2026-11-16: the end events, see section 8.)
- Settings backend of the COM object at 0x6c0f90 (registry path).
- Captions/images of the level buttons and the "refer" buttons (Encarta button class `FUN_0057f380`), and the
  panel background brush (`FUN_00584cd0`, Encarta theme).
- Meaning of `game+0xb4` and the per-slot counter at 0x6c20f0 (decremented by 5 on a panel double-click).
- Logon hotspot pictures (which part of LOGONSCR is the "enter" gate and which the "story" item) - only rects known.
