"""The site's player and song statistics: rating, the rating leaderboard, the difficulty tables, songs, a player's page
and all their scores. A mixin of zdon_site.Site (it uses its store, page() and helpers).

Rating (as YataiDON-Syakai counts it):
- a chart's constant: its level plus its tier in the clear difficulty table (SSS +0.9, SS +0.8, S+ +0.7, S +0.6,
  A +0.5, B +0.4, C +0.3, D +0.2, E +0.1, F +0.0); charts in no table (or tier X) are unrated
- a play: constant x score factor + crown bonus; the factor is 0 under 600,000, then 1/6 at 600k, 2/6 at 700k,
  3/6 at 800k, 4/6 at 900k, 5/6 at 950k, 1 at 1,000,000, even in between; full combo +0.5, donderful combo +1.0
- a player: the 50 best charts (Oni and Ura only) summed and divided by 50 (missing slots count 0)

The tables are YataiDON-Syakai's (site_data/diffchart, GPL-3.0); their hashes are the game's per-course hashes.
"""
from __future__ import annotations

import json
import time
import urllib.parse
from pathlib import Path

import zdon_site as S

TIER_BONUS = {"SSS": 0.9, "SS": 0.8, "S+": 0.7, "S": 0.6, "A": 0.5, "B": 0.4, "C": 0.3, "D": 0.2, "E": 0.1, "F": 0.0}
FACTOR_STEPS = [(600_000, 1 / 6), (700_000, 2 / 6), (800_000, 3 / 6), (900_000, 4 / 6), (950_000, 5 / 6),
                (1_000_000, 1.0)]
CROWN_BONUS = {2: 0.5, 3: 1.0}
BEST_N = 50
RATED = (3, 4)  # Oni, Ura
TABLE_KIND = {"clear": "通關", "fc": "全連", "dfc": "全良"}
DIFF_OF = {"easy": 0, "normal": 1, "hard": 2, "oni": 3, "ura": 4, "edit": 4}
PAGE = 50


def score_factor(score: int) -> float:
    if score < FACTOR_STEPS[0][0]:
        return 0.0
    for (s0, f0), (s1, f1) in zip(FACTOR_STEPS, FACTOR_STEPS[1:]):
        if score < s1:
            return f0 + (f1 - f0) * (score - s0) / (s1 - s0)
    return 1.0


def play_rating(constant: float | None, score: int, crown: int) -> float:
    if constant is None:
        return 0.0
    return constant * score_factor(int(score or 0)) + CROWN_BONUS.get(int(crown or 0), 0.0)


