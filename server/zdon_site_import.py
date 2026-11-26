"""Import scores from a game's scores.db on my page (/me/import). A mixin of zdon_site.Site.

1. GET  /me/import?code=<access code>   the upload form for one of the member's linked game accounts
2. POST /me/import/upload (multipart)   the file is kept (a random name signed with the member's id, at most an hour)
                                        and the players in it are listed; the one whose player_id is the account's
                                        access code (the game's online player) is picked already
3. POST /me/import/run                  imports that player's scores into the account and deletes the file

The file is opened read-only (immutable, query_only, trusted_schema off) and only plain SELECTs run on its players,
scores and songs tables; nothing in it is executed. A chart's best play in the file (crown first, then score: the
game's own "keep the best" rule, scores.cpp import_from_pytaiko) is added as one play when it beats the account's best
on this server for that chart; a play that isn't better is skipped, so nothing better is ever covered. Plays already
here stay as they are (the server keeps every play; pages show the best). There is no input log, so no replay.
"""
from __future__ import annotations

import json
import re
import secrets
import sqlite3
import time
import urllib.parse
from pathlib import Path

import zdon_site as S

UPLOAD_MAX = 64 * 1024 * 1024
UPLOAD_KEEP = 3600            # seconds an uploaded file waits for step 3
UPLOADS_PER_HOUR = 10
MAX_ROWS = 300_000            # plays read from one file
QUERY_SECONDS = 20
SQLITE_MAGIC = b"SQLite format 3\x00"
HASH = re.compile(r"^[0-9A-Za-z]{1,128}$")
TOKEN = re.compile(r"^[0-9A-Za-z_-]{16,64}$")

IMPORT_SCHEMA = """
CREATE TABLE IF NOT EXISTS score_uploads (forge_uid TEXT NOT NULL, at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS score_imports (
    id INTEGER PRIMARY KEY AUTOINCREMENT, forge_uid TEXT NOT NULL, access_code TEXT NOT NULL, player_id TEXT,
    added INTEGER NOT NULL, not_better INTEGER NOT NULL, invalid INTEGER NOT NULL, at INTEGER NOT NULL
);
"""


class BadFile(Exception):
    pass


def parse_multipart(ctype: str, body: bytes) -> dict[str, tuple[str | None, bytes]] | None:
    """multipart/form-data -> {name: (filename or None, data)}; None when it isn't well formed."""
    m = re.search(r'boundary="?([^";]+)"?', ctype)
    if "multipart/form-data" not in ctype or not m:
        return None
    delim = b"--" + m.group(1).encode("latin-1")
    if not body.startswith(delim):
        return None
    out: dict[str, tuple[str | None, bytes]] = {}
    for chunk in body[len(delim):].split(b"\r\n" + delim):
        if chunk.startswith(b"--"):
            return out
        if not chunk.startswith(b"\r\n"):
            return None
        head, sep, data = chunk[2:].partition(b"\r\n\r\n")
        if not sep:
            return None
        disp = ""
        for line in head.decode("utf-8", "replace").split("\r\n"):
            k, _, v = line.partition(":")
            if k.strip().lower() == "content-disposition":
                disp = v
        name = re.search(r'(?:^|;)\s*name="([^"]*)"', disp)
        if name:
            fn = re.search(r'(?:^|;)\s*filename="([^"]*)"', disp)
            out[name.group(1)] = (fn.group(1) if fn else None, data)
    return None  # no closing delimiter: cut off


class GameDb(sqlite3.Connection):
    """An uploaded scores.db, with what open_scores_db found in it."""
    cols: set[str] = set()
    kinds: dict[str, str] = {}


