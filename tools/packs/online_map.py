"""Writes packs/online_map.json for the game's later on-demand fetching (2026-11-17).

Built from the category tables in opentdb.py and triviaapi.py, so the map always matches the importers.
Note: the importers also move single questions between areas by keyword/tag rules (e.g. Open Trivia DB
"Science & Nature" questions about animals go to Life Science); an on-demand fetch can only ask for whole
categories, so this map lists the category defaults plus the categories that feed an area through those
rules.

    python3 -I tools/packs/online_map.py
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmpack  # noqa: E402
import opentdb  # noqa: E402
import triviaapi  # noqa: E402


def main():
    otdb = {str(a): [] for a in range(mmpack.AREAS)}
    for cat, (area, _name) in sorted(opentdb.CATEGORIES.items()):
        if area is not None:
            otdb[str(area)].append(cat)
    otdb["1"].append(17)                       # Science & Nature also feeds Life Science (keyword rule)
    ta = {str(a): [] for a in range(mmpack.AREAS)}
    for cat, area in triviaapi.CATEGORIES.items():
        if area is not None:
            ta[str(area)].append(cat)
    ta["1"].append("science")                  # tag/keyword rule: biology, animals, medicine ...
    ta["5"].append("society_and_culture")      # tag/keyword rule: religion, mythology, philosophy
    doc = {
        "opentdb": {"endpoint": "https://opentdb.com/api.php?amount=50&type=multiple&encode=url3986"
                                "&category={id}&difficulty={difficulty}&token={token}",
                    "categories_by_area": otdb, "difficulty_by_level": opentdb.DIFFICULTY_BY_LEVEL},
        "triviaapi": {"endpoint": "https://the-trivia-api.com/v2/questions?limit=50&types=text_choice"
                                  "&categories={categories}&difficulties={difficulty}",
                      "categories_by_area": ta, "difficulty_by_level": triviaapi.DIFFICULTY_BY_LEVEL},
        "terms": {
            "opentdb": "Open Trivia DB: CC BY-SA 4.0 (attribute opentdb.com, share alike). No key. One request per "
                       "5 s per IP, max 50 questions per request, type=multiple; use a session token "
                       "(api_token.php?command=request, expires after 6 h idle; response_code 4 = exhausted, "
                       "5 = rate limit). Level 3 has no own difficulty: use hard.",
            "triviaapi": "The Trivia API: CC BY-NC 4.0 - free for non-commercial use only, commercial use needs a "
                         "paid subscription; credit the API somewhere discreet. No key for English text_choice "
                         "questions; max 50 per request; 20 requests per 5 s per IP (429 above). Sessions, images, "
                         "translations need a key. Level 3 = hard (the pack also uses isNiche for level 3).",
        },
    }
    path = os.path.join(mmpack.PACKS, "online_map.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("wrote", path)


if __name__ == "__main__":
    main()
