"""Shared helper for the MindMaze question-pack importers (2026-11-17).

Every importer builds a list of raw questions
    {"src_id": ..., "area": 0-8, "level": 0-3, "question": str, "answers": [correct, wrong, wrong, wrong],
     "ref": optional URL, plus any extra keys (kept; the game's loader ignores unknown keys)}
and calls write_pack(), which cleans, filters, dedupes, assigns stable ids, writes packs/<id>.json
and prints a report (total, drops per rule, 9x4 area x level table).

Paths are relative to the repository root, found from this file's location.
Python stdlib only; run with `python3 -I`.
"""
import hashlib
import html
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # tools/packs -> tools -> project root
CACHE = os.path.join(HERE, "cache")
PACKS = os.path.join(ROOT, "packs")
ORIGINAL = os.path.join(ROOT, "questions.json")
USER_AGENT = "MindMaze-pack-builder/1.0"

MAX_ANSWER = 40          # the answer box holds about 40 Tahoma-11 characters
MAX_QUESTION = 300       # the question banner holds about 300
AREAS = 9
LEVELS = 4

RULES = ["empty", "answers_not_4_distinct", "answer_too_long", "question_too_long", "true_false",
         "all_none_above", "unsupported_chars", "dup_in_pack", "dup_original"]


# ---------------------------------------------------------------- HTTP
def http_get(url, headers=None, timeout=60, retries=4, backoff=10.0):
    h = {"User-Agent": USER_AGENT}
    h.update(headers or {})
    for attempt in range(retries + 1):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503, 504) and attempt < retries:
                wait = backoff * (attempt + 1)
                ra = e.headers.get("Retry-After") if e.headers else None
                if ra and ra.isdigit():
                    wait = max(wait, int(ra))
                log("  HTTP %d, waiting %.0f s" % (e.code, wait))
                time.sleep(wait)
                continue
            raise
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            if attempt < retries:
                log("  network error %s, waiting %.0f s" % (e, backoff * (attempt + 1)))
                time.sleep(backoff * (attempt + 1))
                continue
            raise