class Tables:
    """The difficulty tables: what each chart's constant is, and the tables to show."""

    def __init__(self, folder: Path):
        self.tables: list[dict] = []           # {"id", "level", "type", "sections": [{"name", "charts": [...]}]}
        self.constant: dict[tuple[str, int], float] = {}
        self.place: dict[tuple[str, int], str] = {}   # "★10 S" (the clear table's level and tier)
        for f in sorted(folder.glob("*.json")):
            try:
                j = json.loads(f.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            t = {"id": f.stem, "level": int(j.get("level", 0)), "type": j.get("type", ""), "sections": []}
            for sec in j.get("data", {}).get("sections", []):
                charts = []
                for song in sec.get("songs", []):
                    d = DIFF_OF.get(str(song.get("difficulty", "")).lower())
                    if d is None or not song.get("hash"):
                        continue
                    charts.append({"hash": song["hash"], "difficulty": d, "title": song.get("title") or {}})
                    if t["type"] == "clear":
                        key = (song["hash"], d)
                        self.place[key] = f"★{t['level']} {sec.get('name', '')}"
                        if sec.get("name") in TIER_BONUS:
                            self.constant[key] = t["level"] + TIER_BONUS[sec["name"]]
                t["sections"].append({"name": sec.get("name", ""), "charts": charts})
            self.tables.append(t)
        self.tables.sort(key=lambda t: (-t["level"], ["clear", "fc", "dfc"].index(t["type"])
                                        if t["type"] in ("clear", "fc", "dfc") else 9))

    def get(self, tid: str) -> dict | None:
        return next((t for t in self.tables if t["id"] == tid), None)

    def label(self, t: dict) -> str:
        return f"★{t['level']} {TABLE_KIND.get(t['type'], t['type'])}"

    def charts(self, t: dict) -> list[tuple[str, int]]:
        return [(c["hash"], c["difficulty"]) for s in t["sections"] for c in s["charts"]]


def gauge(possible: int, clear: int, fc: int, dfc: int) -> str:
    """Cumulative clear >= fc >= dfc, as the dan gauge: rainbow, gold, silver."""
    def pct(n):
        return 0.0 if possible == 0 else min(100.0, n * 100.0 / possible)
    return (f'<div class="gauge" title="{clear} / {possible}"><div><div class="g-dfc" style="width:{pct(dfc):.1f}%">'
            f'</div><div class="g-fc" style="width:{pct(fc) - pct(dfc):.1f}%"></div>'
            f'<div class="g-clear" style="width:{pct(clear) - pct(fc):.1f}%"></div></div></div>')


STATS_CSS = """
.gauge{height:18px;padding:3px;border-radius:6px;background:#1a0a08;position:relative;min-width:120px}
.gauge>div{display:flex;height:100%;border-radius:3px;overflow:hidden;background:#5c140d}
.gauge::after{content:'';position:absolute;inset:3px;pointer-events:none;
background:repeating-linear-gradient(90deg,transparent 0 8px,rgba(0,0,0,.5) 8px 10px)}
.g-clear{background:linear-gradient(#f2f4f7,#aeb4bc)}.g-fc{background:linear-gradient(#ffe27a,#e0a417)}
.g-dfc{background:linear-gradient(90deg,#ff3b3b,#ff9f1a,#ffe81a,#3bd84a,#26b7ff,#9a4dff)}
.tier{display:grid;grid-template-columns:4.2em 1fr;gap:.6em;border-bottom:1px solid var(--line);padding:.5em 0}
.tier>b{font-size:1.15em;text-align:center;align-self:start;padding-top:.15em}
.chips{display:flex;flex-wrap:wrap;gap:.35em}.chip{border:1px solid var(--line);border-radius:6px;padding:.15em .5em;
font-size:.86em;background:#fff}.chip.k1{background:#eceef1}.chip.k2{background:#ffefb3;border-color:#e0a417}
.chip.k3{background:linear-gradient(90deg,#ffd6d6,#fff3c4,#d8f5d6,#d4eeff,#ead9ff);border-color:#9a4dff}
.chip .u{color:var(--muted);font-size:.8em}.tabs{display:flex;flex-wrap:wrap;gap:.4em;margin:.6em 0}
.tabs a,.tabs span{padding:.25em .7em;border:1px solid var(--line);border-radius:6px;background:#fff;font-size:.9em}
.tabs span{background:var(--ink);color:#fff;border-color:var(--ink)}.big{font-size:2em;font-weight:800}
.pager{display:flex;gap:1em;justify-content:center;margin:1em 0}details summary{cursor:pointer}
th{white-space:nowrap}
"""


class StatsPages:
    """Mixed into zdon_site.Site."""

    _tables: Tables | None = None
    _board_cache: tuple[float, list] = (0.0, [])
    _songs_cache: dict[str, tuple[float, list]] = {}  # /songs?q=, by q

    @property
    def tables(self) -> Tables:
        if StatsPages._tables is None:
            StatsPages._tables = Tables(Path(__file__).parent / "site_data" / "diffchart")
        return StatsPages._tables

    # ------------------------------------------------------------------------------------------------------------
    # rating

    def _best_charts(self, codes: list[str], rated_only: bool = False) -> dict[tuple[str, int], dict]:
        """Per chart: the best score, the best crown and the best rating over all plays of these accounts."""
        if not codes:
            return {}
        marks = ",".join("?" * len(codes))
        extra = " AND difficulty IN (3,4)" if rated_only else ""
        out: dict[tuple[str, int], dict] = {}
        for r in self.s.q(f"""SELECT hash, difficulty, score, crown, received_at FROM scores
                              WHERE access_code IN ({marks}){extra}""", codes):
            key = (r["hash"], r["difficulty"])
            rating = play_rating(self.tables.constant.get(key), r["score"], r["crown"]) if key[1] in RATED else 0.0
            b = out.get(key)
            if b is None:
                out[key] = {"score": r["score"] or 0, "crown": r["crown"] or 0, "rating": rating,
                            "last": r["received_at"]}
            else:
                b["score"] = max(b["score"], r["score"] or 0)
                b["crown"] = max(b["crown"], r["crown"] or 0)
                b["rating"] = max(b["rating"], rating)
                b["last"] = max(b["last"] or 0, r["received_at"] or 0)
        return out

    @staticmethod
    def _rating_of(best: dict) -> tuple[float, list]:
        top = sorted(((k, v) for k, v in best.items() if k[1] in RATED and v["rating"] > 0),
                     key=lambda kv: -kv[1]["rating"])[:BEST_N]
        return sum(v["rating"] for _, v in top) / BEST_N, top

    @staticmethod
    def invalidate_board():
        StatsPages._board_cache = (0.0, [])
        StatsPages._songs_cache = {}

    def leaderboard_rows(self) -> list[dict]:
        """Every player with a rated play, best first (kept a minute)."""
        at, rows = StatsPages._board_cache
        if time.time() - at < 60:
            return rows
        per: dict[str, dict] = {}
        for r in self.s.q("""SELECT s.access_code, s.hash, s.difficulty, MAX(s.score) AS score, MAX(s.crown) AS crown
                             FROM scores s WHERE s.difficulty IN (3,4) GROUP BY s.access_code, s.hash, s.difficulty"""):
            p = per.setdefault(r["access_code"], {"ratings": [], "clear": 0, "fc": 0, "dfc": 0})
            crown = r["crown"] or 0
            p["clear"] += crown >= 1
            p["fc"] += crown >= 2
            p["dfc"] += crown >= 3
            p["ratings"].append(play_rating(self.tables.constant.get((r["hash"], r["difficulty"])), r["score"], crown))
        users = {u["access_code"]: u for u in self.s.q("SELECT rowid AS pid, access_code, username FROM users")}
        off = self.hidden_codes("no_board") | self.hidden_codes("private")  # zdon_site_me.py: privacy
        rows = []
        for code, p in per.items():
            u = users.get(code)
            if u is None or code in off:
                continue
            rating = sum(sorted(p["ratings"], reverse=True)[:BEST_N]) / BEST_N
            rows.append({"pid": u["pid"], "username": u["username"], "rating": rating, "clear": p["clear"],
                         "fc": p["fc"], "dfc": p["dfc"]})
        rows.sort(key=lambda x: (-x["rating"], -x["dfc"], -x["fc"], -x["clear"]))
        StatsPages._board_cache = (time.time(), rows)
        return rows

    def _formula(self) -> str:
        return """<details class="small"><summary class="muted">Rating 怎麼算？</summary><div class="card small">
<p><code>Rating = 最好的 50 個譜面的 Rating 加總 ÷ 50</code><br><code>譜面 Rating = 譜面常數 × 分數係數 + 王冠加分</code></p>
<ul class="muted"><li>譜面常數：星數加上它在<a href="/tables">通關難易度表</a>的等級（SSS +0.9、SS +0.8、S+ +0.7、S +0.6、A +0.5、
B +0.4、C +0.3、D +0.2、E +0.1、F +0）。不在表上的譜面不計。</li>
<li>分數係數：60 萬分以下是 0；60 萬 1/6、70 萬 2/6、80 萬 3/6、90 萬 4/6、95 萬 5/6、100 萬為 1，中間平均增加。</li>
<li>王冠加分：全連 +0.5、全良 +1.0。</li><li>只算魔王與裏魔王；不到 50 個譜面時，空的位置算 0。</li></ul>
<p class="muted">算法與難易度表取自 <a href="https://github.com/Yonokid/YataiDON-Syakai">YataiDON-Syakai</a>（GPL-3.0）。</p>
</div></details>"""

    # ------------------------------------------------------------------------------------------------------------
    # pages

    def leaderboard(self, req, query):
        rows = self.leaderboard_rows()
        head = f"<h1>Rating 排行</h1>{self._formula()}"
        if not rows:
            self.page(req, "Rating 排行", head + '<p class="muted">還沒有魔王或裏魔王的成績。</p>')
            return
        crown = S.crown_badge
        out = [f'<div class="card"><table><tr><th class="num">#</th><th>玩家</th><th class="num">Rating</th>'
               f'<th class="num">{crown(1)}</th><th class="num">{crown(2)}</th><th class="num">{crown(3)}</th></tr>']
        for i, r in enumerate(rows[:200], 1):
            out.append(f'<tr><td class="rk"><span class="rank r{i}">{i}</span></td>'
                       f'<td><a href="/player/{r["pid"]}">{S.esc(r["username"])}</a></td>'
                       f'<td class="num"><b>{r["rating"]:.2f}</b></td><td class="num">{r["clear"]}</td>'
                       f'<td class="num">{r["fc"]}</td><td class="num">{r["dfc"]}</td></tr>')
        out.append('</table><p class="small muted">王冠數：魔王與裏魔王的譜面數（全良也算在全連和通關裡）。</p></div>')
        self.page(req, "Rating 排行", head + "".join(out))

    def _viewer(self, req, query) -> tuple[list[str], str, str]:
        """Whose crowns a table shows: ?p=<player id>, else the member's linked game accounts.
        Returns (access codes, name, the ?p= part for links)."""
        pid = query.get("p", "")
        if pid.isdigit():
            u = self.s.one("SELECT access_code, username FROM users WHERE rowid=?", (int(pid),))
            if u and (not self.prefs(u["access_code"])["private"] or self.viewer_owns(req, u["access_code"])):
                return [u["access_code"]], u["username"], f"?p={pid}"
        if req.sess:
            linked = self._linked(req.sess["uid"])
            if linked:
                return [r["access_code"] for r in linked], "、".join(r["username"] for r in linked), ""
        return [], "", ""

    def tables_index(self, req, query):
        codes, who, pq = self._viewer(req, query)
        best = self._best_charts(codes) if codes else {}
        rows = []
        for t in self.tables.tables:
            charts = self.tables.charts(t)
            line = ""
            if codes:
                got = [best.get(k, {}).get("crown", 0) for k in charts]
                line = (f'<td style="width:40%">{gauge(len(charts), sum(c >= 1 for c in got), sum(c >= 2 for c in got), sum(c >= 3 for c in got))}</td>'
                        f'<td class="num small">{sum(c >= {"clear": 1, "fc": 2, "dfc": 3}.get(t["type"], 1) for c in got)} / {len(charts)}</td>')
            rows.append(f'<tr><td><a href="/tables/{S.esc(t["id"])}{pq}">{S.esc(self.tables.label(t))}</a></td>'
                        f'<td class="num">{len(charts)}</td>{line}</tr>')
        who_line = (f'<p class="small muted">顯示 <b>{S.esc(who)}</b> 的王冠。</p>' if codes else
                    '<p class="small muted">登入並綁定遊戲帳號後，這裡會標出你的王冠。</p>')
        body = (f'<h1>難易度表</h1>{who_line}<div class="card"><table><tr><th>表</th><th class="num">譜面</th>' +
                ('<th>進度</th><th class="num">達成</th>' if codes else '') + '</tr>' + "".join(rows) +
                '</table></div><p class="small muted">難易度表取自 <a href="https://github.com/Yonokid/YataiDON-Syakai">'
                'YataiDON-Syakai</a>（GPL-3.0）。</p>')
        self.page(req, "難易度表", body)

    def table_page(self, req, tid: str, query):
        t = self.tables.get(urllib.parse.unquote(tid))
        if t is None:
            self.page(req, "找不到", "<h1>找不到這個難易度表</h1>", 404)
            return
        codes, who, pq = self._viewer(req, query)
        best = self._best_charts(codes) if codes else {}
        known = {r["hash"] for r in self.s.q("SELECT DISTINCT hash FROM scores")}
        names = {r["hash"]: r["title"] for r in self.s.q("SELECT hash, title FROM songs WHERE title != ''")}
        tabs = "".join(f'<span>{S.esc(self.tables.label(x))}</span>' if x is t else
                       f'<a href="/tables/{S.esc(x["id"])}{pq}">{S.esc(self.tables.label(x))}</a>'
                       for x in self.tables.tables)
        need = {"clear": 1, "fc": 2, "dfc": 3}.get(t["type"], 1)
        charts = self.tables.charts(t)
        got = [best.get(k, {}).get("crown", 0) for k in charts]
        parts = [f'<h1>{S.esc(self.tables.label(t))} 難易度表</h1><div class="tabs">{tabs}</div>']
        if codes:
            parts.append(f'<div class="card"><p class="small" style="margin-top:0"><b>{S.esc(who)}</b>：'
                         f'{sum(c >= need for c in got)} / {len(charts)}</p>'
                         f'{gauge(len(charts), sum(c >= 1 for c in got), sum(c >= 2 for c in got), sum(c >= 3 for c in got))}</div>')
        else:
            parts.append('<p class="small muted">登入並綁定遊戲帳號後，會標出你的王冠。</p>')
        parts.append('<div class="card">')
        for sec in t["sections"]:
            chips = []
            for c in sec["charts"]:
                title = names.get(c["hash"]) or c["title"].get("ja") or c["title"].get("zh") or c["title"].get("en") or c["hash"][:10]
                crown = best.get((c["hash"], c["difficulty"]), {}).get("crown", 0)
                ura = ' <span class="u">裏</span>' if c["difficulty"] == 4 else ""
                text = f'{S.esc(title)}{ura}'
                if c["hash"] in known:
                    text = f'<a href="/chart/{S.esc(c["hash"])}?d={c["difficulty"]}">{text}</a>'
                chips.append(f'<span class="chip k{crown}">{text}</span>')
            parts.append(f'<div class="tier"><b>{S.esc(sec["name"])}</b><div class="chips">{"".join(chips)}</div></div>')
        parts.append('</div><p class="small muted">底色：灰＝通關、金＝全連、彩虹＝全良。有連結的曲目在本站有成績。'
                     '難易度表取自 <a href="https://github.com/Yonokid/YataiDON-Syakai">YataiDON-Syakai</a>（GPL-3.0）。</p>')
        self.page(req, self.tables.label(t), "".join(parts))

    # ------------------------------------------------------------------------------------------------------------
    # songs

    def songs(self, req, query):
        q = (query.get("q") or "").strip()
        args: list = []
        where = ""
        if q:
            where = "WHERE g.title LIKE ? OR g.subtitle LIKE ?"
            args = [f"%{q}%", f"%{q}%"]
        # the same search within a minute is answered from memory (the query scans every score: LIKE '%q%')
        hit = StatsPages._songs_cache.get(q)
        if hit and time.time() - hit[0] < 60:
            rows = hit[1]
        else:
            rows = self.s.q(f"""SELECT COALESCE(g.title, s.hash) AS title, COALESCE(g.subtitle, '') AS subtitle,
                                       COUNT(*) AS plays, COUNT(DISTINCT s.access_code) AS players,
                                       MAX(s.received_at) AS last, GROUP_CONCAT(DISTINCT s.difficulty) AS diffs,
                                       MAX(s.hash) AS hash
                                FROM scores s LEFT JOIN songs g ON g.hash = s.hash {where}
                                GROUP BY COALESCE(g.title, s.hash), COALESCE(g.subtitle, '')
                                ORDER BY plays DESC, last DESC LIMIT 300""", args)
            if len(StatsPages._songs_cache) >= 64:
                StatsPages._songs_cache.clear()
            StatsPages._songs_cache[q] = (time.time(), rows)
        body = [f"""<h1>歌曲</h1><form method="get"><input type="text" name="q" value="{S.esc(q)}" placeholder="搜尋曲名">
<button class="btn small sec">搜尋</button></form>"""]
        if not rows:
            body.append('<p class="muted">沒有符合的曲目。</p>')
        else:
            body.append('<div class="card"><table><tr><th>曲目</th><th>難度</th><th class="num">玩家</th>'
                        '<th class="num">遊玩</th><th>最後</th></tr>')
            for r in rows:
                diffs = "".join(S.course_badge(int(d)) + " " for d in sorted({int(x) for x in str(r["diffs"]).split(",") if x}))
                sub = f'<div class="small muted">{S.esc(r["subtitle"])}</div>' if r["subtitle"] else ""
                body.append(f'<tr><td><a href="/song/{S.esc(r["hash"])}">{S.esc(r["title"])}</a>{sub}</td><td>{diffs}</td>'
                            f'<td class="num">{r["players"]}</td><td class="num">{r["plays"]}</td>'
                            f'<td class="small muted">{S.when(r["last"])}</td></tr>')
            body.append("</table></div>")
        self.page(req, "歌曲", "".join(body))

    def song(self, req, h: str):
        h = urllib.parse.unquote(h)
        g = self.s.one("SELECT title, subtitle FROM songs WHERE hash=?", (h,))
        if g and g["title"]:
            hashes = [r["hash"] for r in self.s.q("SELECT hash FROM songs WHERE title=? AND COALESCE(subtitle,'')=?",
                                                    (g["title"], g["subtitle"] or ""))]
        else:
            hashes = [h]
        marks = ",".join("?" * len(hashes))
        charts = self.s.q(f"""SELECT DISTINCT hash, difficulty FROM scores WHERE hash IN ({marks})
                              ORDER BY difficulty DESC""", hashes)
        if not charts:
            self.page(req, "找不到", "<h1>這首歌還沒有成績</h1>", 404)
            return
        title = g["title"] if g and g["title"] else h[:12] + "…"
        parts = [f'<h1>{S.esc(title)}</h1>']
        if g and g["subtitle"]:
            parts.append(f'<p class="muted">{S.esc(g["subtitle"])}</p>')
        if req.sess and self._linked(req.sess["uid"]):
            parts.append(f'<form method="post" action="/me/jump"><input type="hidden" name="csrf" value="{self._csrf(req.sess)}">'
                         f'<input type="hidden" name="hash" value="{S.esc(charts[0]["hash"])}">'
                         f'<button class="btn small sec">在我的遊戲中開啟這首</button></form>')
        parts.append('<div class="grid">')
        for c in charts:
            key = (c["hash"], c["difficulty"])
            place = self.tables.place.get(key, "")
            const = self.tables.constant.get(key)
            info = (f' <span class="small muted">{S.esc(place)}' + (f'・常數 {const:.1f}' if const else '') + '</span>') if place else ""
            rows = self.s.q("""SELECT u.username, u.rowid AS pid, MAX(s.score) AS score, MAX(s.crown) AS crown
                               FROM scores s JOIN users u USING(access_code) WHERE s.hash=? AND s.difficulty=?
                               GROUP BY s.access_code ORDER BY score DESC LIMIT 10""", key)
            table = "".join(f'<tr><td class="num">{i}</td><td><a href="/player/{r["pid"]}">{S.esc(r["username"])}</a></td>'
                            f'<td class="num">{r["score"]:,}</td><td>{S.crown_badge(r["crown"])}</td></tr>'
                            for i, r in enumerate(rows, 1))
            parts.append(f'<div class="card"><h2 style="margin-top:0">{S.course_badge(c["difficulty"])}{info}</h2>'
                         f'<table>{table}</table><p class="small"><a href="/chart/{S.esc(c["hash"])}?d={c["difficulty"]}">'
                         f'完整排行與挑戰 →</a></p></div>')
        parts.append('</div>')
        self.page(req, title, "".join(parts))

    # ------------------------------------------------------------------------------------------------------------
    # a player

    def player_page(self, req, pid: str):
        u = self.s.one("SELECT rowid AS pid, * FROM users WHERE rowid=?", (int(pid),)) if pid.isdigit() else None
        if u is None:
            self.page(req, "找不到", "<h1>找不到這位玩家</h1>", 404)
            return
        if self.prefs(u["access_code"])["private"] and not self.viewer_owns(req, u["access_code"]):
            self.page(req, u["username"], f'<h1>{S.esc(u["username"])}</h1><p class="muted">這位玩家的頁面不公開。</p>')
            return
        code = u["access_code"]
        best = self._best_charts([code])
        rating, top = self._rating_of(best)
        board = self.leaderboard_rows()
        rank = next((i for i, r in enumerate(board, 1) if r["pid"] == u["pid"]), None)
        plays = self.s.one("SELECT COUNT(*) AS n FROM scores WHERE access_code=?", (code,))["n"]
        crowns = [v["crown"] for v in best.values()]
        from zdon_site_plate import PLATE_JS, nameplate_html
        head = f"""<h1 style="display:flex;align-items:center;gap:.6em;flex-wrap:wrap">{nameplate_html(u["username"], u["title"] or "", int(u["title_bg"] or 0), 1.3, dan=u["plate_dan"], gold=bool(u["plate_gold"]), rainbow=bool(u["plate_rainbow"]))}</h1>{PLATE_JS}
<div class="grid"><div class="card"><div class="muted small">Rating</div><div class="big">{rating:.2f}</div>
<div class="small muted">{f'全站第 {rank} 名' if rank else '還沒有排名'}・<a href="/leaderboard">排行</a></div></div>
<div class="card"><div class="muted small">遊玩</div><div class="big">{plays}</div><div class="small muted">{len(best)} 個譜面・
通關 {sum(c >= 1 for c in crowns)}・全連 {sum(c >= 2 for c in crowns)}・全良 {sum(c >= 3 for c in crowns)}</div></div></div>"""
        # crowns against the clear tables, a line per level
        lines = []
        for t in self.tables.tables:
            if t["type"] != "clear":
                continue
            charts = self.tables.charts(t)
            got = [best.get(k, {}).get("crown", 0) for k in charts]
            c1, c2, c3 = sum(c >= 1 for c in got), sum(c >= 2 for c in got), sum(c >= 3 for c in got)
            lines.append(f'<tr><td><a href="/tables/{S.esc(t["id"])}?p={u["pid"]}">★{t["level"]}</a></td>'
                         f'<td style="width:55%">{gauge(len(charts), c1, c2, c3)}</td>'
                         f'<td class="num small">{c1}・{c2}・{c3} / {len(charts)}</td></tr>')
        gauge_card = (f'<div class="card"><h2 style="margin-top:0">王冠統計</h2><p class="small muted">對照難易度表：'
                      f'通關・全連・全良 / 譜面數</p><table>{"".join(lines)}</table></div>') if lines else ""
        # best 50
        if top:
            rows = "".join(f'<tr><td class="num">{i}</td><td><a href="/song/{S.esc(k[0])}">{S.esc(self.song_name(k[0]))}</a></td>'
                           f'<td>{S.course_badge(k[1])}</td><td class="small muted">{S.esc(self.tables.place.get(k, ""))}</td>'
                           f'<td class="num">{v["score"]:,}</td><td>{S.crown_badge(v["crown"])}</td>'
                           f'<td class="num">{v["rating"]:.2f}</td></tr>' for i, (k, v) in enumerate(top, 1))
            best_card = (f'<div class="card"><h2 style="margin-top:0">Best 50</h2>{self._formula()}<table><tr><th class="num">#</th>'
                         f'<th>曲目</th><th>難度</th><th>表</th><th class="num">分數</th><th></th><th class="num">Rating</th></tr>'
                         f'{rows}</table></div>')
        else:
            best_card = ('<div class="card"><h2 style="margin-top:0">Best 50</h2><p class="muted">還沒有計入 Rating 的成績'
                         '（難易度表上的魔王／裏魔王）。</p></div>')
        # dan
        dans = self.s.q("""SELECT title, MAX(odai_result) AS odai, MAX(score) AS score, COUNT(*) AS n, MAX(received_at) AS last
                           FROM dan_results WHERE access_code=? GROUP BY title ORDER BY MAX(dan_index) DESC, title""", (code,))
        dan_card = ""
        if dans:
            def verdict(o):
                o = int(o or 0)
                if o == 0:
                    return '<span class="muted">不合格</span>'
                kind = "金合格" if o >= 4 else "合格"
                extra = {2: "・全連", 3: "・全良", 5: "・全連", 6: "・全良"}.get(o, "")
                return f'<b>{kind}</b>{extra}'
            dan_rows = "".join(f'<tr><td>{S.esc(d["title"])}</td><td>{verdict(d["odai"])}</td><td class="num">{(d["score"] or 0):,}</td>'
                               f'<td class="num">{d["n"]}</td><td class="small muted">{S.when(d["last"])}</td></tr>' for d in dans)
            dan_card = (f'<div class="card"><h2 style="margin-top:0">段位</h2><table><tr><th>段位</th><th>最佳結果</th>'
                        f'<th class="num">分數</th><th class="num">挑戰</th><th>最後</th></tr>{dan_rows}</table></div>')
        recent = self.s.q("""SELECT s.received_at, s.hash, s.difficulty, s.score, s.crown, ? AS username, ? AS pid
                             FROM scores s WHERE access_code=? ORDER BY id DESC LIMIT 15""", (u["username"], u["pid"], code))
        recent_card = (f'<div class="card"><h2 style="margin-top:0">最近遊玩</h2>{self._plays_table(recent, show_player=False)}'
                       f'<p class="small"><a href="/player/{u["pid"]}/scores">全部成績 →</a></p></div>')
        self.page(req, u["username"], head + gauge_card + best_card + dan_card + recent_card)

    def player_scores(self, req, pid: str, query):
        u = self.s.one("SELECT rowid AS pid, * FROM users WHERE rowid=?", (int(pid),)) if pid.isdigit() else None
        if u is None:
            self.page(req, "找不到", "<h1>找不到這位玩家</h1>", 404)
            return
        if self.prefs(u["access_code"])["private"] and not self.viewer_owns(req, u["access_code"]):
            self.page(req, u["username"], f'<h1>{S.esc(u["username"])}</h1><p class="muted">這位玩家的頁面不公開。</p>')
            return
        q = (query.get("q") or "").strip().lower()
        d = query.get("d", "")
        sort = query.get("sort", "recent")
        best = self._best_charts([u["access_code"]])
        names = {r["hash"]: (r["title"] or "") for r in self.s.q("SELECT hash, title FROM songs")}
        items = []
        for (h, diff), v in best.items():
            name = names.get(h) or h[:12] + "…"
            if q and q not in name.lower():
                continue
            if d.isdigit() and int(d) != diff:
                continue
            items.append((h, diff, name, v))
        keys = {"score": lambda x: -x[3]["score"], "rating": lambda x: -x[3]["rating"], "title": lambda x: x[2].lower(),
                "recent": lambda x: -(x[3]["last"] or 0)}
        items.sort(key=keys.get(sort, keys["recent"]))
        page = max(1, int(query.get("page", "1"))) if query.get("page", "1").isdigit() else 1
        pages = max(1, (len(items) + PAGE - 1) // PAGE)
        page = min(page, pages)
        shown = items[(page - 1) * PAGE: page * PAGE]

        def link(**kw):
            args = {"q": query.get("q", ""), "d": d, "sort": sort, "page": str(page)}
            args.update(kw)
            return "?" + urllib.parse.urlencode({k: v for k, v in args.items() if v})
        opts = "".join(f'<option value="{i}"{" selected" if d == str(i) else ""}>{S.COURSES[i]}</option>' for i in range(5))
        sorts = "".join(f'<option value="{k}"{" selected" if sort == k else ""}>{v}</option>'
                        for k, v in (("recent", "最近"), ("score", "分數"), ("rating", "Rating"), ("title", "曲名")))
        form = (f'<form method="get" class="small"><input type="text" name="q" value="{S.esc(query.get("q", ""))}" '
                f'placeholder="搜尋曲名"> <select name="d"><option value="">全部難度</option>{opts}</select> '
                f'<select name="sort">{sorts}</select> <button class="btn small sec">篩選</button></form>')
        rows = "".join(f'<tr><td><a href="/song/{S.esc(h)}">{S.esc(name)}</a></td><td>{S.course_badge(diff)}</td>'
                       f'<td class="num">{v["score"]:,}</td><td>{S.crown_badge(v["crown"])}</td>'
                       f'<td class="num">{f"{v["rating"]:.2f}" if v["rating"] else ""}</td>'
                       f'<td class="small muted">{S.when(v["last"])}</td></tr>' for h, diff, name, v in shown)
        pager = ""
        if pages > 1:
            prev = f'<a href="{link(page=str(page - 1))}">← 上一頁</a>' if page > 1 else '<span></span>'
            nxt = f'<a href="{link(page=str(page + 1))}">下一頁 →</a>' if page < pages else '<span></span>'
            pager = f'<div class="pager">{prev}<span class="muted small">{page} / {pages}</span>{nxt}</div>'
        table = (f'<table><tr><th>曲目</th><th>難度</th><th class="num">分數</th><th></th><th class="num">Rating</th>'
                 f'<th>最後</th></tr>{rows}</table>') if shown else '<p class="muted">沒有符合的成績。</p>'
        body = (f'<h1><a href="/player/{u["pid"]}">{S.esc(u["username"])}</a> 的全部成績</h1>{form}'
                f'<p class="small muted">每個譜面的最佳分數與王冠，共 {len(items)} 個。</p><div class="card">{table}</div>{pager}')
        self.page(req, f'{u["username"]} 的成績', body)