def open_scores_db(path: Path) -> GameDb:
    """The uploaded file, read-only; BadFile when it isn't a game's scores.db."""
    with path.open("rb") as f:
        if f.read(16) != SQLITE_MAGIC:
            raise BadFile("這不是 SQLite 資料庫檔。")
    uri = "file:" + urllib.parse.quote(path.resolve().as_posix(), safe="/:") + "?mode=ro&immutable=1"
    con = sqlite3.connect(uri, uri=True, check_same_thread=False, factory=GameDb)
    try:
        con.row_factory = sqlite3.Row
        for pragma in ("PRAGMA query_only=ON", "PRAGMA trusted_schema=OFF"):
            try:
                con.execute(pragma)
            except sqlite3.DatabaseError:
                pass
        deadline = time.monotonic() + QUERY_SECONDS
        con.set_progress_handler(lambda: int(time.monotonic() > deadline), 10_000)
        try:
            con.kinds = {r["name"]: r["type"] for r in con.execute("SELECT name, type FROM sqlite_master")}
        except sqlite3.DatabaseError as e:
            raise BadFile("無法讀取這個資料庫（檔案可能損壞）。") from e
        if con.kinds.get("scores") != "table" or con.kinds.get("players") != "table":
            raise BadFile("檔案裡沒有遊戲的成績表（scores、players）；請選遊戲資料夾裡的 scores.db。")
        con.cols = {r["name"] for r in con.execute("PRAGMA table_info(scores)")}
        if not {"player_id", "hash", "difficulty", "crown", "score"} <= con.cols:
            raise BadFile("成績表少了必要的欄位，不是遊戲的 scores.db。")
    except BaseException:
        con.close()
        raise
    return con


def file_players(con: GameDb) -> list[dict]:
    """The players in the file that have scores: id, name, plays, charts, last played."""
    names = {}
    try:
        names = {str(r[0]): r[1] for r in con.execute("SELECT player_id, username FROM players LIMIT 1000")}
    except sqlite3.DatabaseError:
        pass
    played = "MAX(played_at)" if "played_at" in con.cols else "0"
    rows = con.execute(f"""SELECT CAST(player_id AS TEXT) AS pid, COUNT(*) AS plays,
                                  COUNT(DISTINCT hash || '/' || difficulty) AS charts, {played} AS last
                           FROM scores WHERE hash IS NOT NULL AND hash != '' GROUP BY pid
                           ORDER BY plays DESC LIMIT 50""").fetchall()
    return [{"id": r["pid"], "name": str(names.get(r["pid"]) or ""), "plays": r["plays"], "charts": r["charts"],
             "last": r["last"] or 0} for r in rows]


def _int(v, lo: int, hi: int) -> int | None:
    if isinstance(v, bool) or not isinstance(v, (int, float, str)):
        return None
    try:
        n = int(v)
    except (TypeError, ValueError):
        return None
    return n if lo <= n <= hi else None


def clean_play(r: sqlite3.Row, cols: set[str], now: int) -> dict | None:
    """One row of the file's scores table as a play for this server, or None when a value is out of range."""
    h = r["hash"]
    if not isinstance(h, str) or not HASH.match(h):
        return None
    p = {"hash": h, "difficulty": _int(r["difficulty"], 0, 4), "crown": _int(r["crown"], 0, 3),
         "score": _int(r["score"], 0, 9_999_999)}
    for c, hi in (("rank", 100), ("good", 100_000), ("ok", 100_000), ("bad", 100_000), ("drumroll", 100_000),
                  ("max_combo", 100_000)):
        p[c] = _int(r[c], 0, hi) if c in cols else 0
    if any(v is None for v in p.values()):
        return None
    played = _int(r["played_at"], 1, now + 86400) if "played_at" in cols else None
    p["played_at"] = played or 0
    mods = r["modifiers"] if "modifiers" in cols else ""
    p["modifiers"] = mods[:500] if isinstance(mods, str) else ""
    return p


def file_titles(con: GameDb, hashes: set[str]) -> dict[str, tuple[str, str]]:
    if not hashes or con.kinds.get("songs") != "table":
        return {}
    cols = {r["name"] for r in con.execute("PRAGMA table_info(songs)")}
    hcols = [c for c in ("hash_0", "hash_1", "hash_2", "hash_3", "hash_4") if c in cols]
    if not hcols or "title" not in cols:
        return {}
    sub = "subtitle" if "subtitle" in cols else "''"
    out: dict[str, tuple[str, str]] = {}
    for r in con.execute(f"SELECT {', '.join(hcols)}, title, {sub} AS subtitle FROM songs LIMIT 200000"):
        for c in hcols:
            h = r[c]
            if h in hashes and h not in out and isinstance(r["title"], str) and r["title"]:
                out[h] = (r["title"][:200], (r["subtitle"] if isinstance(r["subtitle"], str) else "")[:200])
    return out