def log(*a):
    print(*a, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- text
_TAG = re.compile(r"<[^>]+>")
_WS = re.compile(r"\s+")


def clean(s):
    """Decode entities (repeatedly, for double-encoded text) and strip HTML tags."""
    if s is None:
        return ""
    s = str(s)
    for _ in range(3):
        t = html.unescape(s)
        if t == s:
            break
        s = t
    s = _TAG.sub("", s)
    s = s.replace(" ", " ").replace("​", "")
    return _WS.sub(" ", s).strip()


def norm(s):
    """Normalized text for dedupe: lowercase letters/digits only, single spaces."""
    s = clean(s).lower()
    s = re.sub(r"[^0-9a-zÀ-ɏͰ-ϿЀ-ӿ]+", " ", s)
    return s.strip()


def stable_id(text):
    """Stable positive 31-bit id from the normalized question text (the game stores id as int)."""
    v = int(hashlib.sha1(norm(text).encode("utf-8")).hexdigest()[:8], 16) & 0x7FFFFFFF
    return v or 1


_TF = {"true", "false", "yes", "no"}
_ALLNONE = re.compile(r"\b(all|none|both|neither) of (the )?(above|these|those|them|the answers)\b|^(all|none) of (the )?above$",
                      re.I)


def _unsupported(s):
    # Tahoma covers Latin, Greek, Cyrillic, Hebrew, Arabic, Thai and common symbols; CJK/emoji etc. would
    # show as boxes in the question dialog.
    return any(ord(c) >= 0x2E80 and not (0xFB00 <= ord(c) <= 0xFB4F) for c in s)


def keyword_area(text, default, rules):
    """rules: list of (area, [regex, ...]); first rule with a match wins, else default."""
    low = " " + text.lower() + " "
    for area, pats in rules:
        for p in pats:
            if re.search(p, low):
                return area
    return default


# Words that move a general "science" question to Life Science (area 1).
LIFE_WORDS = (1, [r"\b(animal|animals|species|mammal|mammals|bird|birds|fish|fishes|insect|insects|reptile|amphibian|"
                  r"plant|plants|tree|trees|flower|leaf|leaves|seed|fungus|fungi|bacteria|bacterium|virus|viruses|"
                  r"cell|cells|gene|genes|genetic|dna|rna|protein|enzyme|organ|organs|blood|bone|bones|muscle|"
                  r"brain|heart|liver|kidney|lung|lungs|skin|human body|disease|diseases|vitamin|hormone|"
                  r"biology|biologist|botany|zoology|evolution|darwin|photosynthesis|chromosome|vaccine|"
                  r"anatomy|nerve|organism|predator|dinosaur|whale|shark|snake|spider|ant|bee|dog|cat|horse)\b"])
# Words that move a humanities/society question to Religion and Philosophy (area 5).
RELIGION_WORDS = (5, [r"\b(god|gods|goddess|deity|religion|religious|church|bible|biblical|testament|gospel|jesus|"
                      r"christ|christian|christianity|islam|muslim|koran|quran|qur'an|prophet|muhammad|judaism|jewish|"
                      r"torah|hindu|hinduism|buddha|buddhism|buddhist|sikh|taoism|confucius|confucianism|pope|saint|"
                      r"apostle|myth|mythology|mythological|zeus|odin|thor|philosophy|philosopher|plato|"
                      r"aristotle|socrates|nietzsche|kant|descartes|ethics|existentialism|stoic|stoicism)\b"])

# Words that move a world/people/humanities question to Social Science (area 4).
SOCIAL_WORDS = (4, [r"\b(economy|economic|economics|economist|currency|currencies|inflation|stock market|stock exchange|"
                    r"tax|taxes|gdp|bank|banks|company|corporation|trade|election|elections|elected|parliament|"
                    r"congress|senate|political party|politics|politician|constitution|law|laws|legal|court|"
                    r"supreme court|united nations|nato|european union|psychology|psychologist|sociology|"
                    r"anthropology|education|university|universities|school|crime|criminal|prison|police|"
                    r"marriage|population|census|government|monarchy|democracy|socialism|communism|capitalism)\b"])

# ---------------------------------------------------------------- original questions
_orig_norms = None


def original_norms():
    global _orig_norms
    if _orig_norms is None:
        with open(ORIGINAL, encoding="utf-8") as f:
            d = json.load(f)
        _orig_norms = {norm(q["question"]) for q in d["questions"]}
    return _orig_norms


def original_areas():
    with open(ORIGINAL, encoding="utf-8") as f:
        return json.load(f)["areas"]


# ---------------------------------------------------------------- filter + write
def filter_questions(raw, seen=None):
    """Returns (kept, drops dict). raw items: see module doc. Keeps input order."""
    drops = {r: 0 for r in RULES}
    seen = set() if seen is None else seen
    orig = original_norms()
    out = []
    for r in raw:
        q = clean(r.get("question"))
        ans = [clean(a) for a in r.get("answers", [])]
        if not q or not ans or any(not a for a in ans):
            drops["empty"] += 1
            continue
        if len(ans) != 4 or len({a.lower() for a in ans}) != 4:
            drops["answers_not_4_distinct"] += 1
            continue
        if any(len(a) > MAX_ANSWER for a in ans):
            drops["answer_too_long"] += 1
            continue
        if len(q) > MAX_QUESTION:
            drops["question_too_long"] += 1
            continue
        if any(a.lower().rstrip(".") in _TF for a in ans):
            drops["true_false"] += 1
            continue
        if any(_ALLNONE.search(a) for a in ans):
            drops["all_none_above"] += 1
            continue
        if _unsupported(q) or any(_unsupported(a) for a in ans):
            drops["unsupported_chars"] += 1
            continue
        n = norm(q)
        if n in orig:
            drops["dup_original"] += 1
            continue
        if n in seen:
            drops["dup_in_pack"] += 1
            continue
        seen.add(n)
        area, level = int(r["area"]), int(r["level"])
        assert 0 <= area < AREAS and 0 <= level < LEVELS, r
        item = dict(r)
        item["question"] = q
        item["answers"] = ans
        out.append(item)
    return out, drops


def table(qs):
    t = [[0] * LEVELS for _ in range(AREAS)]
    for q in qs:
        t[q["area"]][q["level"]] += 1
    return t


def format_table(t, names=None):
    lines = ["area                                   L0     L1     L2     L3  total"]
    for a in range(AREAS):
        name = ("%d %s" % (a, names[a] if names else ""))[:34]
        lines.append("%-34s %6d %6d %6d %6d %6d" % ((name,) + tuple(t[a]) + (sum(t[a]),)))
    lines.append("%-34s %6d %6d %6d %6d %6d" % (("total",) + tuple(sum(t[a][l] for a in range(AREAS))
                                                                    for l in range(LEVELS)) + (sum(map(sum, t)),)))
    return "\n".join(lines)


def write_pack(pack, raw, id_key="src_id"):
    """pack: header dict (id, name, url, license, attribution, ...). raw: list of raw questions.
    id_key: raw key holding the source's own numeric id, or None to hash the question text."""
    kept, drops = filter_questions(raw)
    areas = original_areas()
    used = set()
    questions = []
    for r in kept:
        qid = None
        if id_key and isinstance(r.get(id_key), int) and r[id_key] > 0:
            qid = r[id_key]
        if qid is None:
            qid = stable_id(r["question"])
        while qid in used:                       # hash collision (rare): next free value, deterministic order
            qid = (qid + 1) & 0x7FFFFFFF or 1
        used.add(qid)
        q = {"id": qid, "area": r["area"], "level": r["level"], "question": r["question"],
             "answers": [{"text": a} for a in r["answers"]]}
        if r.get("ref"):
            q["ref"] = r["ref"]
        for k, v in r.items():                   # extra provenance keys (ignored by the game)
            if k not in q and k not in ("answers", "src_id") and v not in (None, ""):
                q[k] = v
        questions.append(q)
    questions.sort(key=lambda q: (q["area"], q["level"], q["id"]))
    header = dict(pack)
    header["created"] = time.strftime("%Y-%m-%d")
    header["count"] = len(questions)
    doc = {"format": "mindmaze-questions-1", "pack": header,
           "note": "answers[0] is the correct answer; area 0-8 (see areas), level 0-3",
           "areas": areas, "questions": questions}
    os.makedirs(PACKS, exist_ok=True)
    path = os.path.join(PACKS, pack["id"] + ".json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")
    rep = ["== pack %s: %s" % (pack["id"], path),
           "raw questions: %d, kept: %d" % (len(raw), len(questions)),
           "dropped: " + ", ".join("%s %d" % (k, v) for k, v in drops.items()),
           format_table(table(questions), areas)]
    rep = "\n".join(rep)
    print(rep, flush=True)
    with open(os.path.join(CACHE, pack["id"] + ".report.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(rep + "\n")
    return questions, drops


def cache_path(*parts):
    p = os.path.join(CACHE, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def load_json(path, default=None):
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    return default


def save_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, ensure_ascii=False)
    os.replace(tmp, path)
