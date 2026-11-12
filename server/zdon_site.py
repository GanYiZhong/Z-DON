"""zdon.taiko.ac: the public website, served by zdon_server.py beside the game API.

Members are TaikoForge's (forge.taiko.ac): "log in" sends the browser to <sso_url>?state=..., TaikoForge
sends it back to /auth/callback with a ticket signed with the shared sso_secret (see taikoforge/sso.py).
This site keeps only the member's TaikoForge id, the email to show them, and which game accounts
(access codes) they linked.

Pages: home, download (update channels), skins, charts and their rankings, player profiles, my page
(link a game account, rename it, send a song to the game).

The web version of the game (/play, served by zdon_server.py) logs in the same way: /auth/login?next=/play/
returns there, then the page asks /auth/game-account for the member's game accounts and may create one with
POST /auth/game-account/new; it plays as the account the player picks.
"""
from __future__ import annotations

import base64
import datetime as dt
import hashlib
import hmac
import html
import json
import re
import secrets
import time
import urllib.parse

from zdon_credit import Credit
from http.cookies import SimpleCookie
from pathlib import Path

COURSES = ["簡單", "普通", "困難", "魔王", "裏魔王"]
CROWNS = {1: "通關", 2: "全連", 3: "全良"}
CROWN_CLASS = {1: "clear", 2: "fc", 3: "dfc"}
SESSION_DAYS = 30

SITE_SCHEMA = """
CREATE TABLE IF NOT EXISTS members (
    forge_uid TEXT PRIMARY KEY, email TEXT, created_at INTEGER NOT NULL, last_login INTEGER
);
CREATE TABLE IF NOT EXISTS member_links (
    access_code TEXT PRIMARY KEY, forge_uid TEXT NOT NULL, linked_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS member_links_by_member ON member_links(forge_uid);
CREATE TABLE IF NOT EXISTS songs (
    hash TEXT PRIMARY KEY, title TEXT, subtitle TEXT, updated_at INTEGER
);
CREATE TABLE IF NOT EXISTS link_attempts (
    forge_uid TEXT NOT NULL, at INTEGER NOT NULL
);
"""

CSS = """
:root{--ink:#2b2024;--muted:#7a6d70;--line:#eadfd6;--bg:#fffaf3;--card:#fff;--red:#e8443a;--blue:#3b8ede;
--gold:#d9a400;--rainbow:linear-gradient(90deg,#e8443a,#f0a020,#3fae5a,#3b8ede,#9b59d0)}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,"Noto Sans TC","Noto Sans JP",sans-serif;
color:var(--ink);background:var(--bg);line-height:1.6}
a{color:var(--blue);text-decoration:none}a:hover{text-decoration:underline}
header{background:var(--ink);color:#fff}header .in{display:flex;gap:1.4em;align-items:center;flex-wrap:wrap}
header a{color:#fff}.logo{font-weight:800;font-size:1.3em;letter-spacing:.02em}.logo span{color:var(--red)}
.in{max-width:1040px;margin:0 auto;padding:.8em 1.2em}nav{display:flex;gap:1.1em;flex:1}
.who{font-size:.9em;opacity:.9}main .in{padding-top:1.6em;padding-bottom:3em}
h1{font-size:1.7em;margin:.2em 0 .6em}h2{font-size:1.25em;margin:1.8em 0 .6em}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1.1em 1.3em;margin:1em 0}
.hero{background:linear-gradient(135deg,#fff 0%,#fff1e6 100%)}
.btn{display:inline-block;background:var(--red);color:#fff;border:0;border-radius:8px;padding:.55em 1.2em;
font-size:1em;cursor:pointer;font-weight:600}.btn:hover{text-decoration:none;filter:brightness(1.08)}
.btn.sec{background:#fff;color:var(--ink);border:1px solid var(--line)}.btn.small{padding:.25em .7em;font-size:.88em}
table{border-collapse:collapse;width:100%}td,th{padding:.45em .6em;border-bottom:1px solid var(--line);text-align:left}
td.small{white-space:nowrap}th{font-size:.85em;color:var(--muted);font-weight:600}td.num,th.num{text-align:right;font-variant-numeric:tabular-nums}
.muted{color:var(--muted)}.small{font-size:.88em}code{background:#f5ede4;padding:.1em .35em;border-radius:4px}
pre{background:#f5ede4;padding:.8em 1em;border-radius:8px;overflow:auto}
.crown{font-size:.8em;padding:.05em .45em;border-radius:4px;color:#fff;white-space:nowrap}
.crown.clear{background:#b9a46a}.crown.fc{background:var(--gold)}.crown.dfc{background:var(--rainbow)}
.course{font-size:.8em;padding:.05em .45em;border-radius:4px;background:#eee;white-space:nowrap}
.c0{background:#ffcf3f}.c1{background:#9fd36b}.c2{background:#7ac4e8}.c3{background:#ef7aa0;color:#fff}
.c4{background:#9b6fd0;color:#fff}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:1em}
input[type=text]{padding:.45em .6em;border:1px solid var(--line);border-radius:6px;font-size:1em;width:16em;max-width:100%}
.flash{background:#e9f6ec;border:1px solid #bfe3c8;border-radius:8px;padding:.6em 1em}
.flash.err{background:#fdeceb;border-color:#f3c3bf}footer{color:var(--muted);font-size:.85em;text-align:center;padding:2em}
"""