def import_player(store, con: GameDb, player_id: str, access_code: str) -> dict:
    """Adds the file's best play per chart for player_id where it beats the account's best here."""
    cols = con.cols
    now = int(time.time())
    sel = ", ".join(c if c in cols else f"0 AS {c}" for c in
                    ("hash", "difficulty", "crown", "rank", "score", "good", "ok", "bad", "drumroll", "max_combo",
                     "played_at"))
    sel += ", modifiers" if "modifiers" in cols else ", '' AS modifiers"
    best: dict[tuple[str, int], dict] = {}
    invalid = plays = 0
    for r in con.execute(f"SELECT {sel} FROM scores WHERE CAST(player_id AS TEXT) = ? LIMIT ?",
                         (player_id, MAX_ROWS)):
        plays += 1
        p = clean_play(r, cols | {"modifiers"}, now)
        if p is None:
            invalid += 1
            continue
        key = (p["hash"], p["difficulty"])
        cur = best.get(key)
        if cur is None or (p["crown"], p["score"]) > (cur["crown"], cur["score"]):
            best[key] = p
    have: dict[tuple[str, int], tuple[int, int]] = {}
    for r in store.q("SELECT hash, difficulty, crown, score FROM scores WHERE access_code=?", (access_code,)):
        k = (r["hash"], r["difficulty"])
        v = (r["crown"] or 0, r["score"] or 0)
        if k not in have or v > have[k]:
            have[k] = v
    new = [p for k, p in best.items() if k not in have or (p["crown"], p["score"]) > have[k]]
    new.sort(key=lambda p: (p["played_at"], p["hash"], p["difficulty"]))
    titles = file_titles(con, {p["hash"] for p in new})
    with store.lock:
        store.db.execute("BEGIN")
        try:
            for p in new:
                store.db.execute(
                    """INSERT INTO scores(access_code, hash, difficulty, score, good, ok, bad, drumroll, max_combo, crown,
                                          rank, input_log, played_at, modifiers, received_at)
                       VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                    (access_code, p["hash"], p["difficulty"], p["score"], p["good"], p["ok"], p["bad"], p["drumroll"],
                     p["max_combo"], p["crown"], p["rank"], "[]", p["played_at"] or None, p["modifiers"],
                     p["played_at"] or now))
            for h, (title, sub) in titles.items():
                store.db.execute("""INSERT INTO songs(hash, title, subtitle, updated_at) VALUES (?,?,?,?)
                                    ON CONFLICT(hash) DO NOTHING""", (h, title, sub, now))
            store.db.execute("COMMIT")
        except Exception:
            store.db.execute("ROLLBACK")
            raise
    added = [dict(p, before=have.get((p["hash"], p["difficulty"]))) for p in new]
    return {"plays": plays, "charts": len(best), "added": added, "not_better": len(best) - len(new),
            "invalid": invalid}


class ImportPages:
    """Mixed into zdon_site.Site."""

    def init_import(self):
        with self.s.lock:
            self.s.db.executescript(IMPORT_SCHEMA)
        self.upload_dir = Path(self.cfg["data_dir"]) / "score_uploads"
        self.upload_max = int(float(self.cfg.get("site", {}).get("score_upload_mb", 0) or 0) * 1024 * 1024) or UPLOAD_MAX

    def _upload_path(self, uid: str, token: str) -> Path:
        return self.upload_dir / (self._sign(f"score-upload:{uid}:{token}")[:40] + ".db")

    def _sweep_uploads(self) -> None:
        if not self.upload_dir.is_dir():
            return
        for f in self.upload_dir.glob("*.db"):
            try:
                if time.time() - f.stat().st_mtime > UPLOAD_KEEP:
                    f.unlink()
            except OSError:
                pass

    def _import_account(self, req, code: str):
        linked = self._linked(req.sess["uid"])
        return next((r for r in linked if r["access_code"] == code), linked[0] if linked and not code else None)

    def import_page(self, req, query, flash: str = "", err: bool = False, status: int = 200):
        if not req.sess:
            req.redirect("/auth/login?next=" + urllib.parse.quote("/me/import"))
            return
        acct = self._import_account(req, query.get("code", ""))
        if acct is None:
            self.page(req, "匯入成績", "<h1>匯入成績</h1><p>請先在<a href='/me'>我的頁面</a>綁定遊戲帳號。</p>",
                      flash=flash, err=err)
            return
        code = acct["access_code"]
        linked = self._linked(req.sess["uid"])
        pick = ""
        if len(linked) > 1:
            pick = '<div class="acct-tabs">' + "".join(
                f'<span>{S.esc(r["username"])}</span>' if r["access_code"] == code else
                f'<a href="/me/import?code={urllib.parse.quote(r["access_code"])}">{S.esc(r["username"])}</a>'
                for r in linked) + "</div>"
        mb = self.upload_max // (1024 * 1024)
        body = f"""<h1>匯入成績（scores.db）</h1>
<p class="small"><a href="/me?a={urllib.parse.quote(code)}">← 我的頁面</a></p>{pick}
<div class="card"><h2 style="margin-top:0">匯入到「{S.esc(acct["username"])}」</h2>
<p class="small">選遊戲資料夾裡的 <code>scores.db</code>（Z-DON 或 YataiDON 都可以，最大 {mb} MB）。上傳後會列出檔案裡的玩家讓你選，
再把那位玩家每個譜面最好的成績（先比王冠、再比分數）加進這個帳號；這裡已經有更好的成績就略過，不會蓋掉。
匯入的成績沒有重播。檔案只會被讀取，匯入完或一小時後刪除。</p>
<p class="small muted">遊戲要先關掉再上傳，檔案才是最新的。</p>
<form method="post" action="/me/import/upload" enctype="multipart/form-data">
<input type="hidden" name="csrf" value="{self._csrf(req.sess)}"><input type="hidden" name="code" value="{S.esc(code)}">
<input type="file" name="db" accept=".db,application/vnd.sqlite3,application/x-sqlite3" required>
<button class="btn small">上傳</button></form></div>"""
        self.page(req, "匯入成績", body, status=status, flash=flash, err=err)

    def import_upload(self, req) -> None:
        """POST /me/import/upload: reads the multipart body itself (the site's form() takes small forms only)."""
        try:
            length = int(req.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        too_big = length > self.upload_max + 64 * 1024
        if length < 0 or too_big:
            req.close_connection = True  # the body stays unread
        body = req.rfile.read(length) if length > 0 and not too_big else b""
        parts = parse_multipart(req.headers.get("Content-Type", ""), body) or {}
        if too_big or len(parts.get("db", (None, b""))[1]) > self.upload_max:
            self.page(req, "檔案太大", f"<h1>檔案太大</h1><p>最大 {self.upload_max // (1024 * 1024)} MB。"
                      "<a href='/me/import'>回上一頁</a></p>", 413)
            return
        form = {k: v[1].decode("utf-8", "replace") for k, v in parts.items() if v[0] is None}
        if not self._post_ok(req, form):
            self.page(req, "無效的請求", "<h1>無效的請求</h1><p>請重新整理頁面再試一次。</p>", 403)
            return
        uid = req.sess["uid"]
        code = form.get("code", "")
        q = {"code": code}
        if not self._owns(uid, code):
            self.import_page(req, q, "找不到這個遊戲帳號。", True)
            return
        fn, data = parts.get("db", (None, b""))
        if fn is None or not data:
            self.import_page(req, q, "請選一個檔案。", True)
            return
        if not data.startswith(SQLITE_MAGIC):
            self.import_page(req, q, "這不是 SQLite 資料庫檔；請選遊戲資料夾裡的 scores.db。", True)
            return
        now = int(time.time())
        with self.s.lock:
            n = self.s.one("SELECT COUNT(*) AS n FROM score_uploads WHERE forge_uid=? AND at>?", (uid, now - 3600))["n"]
            if n >= UPLOADS_PER_HOUR:
                self.import_page(req, q, "上傳太多次，請一小時後再試。", True)
                return
            self.s.run("INSERT INTO score_uploads VALUES (?,?)", (uid, now))
        self._sweep_uploads()
        token = secrets.token_urlsafe(18)
        path = self._upload_path(uid, token)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        del data, body, parts
        try:
            con = open_scores_db(path)
            try:
                players = file_players(con)
            finally:
                con.close()
        except (BadFile, sqlite3.DatabaseError) as e:
            path.unlink(missing_ok=True)
            self.import_page(req, q, str(e) if isinstance(e, BadFile) else "無法讀取這個資料庫（檔案可能損壞）。", True)
            return
        if not players:
            path.unlink(missing_ok=True)
            self.import_page(req, q, "檔案裡沒有任何成績。", True)
            return
        default = next((p for p in players if p["id"] == code), players[0])
        acct = self._import_account(req, code)
        mine = ' <span class="small muted">（這個帳號）</span>'
        rows = "".join(
            f'<tr><td><label><input type="radio" name="player_id" value="{S.esc(p["id"])}"'
            f'{" checked" if p is default else ""}> {S.esc(p["name"] or "（沒有名字）")}</label>'
            f'{mine if p["id"] == code else ""}</td>'
            f'<td class="small muted">{S.esc(p["id"][:4] + "…" + p["id"][-4:] if len(p["id"]) > 10 else p["id"])}</td>'
            f'<td class="num">{p["charts"]:,}</td><td class="num">{p["plays"]:,}</td>'
            f'<td class="small muted">{S.when(p["last"]) if p["last"] else "—"}</td></tr>' for p in players)
        body = f"""<h1>匯入成績：選玩家</h1>
<p class="small"><a href="/me/import?code={urllib.parse.quote(code)}">← 重新選檔案</a></p>
<div class="card"><p class="small">檔案 <code>{S.esc((fn or "")[:80])}</code> 裡有這些玩家。選要匯入到「{S.esc(acct["username"])}」的那位：</p>
<form method="post" action="/me/import/run"><input type="hidden" name="csrf" value="{self._csrf(req.sess)}">
<input type="hidden" name="code" value="{S.esc(code)}"><input type="hidden" name="token" value="{S.esc(token)}">
<table><tr><th>玩家</th><th>ID</th><th class="num">譜面</th><th class="num">遊玩</th><th>最後遊玩</th></tr>{rows}</table>
<p><button class="btn small">匯入</button></p></form></div>"""
        self.page(req, "匯入成績", body)

    def import_run(self, req, form) -> None:
        uid = req.sess["uid"]
        code, token, player = form.get("code", ""), form.get("token", ""), form.get("player_id", "")
        q = {"code": code}
        if not self._owns(uid, code):
            self.import_page(req, q, "找不到這個遊戲帳號。", True)
            return
        path = self._upload_path(uid, token) if TOKEN.match(token) else None
        if path is None or not path.exists() or time.time() - path.stat().st_mtime > UPLOAD_KEEP:
            self.import_page(req, q, "上傳的檔案已經過期，請重新上傳。", True)
            return
        try:
            con = open_scores_db(path)
            try:
                if player not in {p["id"] for p in file_players(con)}:
                    self.import_page(req, q, "請選檔案裡的一位玩家。", True)
                    return
                res = import_player(self.s, con, player, code)
            finally:
                con.close()
        except (BadFile, sqlite3.DatabaseError) as e:
            self.import_page(req, q, str(e) if isinstance(e, BadFile) else "無法讀取這個資料庫（檔案可能損壞）。", True)
            return
        finally:
            if path.exists():
                path.unlink(missing_ok=True)
        added = res["added"]
        self.s.run("""INSERT INTO score_imports(forge_uid, access_code, player_id, added, not_better, invalid, at)
                      VALUES (?,?,?,?,?,?,?)""", (uid, code, player, len(added), res["not_better"], res["invalid"],
                                                  int(time.time())))
        if added:
            self.invalidate_board()
        shown = added[-200:][::-1]
        trs = "".join(
            f'<tr><td><a href="/song/{S.esc(p["hash"])}">{S.esc(self.song_name(p["hash"]))}</a></td>'
            f'<td>{S.course_badge(p["difficulty"])}</td><td class="num">{p["score"]:,}</td><td>{S.crown_badge(p["crown"])}</td>'
            f'<td class="small muted num">{format(p["before"][1], ",") if p["before"] else "（新）"}</td></tr>' for p in shown)
        table = (f'<table><tr><th>曲目</th><th>難度</th><th class="num">分數</th><th></th><th class="num">原本最佳</th></tr>'
                 f'{trs}</table>' + (f'<p class="small muted">只列出最近的 200 筆。</p>' if len(added) > 200 else "")
                 ) if added else "<p class='muted'>沒有比這裡更好的成績，什麼都沒有加入。</p>"
        qc = urllib.parse.quote(code)
        body = f"""<h1>匯入完成</h1>
<p class="small"><a href="/me?a={qc}">← 我的頁面</a>　<a href="/me/scores?code={qc}">遊玩紀錄</a></p>
<div class="card"><div class="kv"><div>匯入<b>{len(added):,}</b></div><div>略過（這裡已有更好或相同的）<b>{res["not_better"]:,}</b></div>
<div>略過（資料不完整）<b>{res["invalid"]:,}</b></div><div>檔案裡的遊玩<b>{res["plays"]:,}</b></div></div>
<p class="small muted">每個譜面只取檔案裡最好的一次（{res["charts"]:,} 個譜面）。上傳的檔案已刪除。</p></div>
<div class="card">{table}</div>"""
        self.page(req, "匯入完成", body)
