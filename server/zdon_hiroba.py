"""Donder Hiroba scores -> Z-DON charts, for the import on my page (zdon_site_import.py).

Hiroba (the arcade's site) knows a play by song number and course; this server knows a chart by the game's course
hash. The bridge is the published song manifest (data/songs/song_manifest.json, zdon_git_publish.py): for every ESE
chart it has the chart's header (TITLE / TITLEJA) and its five course hashes, computed by the game itself. Hiroba's
title is matched to TITLEJA or TITLE, both reduced to letters and digits after NFKC and lower case, so "‐" and "-",
"Ⅱ" and "II", full-width and half-width all agree. A title with several charts takes the one named after its folder
(Foo/Foo.tja, the main version); still several, or none, and the song is left out and listed.

Files read (an upload, never executed; JSON only):
  - the donder-hiroba-plus upload payload (Hiroba.txt: base64 of lzutf8-compressed JSON), or that JSON decoded:
    {"taikoProfile", "clearData", "scoreData": {songNo: {"title", "difficulty": {"oni": {crown, score, ...}}}}}
  - hiroba_scrape.js output: hiroba_full.json {"taikoNo", "scores": {...}} or its scores.json
    {songNo: {"title", "levels": {"1".."5": {crown 0-3, score, good, ok, bad, maxCombo, roll}}}}
Only courses with a score are taken (Hiroba's clear list without details has no score to rank by).
"""
from __future__ import annotations

import base64
import binascii
import json
import unicodedata
from pathlib import Path, PurePosixPath

COURSE = {"easy": 0, "normal": 1, "hard": 2, "oni": 3, "ura": 4, "1": 0, "2": 1, "3": 2, "4": 3, "5": 4}
CROWN = {"played": 0, "none": 0, "silver": 1, "gold": 2, "donderfull": 3}
MAX_OUT = 64 * 1024 * 1024


class BadHiroba(Exception):
    pass


def lzutf8_decompress(data: bytes) -> bytes:
    """lzutf8.js's decompressor (one block). Stops at MAX_OUT bytes."""
    out = bytearray()
    n, i = len(data), 0
    while i < n:
        b = data[i]
        if b >> 6 != 3:                                   # 0xxxxxxx / 10xxxxxx: a literal
            out.append(b)
            i += 1
            continue
        ident = b >> 5                                    # 6: short pointer, 7: long pointer
        if i == n - 1 or (i == n - 2 and ident == 7) or data[i + 1] >> 7 == 1:  # a literal UTF-8 lead byte
            out.append(b)
            i += 1
            continue
        length = b & 31
        if ident == 6:
            dist = data[i + 1]
            i += 2
        else:
            dist = (data[i + 1] << 8) | data[i + 2]
            i += 3
        start = len(out) - dist
        if start < 0 or dist == 0:
            raise BadHiroba("bad back-reference")
        for k in range(length):                           # byte by byte: a copy may overlap itself
            out.append(out[start + k])
        if len(out) > MAX_OUT:
            raise BadHiroba("too large")
    return bytes(out)


def load(data: bytes) -> dict:
    """The uploaded file as a dict (payload, decoded payload, hiroba_full.json or scores.json); BadHiroba if not."""
    text = data.decode("utf-8-sig", "replace").strip()
    if not text.startswith("{"):
        try:
            text = lzutf8_decompress(base64.b64decode("".join(text.split()), validate=True)).decode("utf-8")
        except (binascii.Error, ValueError, UnicodeDecodeError, BadHiroba) as e:
            raise BadHiroba("不是 Hiroba 的上傳資料或 JSON") from e
    try:
        doc = json.loads(text)
    except ValueError as e:
        raise BadHiroba("JSON 格式不對") from e
    if not isinstance(doc, dict):
        raise BadHiroba("JSON 不是物件")
    return doc


def _int(v, lo: int, hi: int) -> int | None:
    if isinstance(v, bool) or not isinstance(v, (int, float, str)):
        return None
    try:
        n = int(v)
    except (TypeError, ValueError):
        return None
    return n if lo <= n <= hi else None


def rank_for_score(score: int) -> int:
    """scores.h rank_for_score: 1 white .. 7 rainbow."""
    for limit, rank in ((1_000_000, 7), (950_000, 6), (900_000, 5), (800_000, 4), (700_000, 3), (600_000, 2)):
        if score >= limit:
            return rank
    return 1