def esc(v) -> str:
    return html.escape("" if v is None else str(v))


def b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def unb64(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def when(ts) -> str:
    if not ts:
        return "-"
    return dt.datetime.fromtimestamp(int(ts)).strftime("%Y-%m-%d %H:%M")


def course_badge(d) -> str:
    d = int(d or 0)
    return f'<span class="course c{d}">{COURSES[d] if 0 <= d < len(COURSES) else d}</span>'


def crown_badge(c) -> str:
    c = int(c or 0)
    return f'<span class="crown {CROWN_CLASS[c]}">{CROWNS[c]}</span>' if c in CROWNS else ""


class Site:
    # "/me" only as a whole segment: the game's API has /medals
    PREFIXES = ("/download", "/skins", "/charts", "/chart/", "/player/", "/me/", "/auth/", "/admin/", "/site.css")

    def __init__(self, cfg: dict, store, nonces):
        self.cfg = cfg
        self.s = store
        self.nonces = nonces
        with store.lock:
            store.db.executescript(SITE_SCHEMA)
        site = cfg.get("site", {})
        self.public_url = str(site.get("url", "")).rstrip("/")
        self.sso_url = str(site.get("sso_url", ""))
        self.sso_secret = str(site.get("sso_secret", ""))
        self.forge_url = str(site.get("forge_url", "https://forge.taiko.ac")).rstrip("/")
        self.name = str(site.get("name", "Z-DON"))
        # the TaikoForge accounts (uids) that review reports on /admin
        self.admins = {str(a) for a in site.get("admins", [])}
        self.credit = Credit(store)
        self.secure = self.public_url.startswith("https://")
        key_file = Path(cfg["data_dir"]) / "site_secret.key"
        secret = str(site.get("session_secret", ""))
        if not secret:
            if key_file.exists():
                secret = key_file.read_text(encoding="utf-8").strip()
            else:
                key_file.parent.mkdir(parents=True, exist_ok=True)
                secret = secrets.token_hex(32)
                key_file.write_text(secret, encoding="utf-8")
        self.session_key = secret.encode()

    def handles(self, path: str) -> bool:
        return path in ("/", "/me", "/admin") or path.startswith(self.PREFIXES)

    # ------------------------------------------------------------------------------------------------------------
    # sessions (a signed cookie; the server keeps no session table)

    def _sign(self, data: str) -> str:
        return hmac.new(self.session_key, data.encode(), hashlib.sha256).hexdigest()

    def _cookie(self, name: str, value: str, max_age: int) -> str:
        parts = [f"{name}={value}", "Path=/", "HttpOnly", "SameSite=Lax", f"Max-Age={max_age}"]
        if self.secure:
            parts.append("Secure")
        return "; ".join(parts)

    def _session(self, req) -> dict | None:
        raw = req.cookies.get("zdon_session")
        if not raw or "." not in raw:
            return None
        payload, sig = raw.rsplit(".", 1)
        if not hmac.compare_digest(sig, self._sign(payload)):
            return None
        try:
            data = json.loads(unb64(payload))
        except ValueError:
            return None
        if data.get("exp", 0) < time.time():
            return None
        return data

    def _csrf(self, sess: dict | None) -> str:
        return self._sign("csrf:" + (sess or {}).get("uid", ""))[:32]

    # ------------------------------------------------------------------------------------------------------------
    # page frame

    def page(self, req, title: str, body: str, status: int = 200, flash: str = "", err: bool = False):
        sess = req.sess
        if sess:
            who = (f'<span class="who">{esc(sess.get("email"))}</span> <a href="/me">我的頁面</a> '
                   f'<form method="post" action="/auth/logout" style="display:inline">'
                   f'<input type="hidden" name="csrf" value="{self._csrf(sess)}">'
                   f'<button class="btn small sec">登出</button></form>')
        elif self.sso_url and self.sso_secret:
            who = '<a class="btn small" href="/auth/login">登入</a>'
        else:
            who = ""
        note = f'<p class="flash{" err" if err else ""}">{esc(flash)}</p>' if flash else ""
        doc = f"""<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>{esc(title)} - {esc(self.name)}</title>
<link rel="stylesheet" href="/site.css"></head><body>
<header><div class="in"><a class="logo" href="/">Z-<span>DON</span></a><nav><a href="/download">下載</a>
<a href="/charts">排行</a><a href="/skins">Skin</a></nav><div>{who}</div></div></header>
<main><div class="in">{note}{body}</div></main>
<footer>Z-DON 源自 <a href="https://github.com/Yonokid/YataiDON">YataiDON</a>（GPL-3.0）・會員由
<a href="{esc(self.forge_url)}">TaikoForge</a> 提供</footer></body></html>"""
        req.send_html(status, doc)

    # ------------------------------------------------------------------------------------------------------------
    # dispatch

    def handle(self, req, method: str, path: str, query: dict[str, str]) -> None:
        req.sess = self._session(req)
        if path == "/site.css":
            req.send(200, CSS.encode(), "text/css; charset=utf-8", {"Cache-Control": "max-age=3600"})
            return
        if method == "POST":
            form = req.form()
            if not self._post_ok(req, form):
                self.page(req, "無效的請求", "<h1>無效的請求</h1><p>請重新整理頁面再試一次。</p>", 403)
                return
            routes = {"/auth/logout": self.logout, "/auth/game-account/new": self.new_game_account,
                      "/me/link": self.link, "/me/unlink": self.unlink,
                      "/me/rename": self.rename, "/me/jump": self.jump, "/me/challenge": self.challenge,
                      "/admin/resolve": self.admin_resolve}
            fn = routes.get(path)
            if fn is None:
                self.page(req, "找不到", "<h1>找不到這一頁</h1>", 404)
                return
            fn(req, form)
            return
        if path == "/":
            self.home(req)
        elif path == "/download":
            self.download(req)
        elif path.startswith("/download/") and path.endswith(".zip"):
            self.download_zip(req, path[len("/download/"):-4])
        elif path == "/skins":
            self.skins(req)
        elif path.startswith("/skins/") and path.endswith(".zip"):
            self.skin_zip(req, path[len("/skins/"):-4])
        elif path == "/charts":
            self.charts(req, query)
        elif path.startswith("/chart/"):
            self.chart(req, path[len("/chart/"):], query)
        elif path.startswith("/player/"):
            self.player(req, path[len("/player/"):])
        elif path == "/me":
            self.me(req, query)
        elif path == "/admin":
            self.admin(req)
        elif path == "/auth/login":
            self.login(req, query)
        elif path == "/auth/game-account":
            self.game_account(req)
        elif path == "/auth/callback":
            self.callback(req, query)
        else:
            self.page(req, "找不到", "<h1>找不到這一頁</h1>", 404)

    # ------------------------------------------------------------------------------------------------------------
    # admin: online battle reports (zdon_credit.py)

    def _is_admin(self, req) -> bool:
        return bool(req.sess and req.sess.get("uid") in self.admins)

    def _name(self, code: str) -> str:
        u = self.s.one("SELECT username FROM users WHERE access_code=?", (code,))
        return u["username"] if u else "(unknown)"

    def admin(self, req, flash: str = "", err: bool = False):
        if not self._is_admin(req):
            self.page(req, "找不到", "<h1>找不到這一頁</h1>", 404)
            return
        csrf = self._csrf(req.sess)
        rows = []
        for r in self.credit.pending():
            try:
                ctx = json.loads(r["context"] or "{}")
            except ValueError:
                ctx = {}
            chat = "".join(f'<div class="small"><b>{esc(m.get("from", ""))}</b>：{esc(m.get("text", ""))}</div>'
                           for m in ctx.get("chat", [])) or '<div class="small muted">（沒有聊天紀錄）</div>'
            when = dt.datetime.fromtimestamp(r["created"]).strftime("%Y-%m-%d %H:%M")
            buttons = "".join(
                f'<form method="post" action="/admin/resolve" style="display:inline">'
                f'<input type="hidden" name="csrf" value="{csrf}"><input type="hidden" name="id" value="{r["id"]}">'
                f'<input type="hidden" name="verdict" value="{v}"><button class="btn small{cls}">{label}</button></form> '
                for v, label, cls in (("accepted", f"成立（{esc(self._name(r['reported']))} -5）", ""),
                                      ("rejected", "不成立", " sec"),
                                      ("malicious", f"惡意檢舉（{esc(self._name(r['reporter']))} -5）", " sec")))
            rows.append(f"""<div class="card"><p><b>#{r["id"]}</b>　{esc(self._name(r["reporter"]))} 檢舉
<b>{esc(self._name(r["reported"]))}</b>　<span class="muted small">{when}・房間 {esc(ctx.get("room", "-"))}</span></p>
<p>事由：{esc(r["reason"])}</p><details><summary class="small">當時的聊天紀錄</summary>{chat}</details>
<p>{buttons}</p></div>""")
        done = []
        for r in self.credit.recent(15):
            verdict = {"accepted": "成立", "rejected": "不成立", "malicious": "惡意檢舉"}.get(r["status"], r["status"])
            done.append(f'<li class="small">#{r["id"]} {esc(self._name(r["reporter"]))} → '
                        f'{esc(self._name(r["reported"]))}：{esc(r["reason"])}（{verdict}）</li>')
        body = (f'<h1>檢舉審核</h1><p class="muted small">成立：被檢舉的人信用分 -5；惡意檢舉：檢舉的人 -5。</p>'
                + ("".join(rows) or '<p class="muted">沒有待審的檢舉。</p>')
                + (f'<h2>最近處理的</h2><ul>{"".join(done)}</ul>' if done else ""))
        self.page(req, "檢舉審核", body, flash=flash, err=err)

    def admin_resolve(self, req, form):
        if not self._is_admin(req):
            self.page(req, "找不到", "<h1>找不到這一頁</h1>", 404)
            return
        try:
            rid = int(form.get("id", "0"))
        except ValueError:
            rid = 0
        ok = self.credit.resolve(rid, form.get("verdict", ""), str(req.sess.get("uid")))
        self.admin(req, f"#{rid} 已處理" if ok else f"#{rid} 無法處理（可能已處理過）", not ok)

    def _post_ok(self, req, form) -> bool:
        if not req.sess or not hmac.compare_digest(form.get("csrf", ""), self._csrf(req.sess)):
            return False
        origin = req.headers.get("Origin")
        if origin and self.public_url and origin.rstrip("/") != self.public_url:
            return False
        return True

    # ------------------------------------------------------------------------------------------------------------
    # login (TaikoForge single sign-on)

    @staticmethod
    def _local_next(nxt: str) -> str:
        # only a path on this site (no //host or scheme): where a login returns to
        return nxt if nxt.startswith("/") and not nxt.startswith("//") and "\\" not in nxt and len(nxt) < 200 else ""

    def login(self, req, query: dict[str, str] | None = None):
        if not (self.sso_url and self.sso_secret):
            self.page(req, "登入", "<h1>登入尚未設定</h1><p class='muted'>站長還沒設定 [site] sso_url / sso_secret。</p>",
                      503)
            return
        state = secrets.token_urlsafe(24)
        sep = "&" if "?" in self.sso_url else "?"
        cookies = [self._cookie("zdon_sso_state", state, 600)]
        nxt = self._local_next((query or {}).get("next", ""))
        if nxt:
            cookies.append(self._cookie("zdon_sso_next", urllib.parse.quote(nxt, safe="/"), 600))
        req.redirect(self.sso_url + sep + urllib.parse.urlencode({"state": state}), cookies)

    def callback(self, req, query):
        ticket = query.get("ticket", "")
        state = req.cookies.get("zdon_sso_state", "")
        claims = self.verify_ticket(ticket, state)
        clear_state = self._cookie("zdon_sso_state", "", 0)
        if claims is None:
            req.headers_out = [clear_state]
            self.page(req, "登入失敗", "<h1>登入失敗</h1><p>登入連結過期或無效，請<a href='/auth/login'>再試一次</a>。</p>",
                      400)
            return
        uid, email, now = claims["sub"], claims.get("email", ""), int(time.time())
        with self.s.lock:
            self.s.run("""INSERT INTO members(forge_uid, email, created_at, last_login) VALUES (?,?,?,?)
                          ON CONFLICT(forge_uid) DO UPDATE SET email=excluded.email, last_login=excluded.last_login""",
                       (uid, email, now, now))
        payload = b64(json.dumps({"uid": uid, "email": email, "exp": now + SESSION_DAYS * 86400}).encode())
        nxt = self._local_next(urllib.parse.unquote(req.cookies.get("zdon_sso_next", "")))
        req.redirect(nxt or "/me", [clear_state, self._cookie("zdon_sso_next", "", 0),
                                    self._cookie("zdon_session", f"{payload}.{self._sign(payload)}",
                                                 SESSION_DAYS * 86400)])

    def verify_ticket(self, ticket: str, state: str) -> dict | None:
        if not ticket or "." not in ticket or not state or not self.sso_secret:
            return None
        payload, sig = ticket.rsplit(".", 1)
        want = hmac.new(self.sso_secret.encode(), payload.encode(), hashlib.sha256).hexdigest()
        if not hmac.compare_digest(want, sig):
            return None
        try:
            c = json.loads(unb64(payload))
        except ValueError:
            return None
        now = time.time()
        if (c.get("aud") != "zdon" or not hmac.compare_digest(str(c.get("state", "")), state)
                or not c.get("sub") or not (c.get("iat", 0) - 30 <= now <= c.get("exp", 0))):
            return None
        if not self.nonces.fresh("sso:" + str(c.get("nonce", ""))):
            return None
        return c

    # ------------------------------------------------------------------------------------------------------------
    # the web game (/play): the member's game accounts

    def _json(self, req, status: int, obj) -> None:
        req.send(status, json.dumps(obj, ensure_ascii=False).encode(), "application/json; charset=utf-8",
                 {"Cache-Control": "no-store"})

    def game_account(self, req):
        if not req.sess:
            self._json(req, 200, {"logged_in": False, "login_url": "/auth/login?next=/play/"})
            return
        accounts = [{"access_code": r["access_code"], "username": r["username"]} for r in self._linked(req.sess["uid"])]
        self._json(req, 200, {"logged_in": True, "email": req.sess.get("email", ""), "accounts": accounts,
                              "csrf": self._csrf(req.sess)})

    def new_game_account(self, req, form):
        uid = req.sess["uid"]
        name = (form.get("name") or "").strip()[:32] or "Player"
        now = int(time.time())
        with self.s.lock:
            if len(self._linked(uid)) >= 10:
                self._json(req, 400, {"detail": "too many game accounts"})
                return
            while True:
                code = str(secrets.randbelow(9 * 10**19) + 10**19)  # 20 digits, as zdon_server.new_access_code
                if not self.s.one("SELECT 1 FROM users WHERE access_code=?", (code,)):
                    break
            self.s.run("INSERT INTO users(access_code, username, idm, created_at) VALUES (?,?,?,?)",
                       (code, name, None, now))
            self.s.run("INSERT OR IGNORE INTO member_links VALUES (?,?,?)", (code, uid, now))
        self._json(req, 200, {"access_code": code, "username": name})

    def logout(self, req, form):
        req.redirect("/", [self._cookie("zdon_session", "", 0)])

    # ------------------------------------------------------------------------------------------------------------
    # data helpers

    def song_name(self, h: str) -> str:
        row = self.s.one("SELECT title, subtitle FROM songs WHERE hash=?", (h,))
        if row and row["title"]:
            return row["title"]
        return h[:12] + "…"

    def channels(self) -> list[tuple[str, str, Path]]:
        root = Path(self.cfg["data_dir"]) / "update"
        out = []
        if root.is_dir():
            for ch in sorted(root.iterdir()):
                v = ch / "version.txt"
                if ch.is_dir() and v.is_file() and not ch.name.endswith((".new", ".old")):
                    out.append((ch.name, v.read_text(encoding="utf-8").strip(), ch))
        return out

    def published_skins(self) -> list[tuple[str, str, Path]]:
        root = Path(self.cfg["data_dir"]) / "skins"
        out = []
        if root.is_dir():
            for sk in sorted(root.glob("*/*")):
                if sk.is_dir() and (sk / "checksums.sha256").is_file() and not sk.name.endswith((".new", ".old")):
                    out.append((sk.parent.name, sk.name, sk))
        return out

    def _plays_table(self, rows, show_player=True) -> str:
        if not rows:
            return '<p class="muted">還沒有成績。</p>'
        out = ['<table><tr><th>時間</th>' + ('<th>玩家</th>' if show_player else '') +
               '<th>曲目</th><th>難度</th><th class="num">分數</th><th></th></tr>']
        for r in rows:
            out.append(f'<tr><td class="small muted">{when(r["received_at"])}</td>' +
                       (f'<td><a href="/player/{r["pid"]}">{esc(r["username"])}</a></td>' if show_player else '') +
                       f'<td><a href="/chart/{esc(r["hash"])}?d={r["difficulty"]}">{esc(self.song_name(r["hash"]))}</a>'
                       f'</td><td>{course_badge(r["difficulty"])}</td><td class="num">{r["score"]:,}</td>'
                       f'<td>{crown_badge(r["crown"])}</td></tr>')
        out.append("</table>")
        return "".join(out)

    # ------------------------------------------------------------------------------------------------------------
    # pages

    def home(self, req):
        ch = self.channels()
        latest = ch[0] if ch else None
        dl = (f'<p><a class="btn" href="/download">下載 Z-DON</a> <span class="muted small">{esc(self.version_label(latest[1], latest[2]))} 版</span></p>'
              if latest else '<p class="muted">還沒有發布的版本。</p>')
        recent = self.s.q("""SELECT s.received_at, s.hash, s.difficulty, s.score, s.crown, u.username, u.rowid AS pid
                             FROM scores s JOIN users u USING(access_code) ORDER BY s.id DESC LIMIT 12""")
        stats = self.s.one("SELECT (SELECT COUNT(*) FROM users) AS players, (SELECT COUNT(*) FROM scores) AS plays")
        hot = self.s.q("""SELECT hash, difficulty, COUNT(*) AS n FROM scores
                          WHERE received_at > ? GROUP BY hash, difficulty ORDER BY n DESC LIMIT 8""",
                       (int(time.time()) - 30 * 86400,))
        hot_html = "".join(f'<tr><td><a href="/chart/{esc(r["hash"])}?d={r["difficulty"]}">'
                           f'{esc(self.song_name(r["hash"]))}</a></td><td>{course_badge(r["difficulty"])}</td>'
                           f'<td class="num">{r["n"]}</td></tr>' for r in hot)
        body = f"""<div class="card hero"><h1>Z-DON</h1>
<p>以 YataiDON 為基礎、追求極致效能與新功能的太鼓模擬器。成績與排行在這裡，帳號跟 TaikoForge 共用。</p>{dl}
<p class="small muted">{stats["players"]} 位玩家・{stats["plays"]} 筆成績</p></div>
<div class="grid"><div class="card"><h2 style="margin-top:0">最近遊玩</h2>{self._plays_table(recent)}</div>
<div class="card"><h2 style="margin-top:0">本月熱門</h2>{('<table><tr><th>曲目</th><th>難度</th><th class="num">遊玩</th></tr>' + hot_html + '</table>') if hot else '<p class="muted">還沒有成績。</p>'}</div></div>"""
        self.page(req, "首頁", body)

    @staticmethod
    def version_label(version: str, path: Path) -> str:
        """20261110-2113-874ef6ef -> 2026/11/10 21:13; anything else as it is."""
        m = re.match(r"^(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})", version)
        if m:
            return f"{m[1]}/{m[2]}/{m[3]} {m[4]}:{m[5]}"
        return version or when(path.stat().st_mtime)

    def download(self, req):
        ch = [c for c in self.channels() if (c[2] / "package.zip").is_file()]
        if not ch:
            self.page(req, "下載", '<h1>下載</h1><p class="muted">還沒有可以下載的版本。</p>')
            return
        # the main channel first ("windows"), the rest as small links
        ch.sort(key=lambda c: (c[0] != "windows", c[0]))
        name, version, path = ch[0]
        size = (path / "package.zip").stat().st_size / 1e6
        # the version in the link: a cache in between never hands out an older build under the same address
        others = "".join(f' <a href="/download/{esc(n)}.zip?v={esc(urllib.parse.quote(v))}">{esc(n)}</a>'
                         for n, v, _ in ch[1:])
        body = f"""<div class="card hero" style="text-align:center;padding:2.2em 1.3em">
<h1 style="margin-bottom:.3em">下載 Z-DON</h1>
<p><a class="btn" style="font-size:1.25em;padding:.7em 2.2em" href="/download/{esc(name)}.zip?v={esc(urllib.parse.quote(version))}">下載（Windows）</a></p>
<p class="small muted">{esc(self.version_label(version, path))} 版・{size:.0f} MB</p></div>
<div class="card"><ol style="margin:.2em 0;padding-left:1.4em;line-height:2">
<li>把下載的 zip 解壓縮到任何資料夾</li>
<li>執行 <b>YataiDON.exe</b></li>
<li>第一次開啟時選要用的 skin，會自動下載（約 3～5 分鐘）</li>
</ol></div>
<p class="small muted" style="text-align:center">之後有新版時，遊戲標題畫面會提示，按 <b>F9</b> 就會自動更新。{('<br>其他版本：' + others) if others else ''}</p>"""
        self.page(req, "下載", body)

    def download_zip(self, req, channel: str):
        for name, version, path in self.channels():
            if name == channel and (path / "package.zip").is_file():
                req.send_file(path / "package.zip", f"Z-DON-{name}-{version}.zip")
                return
        self.page(req, "找不到", "<h1>找不到這個安裝包</h1>", 404)

    def skins(self, req):
        host = self.public_url or f"http://{req.headers.get('Host', 'HOST')}"
        rows = []
        for name, branch, path in self.published_skins():
            zip_file = path.parent / f"{branch}.zip"
            dl = (f'<a class="btn small" href="/skins/{esc(name)}/{esc(branch)}.zip">下載</a> '
                  f'<span class="muted small">{zip_file.stat().st_size / 1e6:.1f} MB</span>') if zip_file.is_file() else ""
            rows.append(f"""<div class="card"><h2 style="margin-top:0">{esc(name)} <span class="muted small">{esc(branch)}</span></h2>
<p>{dl}</p><p class="small">自動更新：解壓到 <code>Skins\\{esc(name)}</code>，在裡面建立 <code>.skin-repo</code>（兩行）並把
<code>config.toml</code> 的 <code>skin_updater</code> 設成 <code>true</code>：</p>
<pre>{esc(host)}/skins/{esc(name)}
{esc(branch)}</pre></div>""")
        body = "<h1>Skin</h1>" + ("".join(rows) if rows else '<p class="muted">還沒有發布的 Skin。</p>')
        self.page(req, "Skin", body)

    def skin_zip(self, req, rest: str):
        name, _, branch = rest.partition("/")
        for n, b, path in self.published_skins():
            if n == name and b == branch and (path.parent / f"{b}.zip").is_file():
                req.send_file(path.parent / f"{b}.zip", f"{n}-{b}.zip")
                return
        self.page(req, "找不到", "<h1>找不到這個 Skin</h1>", 404)

    def charts(self, req, query):
        q = (query.get("q") or "").strip()
        args: list = []
        where = ""
        if q:
            where = "WHERE g.title LIKE ? OR g.subtitle LIKE ?"
            args = [f"%{q}%", f"%{q}%"]
        rows = self.s.q(f"""SELECT s.hash, s.difficulty, COUNT(DISTINCT s.access_code) AS players, COUNT(*) AS plays,
                                   MAX(s.score) AS top, MAX(s.received_at) AS last
                            FROM scores s LEFT JOIN songs g ON g.hash = s.hash {where}
                            GROUP BY s.hash, s.difficulty ORDER BY last DESC LIMIT 200""", args)
        body = [f"""<h1>排行</h1><form method="get"><input type="text" name="q" value="{esc(q)}" placeholder="搜尋曲名">
<button class="btn small sec">搜尋</button></form>"""]
        if not rows:
            body.append('<p class="muted">沒有符合的曲目。</p>')
        else:
            body.append('<div class="card"><table><tr><th>曲目</th><th>難度</th><th class="num">玩家</th>'
                        '<th class="num">遊玩</th><th class="num">最高分</th></tr>')
            for r in rows:
                body.append(f'<tr><td><a href="/chart/{esc(r["hash"])}?d={r["difficulty"]}">'
                            f'{esc(self.song_name(r["hash"]))}</a></td><td>{course_badge(r["difficulty"])}</td>'
                            f'<td class="num">{r["players"]}</td><td class="num">{r["plays"]}</td>'
                            f'<td class="num">{r["top"]:,}</td></tr>')
            body.append("</table></div>")
        self.page(req, "排行", "".join(body))

    def chart(self, req, h: str, query):
        h = urllib.parse.unquote(h)
        diffs = [r["difficulty"] for r in self.s.q(
            "SELECT DISTINCT difficulty FROM scores WHERE hash=? ORDER BY difficulty", (h,))]
        if not diffs:
            self.page(req, "找不到", "<h1>這首歌還沒有成績</h1>", 404)
            return
        d = int(query.get("d")) if (query.get("d") or "").lstrip("-").isdigit() else diffs[-1]
        if d not in diffs:
            d = diffs[-1]
        song = self.s.one("SELECT title, subtitle FROM songs WHERE hash=?", (h,))
        title = song["title"] if song and song["title"] else h[:12] + "…"
        sub = f'<p class="muted">{esc(song["subtitle"])}</p>' if song and song["subtitle"] else ""
        tabs = " ".join(f'<a href="?d={x}">{course_badge(x)}</a>' if x != d else course_badge(x) for x in diffs)
        rows = self.s.q("""SELECT u.username, u.rowid AS pid, MAX(s.score) AS score, MAX(s.crown) AS crown,
                                  MAX(s.received_at) AS last, COUNT(*) AS plays,
                                  (SELECT s2.id FROM scores s2 WHERE s2.access_code = s.access_code AND s2.hash = ?
                                   AND s2.difficulty = ? AND length(s2.input_log) > 2
                                   ORDER BY s2.score DESC, s2.id LIMIT 1) AS cid
                           FROM scores s JOIN users u USING(access_code) WHERE s.hash=? AND s.difficulty=?
                           GROUP BY s.access_code ORDER BY score DESC LIMIT 100""", (h, d, h, d))
        can_challenge = bool(req.sess and self._linked(req.sess["uid"]))
        csrf = self._csrf(req.sess) if req.sess else ""
        table = ['<table><tr><th class="num">#</th><th>玩家</th><th class="num">分數</th><th></th>'
                 '<th class="num">遊玩</th><th>最後</th>' + ('<th></th>' if can_challenge else '') + '</tr>']
        for i, r in enumerate(rows, 1):
            table.append(f'<tr><td class="num">{i}</td><td><a href="/player/{r["pid"]}">{esc(r["username"])}</a></td>'
                         f'<td class="num">{r["score"]:,}</td><td>{crown_badge(r["crown"])}</td>'
                         f'<td class="num">{r["plays"]}</td><td class="small muted">{when(r["last"])}</td>' +
                         ((f'<td><form method="post" action="/me/challenge"><input type="hidden" name="csrf" '
                           f'value="{csrf}"><input type="hidden" name="score_id" value="{r["cid"]}">'
                           f'<button class="btn small sec">挑戰</button></form></td>' if r["cid"] else '<td></td>')
                          if can_challenge else '') + '</tr>')
        table.append("</table>")
        jump = ""
        if req.sess and self._linked(req.sess["uid"]):
            jump = (f'<form method="post" action="/me/jump"><input type="hidden" name="csrf" value="{self._csrf(req.sess)}">'
                    f'<input type="hidden" name="hash" value="{esc(h)}"><button class="btn small sec">在我的遊戲中開啟這首</button>'
                    f'</form>')
        self.page(req, title, f"<h1>{esc(title)}</h1>{sub}<p>{tabs}</p>{jump}<div class='card'>{''.join(table)}</div>")

    def player(self, req, pid: str):
        u = self.s.one("SELECT rowid AS pid, * FROM users WHERE rowid=?", (int(pid),)) if pid.isdigit() else None
        if u is None:
            self.page(req, "找不到", "<h1>找不到這位玩家</h1>", 404)
            return
        stats = self.s.one("""SELECT COUNT(*) AS plays, COUNT(DISTINCT hash || ':' || difficulty) AS charts,
                                     SUM(crown >= 2) AS fcs FROM scores WHERE access_code=?""", (u["access_code"],))
        best = self.s.q("""SELECT hash, difficulty, MAX(score) AS score, MAX(crown) AS crown, MAX(received_at) AS received_at
                           FROM scores WHERE access_code=? GROUP BY hash, difficulty ORDER BY difficulty DESC, score DESC
                           LIMIT 50""", (u["access_code"],))
        recent = self.s.q("""SELECT s.received_at, s.hash, s.difficulty, s.score, s.crown, ? AS username, ? AS pid
                             FROM scores s WHERE access_code=? ORDER BY id DESC LIMIT 15""",
                          (u["username"], u["pid"], u["access_code"]))
        best_rows = "".join(f'<tr><td><a href="/chart/{esc(r["hash"])}?d={r["difficulty"]}">{esc(self.song_name(r["hash"]))}'
                            f'</a></td><td>{course_badge(r["difficulty"])}</td><td class="num">{r["score"]:,}</td>'
                            f'<td>{crown_badge(r["crown"])}</td></tr>' for r in best)
        title = f'<p class="muted">{esc(u["title"])}</p>' if u["title"] else ""
        body = f"""<h1>{esc(u["username"])}</h1>{title}
<p class="small muted">{stats["plays"]} 次遊玩・{stats["charts"]} 個譜面・全連 {stats["fcs"] or 0}</p>
<div class="grid"><div class="card"><h2 style="margin-top:0">最佳成績</h2>
{('<table><tr><th>曲目</th><th>難度</th><th class="num">分數</th><th></th></tr>' + best_rows + '</table>') if best else '<p class="muted">還沒有成績。</p>'}</div>
<div class="card"><h2 style="margin-top:0">最近遊玩</h2>{self._plays_table(recent, show_player=False)}</div></div>"""
        self.page(req, u["username"], body)

    # ------------------------------------------------------------------------------------------------------------
    # my page

    def _linked(self, uid: str):
        return self.s.q("""SELECT l.access_code, u.username, u.rowid AS pid FROM member_links l
                           JOIN users u USING(access_code) WHERE l.forge_uid=? ORDER BY l.linked_at""", (uid,))

    def _owns(self, uid: str, code: str) -> bool:
        return self.s.one("SELECT 1 FROM member_links WHERE forge_uid=? AND access_code=?", (uid, code)) is not None

    def me(self, req, query, flash: str = "", err: bool = False):
        if not req.sess:
            req.redirect("/auth/login")
            return
        csrf = self._csrf(req.sess)
        linked = self._linked(req.sess["uid"])
        parts = [f"<h1>我的頁面</h1><p class='muted small'>TaikoForge 帳號：{esc(req.sess['email'])}"
                 f"（<a href='{esc(self.forge_url)}/account'>管理帳號</a>）</p>"]
        if linked:
            for r in linked:
                masked = r["access_code"][:4] + "…" + r["access_code"][-4:]
                parts.append(f"""<div class="card"><h2 style="margin-top:0"><a href="/player/{r["pid"]}">{esc(r["username"])}</a>
<span class="muted small">遊戲帳號 {esc(masked)}</span></h2>
<form method="post" action="/me/rename" style="display:inline"><input type="hidden" name="csrf" value="{csrf}">
<input type="hidden" name="code" value="{esc(r["access_code"])}"><input type="text" name="name" maxlength="32"
value="{esc(r["username"])}"> <button class="btn small sec">改名</button></form>
<form method="post" action="/me/unlink" style="display:inline;margin-left:1em"><input type="hidden" name="csrf" value="{csrf}">
<input type="hidden" name="code" value="{esc(r["access_code"])}"><button class="btn small sec">解除綁定</button></form>
</div>""")
        else:
            parts.append('<p class="muted">還沒有綁定遊戲帳號。</p>')
        parts.append(f"""<div class="card"><h2 style="margin-top:0">綁定遊戲帳號</h2>
<p class="small">遊戲帳號的 access code 在遊戲資料夾 <code>config.toml</code> 的 <code>[network]</code>
<code>access_code_1</code>（20 位數字）。綁定後可以在這裡改名、從排行頁把歌送到遊戲。</p>
<form method="post" action="/me/link"><input type="hidden" name="csrf" value="{csrf}">
<input type="text" name="code" inputmode="numeric" autocomplete="off" placeholder="access code">
<button class="btn small">綁定</button></form></div>""")
        self.page(req, "我的頁面", "".join(parts), flash=flash, err=err)

    def link(self, req, form):
        uid = req.sess["uid"]
        code = "".join(ch for ch in form.get("code", "") if ch.isalnum())
        now = int(time.time())
        with self.s.lock:
            tries = self.s.one("SELECT COUNT(*) AS n FROM link_attempts WHERE forge_uid=? AND at>?",
                               (uid, now - 3600))["n"]
            if tries >= 10:
                self.me(req, {}, "嘗試太多次，請一小時後再試。", True)
                return
            self.s.run("INSERT INTO link_attempts VALUES (?,?)", (uid, now))
            user = self.s.one("SELECT access_code FROM users WHERE access_code=?", (code,)) if code else None
            if user is None:
                self.me(req, {}, "找不到這個遊戲帳號。", True)
                return
            owner = self.s.one("SELECT forge_uid FROM member_links WHERE access_code=?", (code,))
            if owner and owner["forge_uid"] != uid:
                self.me(req, {}, "這個遊戲帳號已經綁定在別的會員。", True)
                return
            self.s.run("INSERT OR IGNORE INTO member_links VALUES (?,?,?)", (code, uid, now))
        self.me(req, {}, "已綁定。")

    def unlink(self, req, form):
        self.s.run("DELETE FROM member_links WHERE forge_uid=? AND access_code=?", (req.sess["uid"], form.get("code", "")))
        self.me(req, {}, "已解除綁定。")

    def rename(self, req, form):
        code, name = form.get("code", ""), form.get("name", "").strip()[:32]
        if not self._owns(req.sess["uid"], code) or not name:
            self.me(req, {}, "無法改名。", True)
            return
        self.s.run("UPDATE users SET username=? WHERE access_code=?", (name, code))
        self.me(req, {}, "已改名；遊戲下次登入時會套用。")

    def challenge(self, req, form):
        sid = int(form.get("score_id", "0") or 0) if str(form.get("score_id", "")).isdigit() else 0
        linked = self._linked(req.sess["uid"])
        row = self.s.one("SELECT id FROM scores WHERE id=? AND length(input_log) > 2", (sid,)) if sid else None
        if not linked or row is None:
            self.me(req, {}, "無法挑戰這筆紀錄。", True)
            return
        for r in linked:
            self.s.run("INSERT OR REPLACE INTO jumps(access_code, hash, score_id, challenge) VALUES (?,?,?,1)",
                       (r["access_code"], None, sid))
        self.me(req, {}, "已送出挑戰；遊戲在選曲畫面會開始這場對戰。")

    def jump(self, req, form):
        h = form.get("hash", "")
        linked = self._linked(req.sess["uid"])
        if not linked or not h:
            self.me(req, {}, "請先綁定遊戲帳號。", True)
            return
        for r in linked:
            self.s.run("INSERT OR REPLACE INTO jumps(access_code, hash, score_id, challenge) VALUES (?,?,?,0)",
                       (r["access_code"], h, None))
        self.me(req, {}, "已送出；遊戲在選曲畫面會跳到這首歌。")
