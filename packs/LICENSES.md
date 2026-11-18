# Question packs - licences

The MIT licence of this repository covers the code only. Each question pack is data with its own licence;
the full attribution text is in the `pack` header of each file.

| File | Source | Licence |
|---|---|---|
| `opentdb.json` | Open Trivia Database (https://opentdb.com), PixelTail Games and contributors | CC BY-SA 4.0 |
| `opentriviaqa.json` | OpenTriviaQA (https://github.com/uberspot/OpenTriviaQA), uberspot and contributors | CC BY-SA 4.0 |
| `online_map.json` | category mapping for the optional online top-up, written for this project | MIT (as the code) |

CC BY-SA 4.0: https://creativecommons.org/licenses/by-sa/4.0/ - the packs were filtered and mapped to
MindMaze's nine subject areas and four levels (tools/packs); they are shared under the same licence.

Not included:
- The original Encarta questions (Microsoft). They come from your own disc (tools/extract/extract.py).
- A pack from The Trivia API (CC BY-NC 4.0, non-commercial, API terms). Its importer is in
  `tools/packs/triviaapi.py`; build the pack yourself if their terms allow your use.

Community questions can be wrong or out of date. Packs are switched on and off in the game's Settings screen.