def charts(doc: dict) -> tuple[dict, list[dict]]:
    """({"name", "taiko_no"}, [{"no", "title", "course", "crown", "score", "good", "ok", "bad", "roll", "max_combo"}]);
    a course with values out of range is dropped."""
    prof = doc.get("taikoProfile") if isinstance(doc.get("taikoProfile"), dict) else {}
    who = {"name": str(prof.get("nickname") or "")[:40], "taiko_no": str(prof.get("taikoNo") or doc.get("taikoNo") or "")[:20]}
    songs: list[tuple[str, str, dict]] = []                           # (songNo, title, {course: detail})
    if isinstance(doc.get("scoreData"), dict):                        # the payload
        for no, s in doc["scoreData"].items():
            if isinstance(s, dict) and isinstance(s.get("difficulty"), dict):
                songs.append((str(no), str(s.get("title") or ""), s["difficulty"]))
    else:                                                             # the scrape
        scores = doc.get("scores") if isinstance(doc.get("scores"), dict) else doc
        for no, s in scores.items():
            if isinstance(s, dict) and isinstance(s.get("levels"), dict):
                songs.append((str(no), str(s.get("title") or ""), s["levels"]))
    out = []
    for no, title, courses in songs[:20000]:
        for cname, c in courses.items():
            course = COURSE.get(str(cname).lower())
            if course is None or not isinstance(c, dict) or c.get("score") is None:
                continue
            crown = c.get("crown")
            crown = CROWN.get(crown) if isinstance(crown, str) else _int(crown if crown is not None else 0, 0, 3)
            p = {"no": no[:12], "title": title[:200], "course": course, "crown": crown,
                 "score": _int(c.get("score"), 0, 9_999_999)}
            for key, src in (("good", "good"), ("ok", "ok"), ("bad", "bad"), ("roll", "roll"), ("max_combo", "maxCombo")):
                p[key] = _int(c.get(src) or 0, 0, 100_000)
            if all(v is not None for v in p.values()) and title:
                out.append(p)
    return who, out


def norm(s: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFKC", s or "").lower() if unicodedata.category(c)[0] in "LN")


class Catalog:
    """Titles -> charts from the published song manifest; reloaded when the file changes."""

    def __init__(self, path: Path):
        self.path = path
        self.key = None
        self.by_title: dict[str, list[str]] = {}
        self.hashes: dict[str, list[str]] = {}
        self.names: dict[str, tuple[str, str]] = {}                  # chart -> (TITLE, SUBTITLE) for the site

    def ready(self) -> bool:
        try:
            st = self.path.stat()
        except OSError:
            return False
        key = (st.st_mtime_ns, st.st_size)
        if key != self.key:
            doc = json.loads(self.path.read_text(encoding="utf-8"))
            self.hashes = {k: v for k, v in (doc.get("hashes") or {}).items() if isinstance(v, list) and len(v) == 5}
            self.by_title, self.names = {}, {}
            for chart, stub in (doc.get("stubs") or {}).items():
                if chart not in self.hashes:
                    continue
                head = {}
                for line in stub.splitlines():
                    k, sep, v = line.partition(":")
                    if sep and k in ("TITLE", "TITLEJA", "SUBTITLE") and k not in head:
                        head[k] = v.strip()
                    if line.startswith("#START"):
                        break
                self.names[chart] = (head.get("TITLE") or head.get("TITLEJA") or "", head.get("SUBTITLE", ""))
                for t in {norm(head.get("TITLE", "")), norm(head.get("TITLEJA", ""))} - {""}:
                    self.by_title.setdefault(t, []).append(chart)
            self.key = key
        return bool(self.hashes)

    def find(self, title: str) -> tuple[str | None, int]:
        """(the chart for a Hiroba title or None, how many charts have that title)."""
        found = sorted(set(self.by_title.get(norm(title), [])))
        if len(found) == 1:
            return found[0], 1
        main = [c for c in found if PurePosixPath(c).stem == PurePosixPath(c).parent.name]
        return (main[0] if len(main) == 1 else None), len(found)


def match(catalog: Catalog, plays: list[dict]) -> dict:
    """{"best": {(hash, course): play}, "titles": {hash: (title, subtitle)}, "matched": n songs,
    "several": [titles], "missing": [titles], "no_course": n plays whose chart lacks that course}"""
    best: dict[tuple[str, int], dict] = {}
    titles: dict[str, tuple[str, str]] = {}
    several, missing, seen, matched = [], [], set(), set()
    no_course = 0
    for p in plays:
        chart, n = catalog.find(p["title"])
        if chart is None:
            if p["no"] not in seen:
                seen.add(p["no"])
                (several if n else missing).append(p["title"])
            continue
        matched.add(p["no"])
        h = catalog.hashes[chart][p["course"]]
        if not h:
            no_course += 1
            continue
        play = {"hash": h, "difficulty": p["course"], "crown": p["crown"], "score": p["score"],
                "rank": rank_for_score(p["score"]), "good": p["good"], "ok": p["ok"], "bad": p["bad"],
                "drumroll": p["roll"], "max_combo": p["max_combo"], "played_at": 0, "modifiers": ""}
        cur = best.get((h, p["course"]))
        if cur is None or (play["crown"], play["score"]) > (cur["crown"], cur["score"]):
            best[(h, p["course"])] = play
        titles[h] = catalog.names.get(chart, ("", ""))
    return {"best": best, "titles": titles, "matched": len(matched), "several": several, "missing": missing,
            "no_course": no_course}
