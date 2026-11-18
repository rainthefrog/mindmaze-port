# HD (2x) and widescreen art

The game runs with the original pictures. Optionally an AI agent that can generate images (the prompts were
written for OpenAI Codex) paints sharper 2x versions (F2) and side extensions for a wider view (F3) from **your
own extracted pictures**. The results are derivatives of Microsoft art: keep them for yourself, do not commit or
share them.

Nothing needs to be installed: MindMaze itself prepares the sheets for the agent and turns the agent's results
into the pictures it shows. The agent only paints.

## How
1. Start MindMaze once. Next to the pictures it extracted, the data folder now holds `art/`: the instructions
   (`START-HERE.txt`, `PROMPT-HD.txt`, `PROMPT-WIDE.txt`) and the sheets to paint (`art/work/hd/sheets`, 28
   sheets, and `art/work/wide/sheets`, 20 sheets).
2. Open the agent on the data folder and give it `art/START-HERE.txt`. Its placeholders `<data-folder>` and
   `<which-art>` (HD, WIDESCREEN or BOTH) can be filled in first; if they are not, the agent asks you.
3. The agent paints every sheet into `art/work/hd/out_gen` / `art/work/wide/out_gen`.
4. Installing: start MindMaze again (it installs new results by itself), or press Settings > Pictures > Install.
   MindMaze writes `art/work/REPORT.txt`; a line starting with REGENERATE names a widescreen sheet that has to be
   painted once more.
5. Switch the pictures on and off with F2 / F3, the MindMaze menu, or Settings > Pictures.

`art/work` can be deleted afterwards.

## Token / usage cost
Making the art is the expensive part. HD is 28 image generations (1536x1024 each), widescreen 20 more (up to
2 tries per sheet), and for every sheet the model also reads its notes and checks the result. On a
subscription this can use a large share of the usage limit (possibly more than one limit window); on an API it
is a noticeable bill. Ways to keep it under control:
- run HD and widescreen as separate sessions (HD alone already gives most of the improvement);
- stop at any time: both prompts skip sheets whose result already exists, so a new session continues;
- do not ask the model to "improve" or regenerate sheets: the prompts allow one generation per sheet (two for
  widescreen when a check fails), which is also what gives the best results.

## What MindMaze does with the results (for developers)
The game's built-in art tools (`src/art/mm_art.c`) are a C port of the reference Python tools in `tools/art/`
(`mm_sheets.py`, `mm_cut.py`, `mm_wide_sheets.py`, `mm_wide_cut.py`, `key_flat_bg.py`):
- HD: the result is keyed on magenta; drawing outside a sprite's box is kept when it connects to the sprite;
  the original decides what is solid; colours are matched back to the original; flat panel backgrounds are
  repainted in the original's colour; award squares become transparent. Output `hd/<key>.png` + `hd/bounds.txt`.
- Widescreen: FILL sheets keep the picture in place and take the painted strips; STRETCH sheets (logon and
  title screens, which have their own frame) take the whole widened painting. Output `wide/<key>.png` (2x),
  `<key>_1x.png` and `wide.txt`. Results painted outside their box are reported (REGENERATE).
- `tests/art_compare.py` compares the C tools with the Python ones (verified 2026-11-18 on the reference disc with
  the original Codex session's results: all 48 sheets identical, all 38 widescreen pictures identical, 104 of 106
  HD pictures identical and the other two differing in one pixel by one level).
- After changing the descriptions or the widescreen tasks in the Python tools, regenerate `src/art/mm_art_text.h`
  with `make art-text`.

## Lessons learned
- Never give the model an earlier result as an input image: it copies that sheet's layout.
- One generation per sheet; do not resize with scripts - every sheet must come from the model.
- Tall pictures are centred on their sheet so the model does not stretch them to fill the canvas.
- The sign-in screen (STRETCH action) needs the rule "do not draw the border over the path".
- When two generations exist, choose by measuring (bounding box and registration against the original) and
  by eye.
- Objects keep their original positions in widescreen; only the edges are extended.
