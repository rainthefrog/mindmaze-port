"""Wikidata -> packs/wikidata.json: clue-style questions like the original game's (2026-11-17).

"Capital of Peru." -> Lima, with three wrong answers of the same kind (other capitals).

Each TEMPLATE runs one small SPARQL query on https://query.wikidata.org/sparql: a sub-select of the
most-linked subjects (ORDER BY sitelinks, LIMIT), then their answer, English labels, the answer's
sitelink count (to pick the best-known answer when a subject has several) and English Wikipedia articles.
Raw responses are cached in cache/wikidata/<key>.json together with a hash of the query, so a re-run only
hits the service for templates whose query changed (or with --refresh). 4 s pause between live queries.

Rules:
- kind "item": answer = the answer item's label; wrong answers = other answers of the same template
  (never another valid answer of the same subject, never a label contained in the question).
- kind "subject": the subject itself is the answer (e.g. "Chemical element with the symbol Fe." -> Iron);
  wrong answers = other subjects of the template.
- kind "year": answer = a year; wrong answers = the template's other years closest to it.
- Level: quartiles of the subject's sitelink count within each template: most linked quarter = 0 ... least = 3.
- ref = English Wikipedia article of the answer (of the subject for "subject"/"year" kinds).
- Subjects whose English label is missing or is a bare Q-id are skipped; so are questions where the
  answer text appears in the question.

    python3 -I tools/packs/wikidata.py              # cached where possible
    python3 -I tools/packs/wikidata.py --refresh    # re-query everything
    python3 -I tools/packs/wikidata.py --only capital,painting   # (re)build listed templates only, for testing
"""
import hashlib
import json
import os
import random
import re
import sys
import time
import urllib.error
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmpack  # noqa: E402

ENDPOINT = "https://query.wikidata.org/sparql"
PAUSE = 4.0

PACK = {
    "id": "wikidata",
    "name": "Wikidata clues",
    "url": "https://www.wikidata.org",
    "license": "CC0 1.0",
    "attribution": "Questions generated from Wikidata (https://www.wikidata.org), CC0 1.0 public domain "
                   "dedication. Linked articles are from English Wikipedia (CC BY-SA 4.0).",
    "terms": "Wikidata Query Service: 60 s query limit, polite use with a descriptive User-Agent; queries kept "
             "small (LIMIT), paused 4 s apart and cached.",
    "level_rule": "per template: quartiles of the subject's Wikidata sitelink count (most linked = 0, least = 3).",
}

