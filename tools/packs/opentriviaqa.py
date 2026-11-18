"""OpenTriviaQA -> packs/opentriviaqa.json (2026-11-17).

Source: https://github.com/uberspot/OpenTriviaQA (CC BY-SA 4.0). The repository zip is downloaded once
to cache/opentriviaqa/master.zip; the category files are parsed from it:

    #Q question text (may continue on following lines)
    ^ correct answer
    A option
    B option
    C option
    D option

Only questions with exactly four options A-D (and the ^ answer among them) are kept.
No difficulty in the source: level = stable hash of the normalized question text mod 4 (an even spread
that does not change between runs).

    python3 -I tools/packs/opentriviaqa.py            # uses the cached zip
    python3 -I tools/packs/opentriviaqa.py --refresh  # download the zip again
"""
import hashlib
import io
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmpack  # noqa: E402

ZIP_URL = "https://github.com/uberspot/OpenTriviaQA/archive/refs/heads/master.zip"

# category file -> area (None = mixed/unsuitable, not imported)
CATEGORIES = {
    "animals": 1,
    "brain-teasers": None,        # riddles/logic puzzles
    "celebrities": 7,
    "entertainment": 7,
    "for-kids": None,             # mixed topics
    "general": None,              # mixed topics
    "geography": 2,
    "history": 3,
    "hobbies": 8,
    "humanities": 6,              # religion/philosophy words move a question to area 5
    "literature": 6,
    "movies": 7,
    "music": 7,
    "newest": None,               # mixed topics (latest additions)
    "people": 3,                  # biographies; social-science words move a question to area 4
    "rated": None,                # mixed topics (top rated)
    "religion-faith": 5,
    "science-technology": 0,      # life-science words move a question to area 1
    "sports": 8,
    "television": 7,
    "video-games": 8,
    "world": 2,                   # countries, places; religion/social words move a question to 5/4
}
KEYWORD_SPLIT = {"humanities": [mmpack.RELIGION_WORDS, mmpack.SOCIAL_WORDS], "science-technology": [mmpack.LIFE_WORDS],
                 "world": [mmpack.RELIGION_WORDS, mmpack.SOCIAL_WORDS], "people": [mmpack.SOCIAL_WORDS]}

PACK = {
    "id": "opentriviaqa",
    "name": "OpenTriviaQA",
    "url": "https://github.com/uberspot/OpenTriviaQA",
    "license": "CC BY-SA 4.0",
    "attribution": "Questions from OpenTriviaQA (https://github.com/uberspot/OpenTriviaQA) by uberspot and "
                   "contributors, licensed CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/). "
                   "Filtered and mapped to MindMaze areas; this pack is shared under the same licence.",
    "terms": "Static files on GitHub, downloaded once.",
    "level_rule": "no difficulty in the source: level = sha1(normalized question) mod 4 (stable, even spread).",
}


def parse(text):
    """Yields (question, correct, [options]) from one category file."""
    q, correct, opts = None, None, []

    def flush():
        if q is not None:
            return (q.strip(), correct, opts)
        return None

    for line in text.splitlines():
        s = line.rstrip()
        if s.startswith("#Q"):
            item = flush()
            if item:
                yield item
            q, correct, opts = s[2:].strip(), None, []
        elif q is None:
            continue
        elif s.startswith("^"):
            correct = s[1:].strip()
        elif len(s) >= 2 and s[0] in "ABCDEFGH" and s[1] == " ":
            opts.append(s[2:].strip())
        elif s.strip() and correct is None and not opts:
            q += " " + s.strip()                  # question continued on the next line
    item = flush()
    if item:
        yield item


def level_of(question):
    return int(hashlib.sha1(mmpack.norm(question).encode("utf-8")).hexdigest(), 16) % 4


def main():
    zp = mmpack.cache_path("opentriviaqa", "master.zip")
    if "--refresh" in sys.argv or not os.path.exists(zp):
        data = mmpack.http_get(ZIP_URL, timeout=180)
        with open(zp, "wb") as f:
            f.write(data)
    z = zipfile.ZipFile(zp)
    raw, skipped = [], {"unmapped_category": 0, "not_4_options": 0, "answer_not_in_options": 0}
    for name in sorted(z.namelist()):
        parts = name.split("/")
        if len(parts) != 3 or parts[1] != "categories" or not parts[2]:
            continue
        cat = parts[2]
        area = CATEGORIES.get(cat)
        text = z.read(name).decode("utf-8", "replace")
        for q, correct, opts in parse(text):
            if area is None:
                skipped["unmapped_category"] += 1
                continue
            if len(opts) != 4:
                skipped["not_4_options"] += 1
                continue
            if correct not in opts:
                skipped["answer_not_in_options"] += 1
                continue
            a = area
            if cat in KEYWORD_SPLIT:              # first matching rule wins, else the category's area
                a = mmpack.keyword_area(mmpack.clean(q), a, KEYWORD_SPLIT[cat])
            raw.append({"area": a, "level": level_of(q), "question": q,
                        "answers": [correct] + [o for o in opts if o != correct], "src_cat": cat})
    mmpack.log("opentriviaqa: skipped before filtering: %r" % skipped)
    mmpack.write_pack(PACK, raw, id_key=None)


if __name__ == "__main__":
    main()
