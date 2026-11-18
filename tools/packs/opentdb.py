"""Open Trivia DB -> packs/opentdb.json (2026-11-17).

Fetches every multiple-choice question per category with a session token (amount<=50,
1 request per 5 s as the API asks, encode=url3986), caches the raw results under
cache/opentdb/cat_<id>.json, then maps and filters them through mmpack.

    python3 -I tools/packs/opentdb.py            # use the cache where complete, fetch the rest
    python3 -I tools/packs/opentdb.py --refresh  # ignore the cache and fetch everything again
"""
import json
import os
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmpack  # noqa: E402

API = "https://opentdb.com"
PAUSE = 5.5      # API: one request per 5 seconds per IP

# category id -> (area, name). None = not mapped (no fitting MindMaze area).
CATEGORIES = {
    9: (None, "General Knowledge"),
    10: (6, "Entertainment: Books"),
    11: (7, "Entertainment: Film"),
    12: (7, "Entertainment: Music"),
    13: (7, "Entertainment: Musicals & Theatres"),
    14: (7, "Entertainment: Television"),
    15: (8, "Entertainment: Video Games"),
    16: (8, "Entertainment: Board Games"),
    17: (0, "Science & Nature"),          # life-science words move a question to area 1
    18: (0, "Science: Computers"),
    19: (0, "Science: Mathematics"),
    20: (5, "Mythology"),
    21: (8, "Sports"),
    22: (2, "Geography"),
    23: (3, "History"),
    24: (4, "Politics"),
    25: (6, "Art"),
    26: (7, "Celebrities"),
    27: (1, "Animals"),
    28: (0, "Vehicles"),
    29: (6, "Entertainment: Comics"),
    30: (0, "Science: Gadgets"),
    31: (6, "Entertainment: Japanese Anime & Manga"),
    32: (7, "Entertainment: Cartoon & Animations"),
}
KEYWORD_SPLIT = {17: [mmpack.LIFE_WORDS]}
DIFFICULTY = {"easy": 0, "medium": 1, "hard": 2}
DIFFICULTY_BY_LEVEL = {"0": "easy", "1": "medium", "2": "hard", "3": "hard"}

PACK = {
    "id": "opentdb",
    "name": "Open Trivia DB",
    "url": "https://opentdb.com",
    "license": "CC BY-SA 4.0",
    "attribution": "Questions from the Open Trivia Database (https://opentdb.com) by PixelTail Games and its "
                   "contributors, licensed CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/). "
                   "Filtered and mapped to MindMaze areas; this pack is shared under the same licence.",
    "terms": "Free API, no key; one request per 5 s per IP; max 50 questions per request; session tokens expire "
             "after 6 h without use.",
    "level_rule": "easy=0, medium=1, hard=2 (no level 3).",
}


def get(path, **params):
    url = API + path + ("?" + urllib.parse.urlencode(params) if params else "")
    return json.loads(mmpack.http_get(url).decode("utf-8"))


class Fetcher:
    def __init__(self):
        self.last = 0.0
        self.token = None

    def call(self, path, **params):
        wait = self.last + PAUSE - time.time()
        if wait > 0:
            time.sleep(wait)
        try:
            return get(path, **params)
        finally:
            self.last = time.time()

    def new_token(self):
        d = self.call("/api_token.php", command="request")
        self.token = d["token"]

    def fetch_category(self, cat):
        """All type=multiple questions of one category. Uses the session token so nothing repeats;
        response 1/4 (not enough left) steps the amount down 50-20-10-5-2-1; at amount 1 the category is done."""
        out, amount = [], 50
        while True:
            d = self.call("/api.php", amount=amount, category=cat, type="multiple", encode="url3986",
                          token=self.token)
            rc = d.get("response_code")
            if rc == 0:
                out.extend(d["results"])
                mmpack.log("  cat %d: +%d (%d)" % (cat, len(d["results"]), len(out)))
                continue
            if rc in (1, 4):                      # fewer left than `amount` (1 = no results, 4 = token
                if amount == 1:                   # exhausted for this query): step down, done at amount=1
                    break
                amount = {50: 20, 20: 10, 10: 5, 5: 2, 2: 1}[amount]
                continue
            if rc == 5:                           # rate limited
                time.sleep(10)
                continue
            if rc == 3:                           # token not found/expired
                self.new_token()
                continue
            raise RuntimeError("opentdb response %r" % d)
        return out


def decode(r):
    u = urllib.parse.unquote
    return {"category": u(r["category"]), "difficulty": u(r["difficulty"]), "question": u(r["question"]),
            "correct_answer": u(r["correct_answer"]), "incorrect_answers": [u(x) for x in r["incorrect_answers"]],
            "type": u(r["type"])}


def main():
    refresh = "--refresh" in sys.argv
    f = Fetcher()
    raw = []
    for cat, (area, name) in sorted(CATEGORIES.items()):
        cp = mmpack.cache_path("opentdb", "cat_%d.json" % cat)
        c = None if refresh else mmpack.load_json(cp)
        if c is None or not c.get("complete"):
            if f.token is None:
                f.new_token()
            mmpack.log("opentdb: fetching category %d %s" % (cat, name))
            res = f.fetch_category(cat)
            c = {"category": cat, "name": name, "fetched": time.strftime("%Y-%m-%d"), "complete": True,
                 "results": res}
            mmpack.save_json(cp, c)
        if area is None:
            continue
        for r in c["results"]:
            r = decode(r)
            if r["type"] != "multiple":
                continue
            q = mmpack.clean(r["question"])
            a = area
            for rule in KEYWORD_SPLIT.get(cat, []):
                a = mmpack.keyword_area(q, a, [rule])
            raw.append({"area": a, "level": DIFFICULTY[r["difficulty"]], "question": r["question"],
                        "answers": [r["correct_answer"]] + r["incorrect_answers"],
                        "src_cat": "%d %s" % (cat, name)})
    mmpack.write_pack(PACK, raw, id_key=None)


if __name__ == "__main__":
    main()