# A template: key, area, kind, question format ({s} subject label, {x} extra value), subject sub-select
# (must bind ?s and ?sl; may bind ?x), answer pattern (binds ?a for kind item/subject-less; may use ?s).
# For kind "subject" and "year" the answer pattern may be empty.
SOV = "?a wdt:P31 wd:Q3624078 . FILTER NOT EXISTS { ?a wdt:P576 ?end }"
SOV_S = "?s wdt:P31 wd:Q3624078 . FILTER NOT EXISTS { ?s wdt:P576 ?end }"
HUMAN_A = "?a wdt:P31 wd:Q5 ."
TEMPLATES = [
    # ---- 0 Physical Science & Technology
    dict(key="element_symbol", area=0, kind="subject", q="Chemical element with the symbol {x}.",
         sub="?s wdt:P31 wd:Q11344 ; wdt:P246 ?x .", limit=130),
    dict(key="element_number", area=0, kind="subject", q="Chemical element with atomic number {x}.",
         sub="?s wdt:P31 wd:Q11344 ; wdt:P1086 ?x .", limit=130),
    dict(key="inventor", area=0, kind="item", q="Credited with the invention or discovery of {s}.",
         sub="?s wdt:P61 ?inv . ?inv wdt:P31 wd:Q5 . FILTER NOT EXISTS { ?s wdt:P31 wd:Q5 } "
             "FILTER NOT EXISTS { ?s wdt:P31 wd:Q16521 }", ans="?s wdt:P61 ?a . " + HUMAN_A, limit=500, min_sl=25),
    dict(key="unit_quantity", area=0, kind="item", q="Physical quantity measured in {s}.",
         sub="?s wdt:P31 ?ut ; wdt:P111 ?pq . VALUES ?ut { wd:Q208469 wd:Q223662 wd:Q61610698 }", ans="?s wdt:P111 ?a .", limit=200, min_sl=10),
    # ---- 1 Life Science
    dict(key="scientific_name", area=1, kind="subject", q="Species with the scientific name {x}.",
         sub="?s wdt:P105 wd:Q7432 ; wdt:P225 ?x .", limit=600, min_sl=70),
    dict(key="taxon_order", area=1, kind="item", q="Taxonomic order of the {s}.",
         sub="?s wdt:P105 wd:Q7432 ; wdt:P225 ?n .",
         ans="?s wdt:P171+ ?a . ?a wdt:P105 wd:Q36602 .", limit=600, min_sl=70),
    dict(key="taxon_class", area=1, kind="item", q="Taxonomic class of the {s}.",
         sub="?s wdt:P105 wd:Q7432 ; wdt:P225 ?n .",
         ans="?s wdt:P171+ ?a . ?a wdt:P105 wd:Q37517 .", limit=600, min_sl=70),
    dict(key="taxon_family", area=1, kind="item", q="Taxonomic family of the {s}.",
         sub="?s wdt:P105 wd:Q7432 ; wdt:P225 ?n .",
         ans="?s wdt:P171+ ?a . ?a wdt:P105 wd:Q35409 .", limit=600, min_sl=70),
    # ---- 2 Geography
    dict(key="capital", area=2, kind="item", q="Capital of {s}.",
         sub=SOV_S, ans="?s wdt:P36 ?a .", limit=250),
    dict(key="city_country", area=2, kind="item", q="Country in which the city of {s} lies.",
         sub="?s wdt:P31 wd:Q1549591 .", ans="?s wdt:P17 ?a . " + SOV + " FILTER NOT EXISTS { ?a wdt:P36 ?s }",
         limit=500, min_sl=40),
    dict(key="mountain_country", area=2, kind="item", q="Country in which the mountain {s} lies.",
         sub="?s wdt:P31 wd:Q8502 .", ans="?s wdt:P17 ?a . " + SOV, limit=400, min_sl=20),
    dict(key="lake_country", area=2, kind="item", q="Country in which the lake {s} lies.",
         sub="?s wdt:P31 wd:Q23397 .", ans="?s wdt:P17 ?a . " + SOV, limit=400, min_sl=20),
    # ---- 3 History
    dict(key="treaty_year", area=3, kind="year", q="Year in which the {s} was signed.",
         sub="?s wdt:P31 wd:Q131569 . OPTIONAL { ?s wdt:P585 ?d1 } OPTIONAL { ?s wdt:P571 ?d2 } "
             "OPTIONAL { ?s wdt:P580 ?d3 } BIND(YEAR(COALESCE(?d1, ?d2, ?d3)) AS ?x) FILTER(BOUND(?x))",
         limit=300, min_sl=10),
    dict(key="battle_year", area=3, kind="year", q="Year of the {s}.",
         sub="?s wdt:P31 wd:Q178561 . OPTIONAL { ?s wdt:P585 ?d1 } OPTIONAL { ?s wdt:P580 ?d2 } "
             "BIND(YEAR(COALESCE(?d1, ?d2)) AS ?x) FILTER(BOUND(?x))", limit=400, min_sl=25),
    dict(key="battle_war", area=3, kind="item", q="War in which the {s} was fought.",
         sub="?s wdt:P31 wd:Q178561 ; wdt:P361 ?w .", ans="?s wdt:P361 ?a . ?a wdt:P31/wdt:P279* wd:Q198 .",
         limit=500, min_sl=20),
    dict(key="royal_house", area=3, kind="item", q="Royal house or dynasty of {s}.",
         sub="?s wdt:P31 wd:Q5 ; wdt:P53 ?f . ?f wdt:P31 ?ft . VALUES ?ft { wd:Q13417114 wd:Q164950 wd:Q1156073 }",
         ans="?s wdt:P53 ?a . ?a wdt:P31 ?at . VALUES ?at { wd:Q13417114 wd:Q164950 wd:Q1156073 }", limit=500, min_sl=40),
    # ---- 4 Social Science
    dict(key="currency", area=4, kind="item", q="Currency of {s}.",
         sub=SOV_S, ans="?s p:P38 ?st . ?st ps:P38 ?a . FILTER NOT EXISTS { ?st pq:P582 ?e }", limit=250),
    dict(key="org_headquarters", area=4, kind="item", q="City where the {s} has its headquarters.",
         sub="?s wdt:P31/wdt:P279? wd:Q484652 ; wdt:P159 ?h .", ans="?s wdt:P159 ?a .", limit=300, min_sl=20),
    dict(key="party_founder", area=4, kind="item", q="Founder of the {s}.",
         sub="?s wdt:P31 wd:Q7278 ; wdt:P112 ?f .", ans="?s wdt:P112 ?a . " + HUMAN_A, limit=400, min_sl=15),
    dict(key="social_science_work", area=4, kind="item", q="Author of \"{s}\".",
         sub="?s wdt:P50 ?au . ?au wdt:P106 ?occ . VALUES ?occ { wd:Q188094 wd:Q2306091 wd:Q212980 wd:Q1238570 "
             "wd:Q4773904 }", ans="?s wdt:P50 ?a .", limit=300, min_sl=10),
    # ---- 5 Religion and Philosophy
    dict(key="philosopher_work", area=5, kind="item", q="Philosopher who wrote \"{s}\".",
         sub="?s wdt:P50 ?au . ?au wdt:P106 wd:Q4964182 .", ans="?s wdt:P50 ?a .", limit=400, min_sl=10),
    dict(key="founder_religion", area=5, kind="item", q="Founder of {s}.",
         sub="?s wdt:P112 ?f . ?s wdt:P31/wdt:P279? ?t . VALUES ?t { wd:Q9174 wd:Q2915955 wd:Q13414953 "
             "wd:Q1387659 wd:Q1826286 wd:Q477544 wd:Q879146 }", ans="?s wdt:P112 ?a . " + HUMAN_A, limit=300, min_sl=10),
    dict(key="philosopher_teacher", area=5, kind="item", q="Teacher of the philosopher {s}.",
         sub="?s wdt:P106 wd:Q4964182 ; wdt:P1066 ?t .", ans="?s wdt:P1066 ?a . ?a wdt:P106 wd:Q4964182 .",
         limit=400, min_sl=25),
    dict(key="sacred_text_religion", area=5, kind="item", q="Religion of the sacred text {s}.",
         sub="?s wdt:P31/wdt:P279* wd:Q179461 ; wdt:P140 ?r .", ans="?s wdt:P140 ?a .", limit=200, min_sl=10),
    # ---- 6 Art, Language, and Literature
    dict(key="book_author", area=6, kind="item", q="Author of \"{s}\".",
         sub="?s wdt:P31/wdt:P279? wd:Q7725634 ; wdt:P50 ?au . FILTER NOT EXISTS { ?s wdt:P31 wd:Q25379 } "
             "FILTER NOT EXISTS { ?au wdt:P106 wd:Q4964182 }", ans="?s wdt:P50 ?a .", limit=600, min_sl=30),
    dict(key="painting_painter", area=6, kind="item", q="Painter of \"{s}\".",
         sub="?s wdt:P31 wd:Q3305213 ; wdt:P170 ?p .", ans="?s wdt:P170 ?a . " + HUMAN_A, limit=500, min_sl=10),
    dict(key="sculpture_sculptor", area=6, kind="item", q="Sculptor of \"{s}\".",
         sub="?s wdt:P31 wd:Q860861 ; wdt:P170 ?p .", ans="?s wdt:P170 ?a . " + HUMAN_A, limit=300, min_sl=8),
    dict(key="official_language", area=6, kind="item", q="Official language of {s}.",
         sub=SOV_S, ans="?s wdt:P37 ?a .", limit=250),
    # ---- 7 Performing Arts
    dict(key="opera_composer", area=7, kind="item", q="Composer of the opera \"{s}\".",
         sub="?s wdt:P31 wd:Q1344 ; wdt:P86 ?c .", ans="?s wdt:P86 ?a .", limit=500, min_sl=10),
    dict(key="film_director", area=7, kind="item", q="Director of the film \"{s}\".",
         sub="?s wdt:P31 wd:Q11424 ; wdt:P57 ?d .", ans="?s wdt:P57 ?a .", limit=600, min_sl=60),
    dict(key="musician_instrument", area=7, kind="item", q="Instrument played by {s}.",
         sub="?s wdt:P31 wd:Q5 ; wdt:P1303 ?i .", ans="?s wdt:P1303 ?a . FILTER(?a != wd:Q17172850)",
         limit=500, min_sl=60),
    dict(key="play_playwright", area=7, kind="item", q="Playwright who wrote \"{s}\".",
         sub="?s wdt:P31 wd:Q25379 ; wdt:P50 ?au .", ans="?s wdt:P50 ?a .", limit=400, min_sl=10),
    # ---- 8 Sports, Hobbies, and Pets
    dict(key="athlete_sport", area=8, kind="item", q="Sport of the athlete {s}.",
         sub="?s wdt:P31 wd:Q5 ; wdt:P641 ?sp .", ans="?s wdt:P641 ?a .", limit=600, min_sl=80),
    dict(key="olympics_host", area=8, kind="item", q="Host city of the {s}.",
         sub="?s wdt:P31 ?g . VALUES ?g { wd:Q159821 wd:Q82414 }", ans="?s wdt:P276 ?a . ?a wdt:P31/wdt:P279* wd:Q515 .",
         limit=100),
    dict(key="dog_breed_origin", area=8, kind="item", q="Country of origin of the dog breed {s}.",
         sub="?s wdt:P31 wd:Q39367 ; wdt:P495 ?o .", ans="?s wdt:P495 ?a .", limit=400, min_sl=5),
    dict(key="cat_breed_origin", area=8, kind="item", q="Country of origin of the cat breed {s}.",
         sub="?s wdt:P31 wd:Q43577 ; wdt:P495 ?o .", ans="?s wdt:P495 ?a .", limit=150, min_sl=3),
    dict(key="club_city", area=8, kind="item", q="Home city of the football club {s}.",
         sub="?s wdt:P31 wd:Q476028 ; wdt:P159 ?h .", ans="?s wdt:P159 ?a .", limit=400, min_sl=40),
]


