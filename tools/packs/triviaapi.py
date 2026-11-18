"""The Trivia API (v2) -> packs/triviaapi.json (2026-11-17).

Licence (read 2026-11-17 on https://the-trivia-api.com, docs and pricing pages): the API and all data
returned from it are CC BY-NC 4.0 - free for non-commercial use, commercial use needs a subscription;
"please credit the API somewhere discreet". No key needed for English multiple choice (text_choice).
Limits without a key: up to 50 questions per request, 20 requests per 5 s per IP (429 above that).

Samples /v2/questions?limit=50&categories=<c> round-robin over the categories, 1 request per 1.5 s,
until about 5,000 unique questions or every category stops returning new ones. Raw questions are cached
in cache/triviaapi/questions.json (keyed by the API's id), so a re-run without --fetch only rebuilds.

    python3 -I tools/packs/triviaapi.py            # fetch only if the cache is empty, then build
    python3 -I tools/packs/triviaapi.py --fetch    # sample more (adds to the cache), then build
"""
import json
import os
import re
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmpack  # noqa: E402

API = "https://the-trivia-api.com/v2/questions"
PAUSE = 1.5
TARGET = 5000

# category -> default area (None = only via tags/keywords below)
CATEGORIES = {
    "science": 0,
    "geography": 2,
    "history": 3,
    "society_and_culture": 4,
    "arts_and_literature": 6,
    "film_and_tv": 7,
    "music": 7,
    "sport_and_leisure": 8,
    "food_and_drink": 8,
    "general_knowledge": None,
}
# Tags look like "prefix:value" (e.g. "animal:mammals", "science:biology", "category:history").
# Tag rules only refine three categories: science (-> Life Science), society_and_culture (-> Religion and
# Philosophy) and general_knowledge (-> any area). First matching regex wins.
TAG_AREAS = [
    (r"^(religion|mythology|philosophy|god|gods|deity|bible)[:]|[:-](religion|religions|mythology|philosophy|"
     r"philosophers|the-bible|bible|gods|saints|christianity|islam|judaism|hinduism|buddhism)$", 5),
    (r"^(animal|plant|nature)[:]|^science:(biology|medicine|human-body|diseases|senses|palaeontology|botany|"
     r"genetics|anatomy|zoology|evolution|plants|ecology)$", 1),
    (r"^science:", 0),
    (r"^category:geography$|^place:", 2),
    (r"^category:history$|^era:|^history:", 3),
    (r"^category:society-and-culture$|^(politics|economics|business|law):", 4),
    (r"^category:arts-and-literature$|^(literature|art|language|words):", 6),
    (r"^category:(music|film-and-tv)$|^(music|film|tv|television|theatre):", 7),
    (r"^category:(sport-and-leisure|food-and-drink)$|^(sport|food|drink|hobby|games):", 8),
]
DIFFICULTY = {"easy": 0, "medium": 1, "hard": 2}
DIFFICULTY_BY_LEVEL = {"0": "easy", "1": "medium", "2": "hard", "3": "hard"}

PACK = {
    "id": "triviaapi",
    "name": "The Trivia API",
    "url": "https://the-trivia-api.com",
    "license": "CC BY-NC 4.0",
    "attribution": "Questions from The Trivia API (https://the-trivia-api.com) by Trivia Services Ltd, licensed "
                   "CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/). Non-commercial use only; "
                   "filtered and mapped to MindMaze areas.",
    "terms": "CC BY-NC 4.0: free for non-commercial use, commercial use needs a paid subscription; credit the API. "
             "No key for English multiple choice; max 50 per request; 20 requests per 5 s per IP (100 with a key).",
    "level_rule": "easy=0, medium=1, hard=2, hard with isNiche=true -> 3.",
}


def area_of(item):
    cat = item.get("category")
    a = CATEGORIES.get(cat)
    q = mmpack.clean((item.get("question") or {}).get("text"))
    tags = [t.lower() for t in item.get("tags") or []]
    if cat in ("science", "society_and_culture", "general_knowledge"):
        rules = TAG_AREAS if cat == "general_knowledge" else [r for r in TAG_AREAS if r[1] in ((1,) if cat == "science" else (5,))]
        for pat, area in rules:
            if any(re.search(pat, t) for t in tags):
                return area
    if a == 0:
        return mmpack.keyword_area(q, 0, [mmpack.LIFE_WORDS])
    if a == 4:
        return mmpack.keyword_area(q, 4, [mmpack.RELIGION_WORDS])
    if a is None:     # general knowledge without a known tag: keyword rules, else unmapped
        return mmpack.keyword_area(q, None, [mmpack.LIFE_WORDS, mmpack.RELIGION_WORDS])
    return a


def level_of(item):
    lv = DIFFICULTY.get(item.get("difficulty"), 1)
    if lv == 2 and item.get("isNiche"):
        lv = 3
    return lv


def sample(store, cp):
    cats = list(CATEGORIES)
    stalled = {c: 0 for c in cats}
    i, last = 0, 0.0
    while len(store) < TARGET and any(v < 3 for v in stalled.values()):
        c = cats[i % len(cats)]
        i += 1
        if stalled[c] >= 3:
            continue
        wait = last + PAUSE - time.time()
        if wait > 0:
            time.sleep(wait)
        url = API + "?" + urllib.parse.urlencode({"limit": 50, "categories": c, "types": "text_choice"})
        try:
            items = json.loads(mmpack.http_get(url).decode("utf-8"))
        finally:
            last = time.time()
        new = 0
        for it in items:
            if it.get("id") and it["id"] not in store:
                store[it["id"]] = it
                new += 1
        stalled[c] = stalled[c] + 1 if new < 3 else 0
        mmpack.log("triviaapi %-20s +%2d  total %d" % (c, new, len(store)))
        if i % 10 == 0:
            mmpack.save_json(cp, store)
    mmpack.save_json(cp, store)


def main():
    cp = mmpack.cache_path("triviaapi", "questions.json")
    store = mmpack.load_json(cp, {})
    if "--fetch" in sys.argv or not store:
        sample(store, cp)
    raw, unmapped = [], 0
    for qid in sorted(store):
        it = store[qid]
        if it.get("type", "text_choice") != "text_choice":
            continue
        a = area_of(it)
        if a is None:
            unmapped += 1
            continue
        raw.append({"area": a, "level": level_of(it), "question": (it.get("question") or {}).get("text"),
                    "answers": [it.get("correctAnswer")] + list(it.get("incorrectAnswers") or []),
                    "src_cat": it.get("category"), "src_ref": "https://the-trivia-api.com/v2/question/" + qid})
    mmpack.log("triviaapi: %d cached, %d unmapped (general knowledge without a fitting tag)" % (len(store), unmapped))
    mmpack.write_pack(PACK, raw, id_key=None)


if __name__ == "__main__":
    main()