def build_query(t):
    xvar = " ?x" if "?x" in t["sub"] else ""
    ans = t.get("ans", "")
    sel_a = " ?a ?aLabel ?asl ?art" if ans else ""
    q = ["SELECT ?s ?sLabel ?sl ?sart%s%s WHERE {" % (xvar, sel_a),
         "  { SELECT DISTINCT ?s ?sl%s WHERE { %s ?s wikibase:sitelinks ?sl . FILTER(?sl >= %d) }"
         " ORDER BY DESC(?sl) LIMIT %d }" % (xvar, t["sub"], t.get("min_sl", 0), t["limit"]),
         "  ?s rdfs:label ?sLabel . FILTER(LANG(?sLabel) = \"en\")",
         "  OPTIONAL { ?sart schema:about ?s ; schema:isPartOf <https://en.wikipedia.org/> }"]
    if ans:
        q += ["  %s" % ans,
              "  ?a rdfs:label ?aLabel . FILTER(LANG(?aLabel) = \"en\")",
              "  ?a wikibase:sitelinks ?asl .",
              "  OPTIONAL { ?art schema:about ?a ; schema:isPartOf <https://en.wikipedia.org/> }"]
    q.append("}")
    return "\n".join(q)


_last = [0.0]


class Throttled(Exception):
    pass


def run(t, refresh):
    query = build_query(t)
    qh = hashlib.sha1(query.encode("utf-8")).hexdigest()
    cp = mmpack.cache_path("wikidata", t["key"] + ".json")
    c = mmpack.load_json(cp)
    if c and c.get("query_sha1") == qh and not refresh:
        return c["bindings"]
    wait = _last[0] + PAUSE - time.time()
    if wait > 0:
        time.sleep(wait)
    mmpack.log("wikidata: querying %s" % t["key"])
    url = ENDPOINT + "?" + urllib.parse.urlencode({"query": query, "format": "json"})
    t0 = time.time()
    try:
        data = json.loads(mmpack.http_get(url, headers={"Accept": "application/sparql-results+json"},
                                          timeout=70, retries=0).decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 429:                             # throttled: stop all live queries, re-run later
            raise Throttled("HTTP 429, Retry-After %s" % (e.headers.get("Retry-After") if e.headers else "?"))
        raise                                         # 504 = query timed out: no retry (would only repeat)
    finally:
        _last[0] = time.time()
    b = data["results"]["bindings"]
    mmpack.log("  %d rows in %.1f s" % (len(b), time.time() - t0))
    mmpack.save_json(cp, {"key": t["key"], "fetched": time.strftime("%Y-%m-%d"), "query_sha1": qh, "query": query,
                          "bindings": b})
    return b


def val(row, k):
    v = row.get(k)
    return v["value"] if v else None


def qid(uri):
    return uri.rsplit("/", 1)[-1] if uri else None


_QLABEL = re.compile(r"^Q\d+$")


def contains_word(hay, needle):
    h, n = " " + mmpack.norm(hay) + " ", mmpack.norm(needle)
    return bool(n) and (" " + n + " ") in h


def make_questions(t, rows):
    subj = {}          # qid -> dict(label, sl, x, sart, answers{aqid: (label, asl, art)})
    for r in rows:
        s = qid(val(r, "s"))
        e = subj.setdefault(s, {"label": val(r, "sLabel"), "sl": int(val(r, "sl") or 0), "x": set(),
                                "sart": val(r, "sart"), "answers": {}})
        if val(r, "x"):
            e["x"].add(val(r, "x"))
        if val(r, "a"):
            e["answers"][qid(val(r, "a"))] = (val(r, "aLabel"), int(val(r, "asl") or 0), val(r, "art"))
    kind = t["kind"]
    # drop unusable subjects
    items = []
    for s, e in subj.items():
        lab = e["label"]
        if not lab or _QLABEL.match(lab):
            continue
        if kind == "item":
            ans = {k: v for k, v in e["answers"].items() if v[0] and not _QLABEL.match(v[0])}
            if not ans:
                continue
            e["answers"] = ans
        elif not e["x"]:
            continue
        items.append((s, e))
    if not items:
        return []
    # levels: sitelink quartiles within the template
    sls = sorted((e["sl"] for _, e in items), reverse=True)
    n = len(sls)

    def level(sl):
        rank = sum(1 for v in sls if v > sl)          # 0 = most linked
        return min(3, rank * 4 // n)

    out = []
    if kind == "item":
        pool = {}
        for _, e in items:
            for a, (lab, asl, art) in e["answers"].items():
                pool[a] = lab
        for s, e in items:
            best = max(e["answers"].items(), key=lambda kv: (kv[1][1], kv[0]))
            a, (alab, asl, art) = best
            q = t["q"].format(s=e["label"], x="")
            bad = {mmpack.norm(v[0]) for v in e["answers"].values()}
            cands = sorted({lab for k, lab in pool.items() if k not in e["answers"] and mmpack.norm(lab) not in bad
                            and not contains_word(q, lab) and len(lab) <= mmpack.MAX_ANSWER})
            if len(cands) < 3 or contains_word(q, alab):
                continue
            rnd = random.Random(t["key"] + s)
            wrong = rnd.sample(cands, 3)
            out.append({"area": t["area"], "level": level(e["sl"]), "question": q, "answers": [alab] + wrong,
                        "ref": art or e["sart"], "src_cat": t["key"], "wikidata": s})
    elif kind == "subject":
        labels = {s: e["label"] for s, e in items}
        for s, e in items:
            x = sorted(e["x"])[0]
            if x.endswith(".0"):
                x = x[:-2]
            q = t["q"].format(s=e["label"], x=x)
            if mmpack.norm(e["label"]) == mmpack.norm(x) or contains_word(q, e["label"]):
                continue
            cands = sorted({lab for k, lab in labels.items() if k != s and mmpack.norm(lab) != mmpack.norm(e["label"])})
            rnd = random.Random(t["key"] + s)
            out.append({"area": t["area"], "level": level(e["sl"]), "question": q,
                        "answers": [e["label"]] + rnd.sample(cands, 3), "ref": e["sart"], "src_cat": t["key"],
                        "wikidata": s})
    elif kind == "year":
        years = sorted({int(float(x)) for _, e in items for x in e["x"]})
        for s, e in items:
            ys = {int(float(x)) for x in e["x"]}
            if len(ys) != 1:
                continue
            y = ys.pop()
            if y < 0:
                continue
            near = sorted((v for v in years if v != y and v > 0), key=lambda v: (abs(v - y), v))[:9]
            if len(near) < 3:
                continue
            rnd = random.Random(t["key"] + s)
            q = t["q"].format(s=e["label"], x="")
            if str(y) in q:
                continue
            out.append({"area": t["area"], "level": level(e["sl"]), "question": q,
                        "answers": [str(y)] + [str(v) for v in rnd.sample(near, 3)], "ref": e["sart"],
                        "src_cat": t["key"], "wikidata": s})
    return out


def main():
    refresh = "--refresh" in sys.argv
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    raw, per = [], {}
    for t in TEMPLATES:
        if only and t["key"] not in only:
            continue
        try:
            rows = run(t, refresh)
        except Throttled as ex:
            mmpack.log("wikidata: %s throttled (%s); not querying further, re-run later (cache is kept)" % (t["key"], ex))
            sys.exit(2)
        except Exception as ex:                       # one failing template must not stop the others
            mmpack.log("wikidata: %s FAILED: %s" % (t["key"], ex))
            per[t["key"]] = "failed"
            continue
        qs = make_questions(t, rows)
        per[t["key"]] = len(qs)
        raw.extend(qs)
    mmpack.log("wikidata: questions per template (before filtering): " + json.dumps(per))
    if only:
        for q in raw[:: max(1, len(raw) // 25)]:
            print(q["level"], q["question"], "|", " / ".join(q["answers"]), "|", q.get("ref"))
        return
    PACK["templates"] = per
    mmpack.write_pack(PACK, raw, id_key=None)


if __name__ == "__main__":
    main()
