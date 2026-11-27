"""TaikoForge login from the desktop game (src/scenes/zdon_login.cpp).

The player types their TaikoForge email and password into the game; the game sends them here (a signed request, over
https in production) and this server asks TaikoForge (POST /partner/<client>/login, signed with the SSO secret) whether
they are right. The password is not kept anywhere. On success the game gets a token (signed here, 180 days) that it
keeps in config.toml instead of the password, and the game account (access code) it plays as: a member's game accounts
are the website's member_links (zdon_site.py), so the same account shows on the website.

    POST /forge_login     email, password, access_code (the game's current 1P code, may be empty / "0"), username
                         -> {token, email, access_code, username, accounts: [{access_code, username}]}
                            401 wrong email or password, 403 {code: unverified|blocked}, 429 {code: locked|rate_limited},
                            503 not set up / TaikoForge unreachable
    POST /forge_session  token -> {email, accounts} (still a member; TaikoForge is asked too when it answers)
                         401 a bad or expired token, 403 the member was blocked
    GET  /forge_songs    token -> {songs: [{id, title, ...}], paid, retention_days}: the member's TaikoForge charts
                         (the same list the web version gets from /forge/songs), 401 a bad or expired token
    GET  /forge_song     token, id (32 hex), file tja|audio -> the chart's TJA or its OGG audio (bytes)
    The server's /health says {"forge_login": "required"} when [site] game_login = "required" (the game then lets
    nobody play before logging in; "optional" shows the login screen with a skip button; "" / absent: no login screen).
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import re
import secrets
import time
import urllib.error
import urllib.parse
import urllib.request

TOKEN_DAYS = 180
SONG_ID = re.compile(r"^[0-9a-f]{32}$")
MAX_GAME_ACCOUNTS = 10


def _b64(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def _unb64(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


class GameLogin:
    def __init__(self, cfg: dict, store, site):
        self.s = store
        self.site = site  # zdon_site.Site: sso_secret, forge_url, the session key, the member tables
        mode = str(cfg.get("site", {}).get("game_login", "")).strip().lower()
        self.mode = mode if mode in ("required", "optional") else ""

    # ---------------------------------------------------------------------------------------------------------------

    def ready(self) -> bool:
        return bool(self.site.sso_secret and self.site.sso_url)

    def _client(self) -> str:
        return self.site.sso_url.rstrip("/").rsplit("/", 1)[-1] or "zdon"

    def _forge(self, method: str, path: str, query: str = "", body: bytes = b"", timeout: float = 15):
        """(status, json) from TaikoForge's partner API; status 0 when it does not answer."""
        ts = str(int(time.time()))
        signed = hashlib.sha256(body).hexdigest() if method == "POST" else query
        sig = hmac.new(self.site.sso_secret.encode(), f"{method}\n{path}\n{signed}\n{ts}".encode(),
                       hashlib.sha256).hexdigest()
        headers = {"X-TF-Time": ts, "X-TF-Sig": sig, "User-Agent": "zdon-server"}
        if method == "POST":
            headers["Content-Type"] = "application/json"
        url = f"{self.site.forge_url}{path}" + (f"?{query}" if query else "")
        req = urllib.request.Request(url, data=body if method == "POST" else None, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, json.loads(resp.read() or b"{}")
        except urllib.error.HTTPError as e:
            try:
                return e.code, json.loads(e.read() or b"{}")
            except ValueError:
                return e.code, {}
        except (urllib.error.URLError, TimeoutError, OSError, ValueError):
            return 0, {}

    def _token(self, uid: str, email: str) -> str:
        payload = _b64(json.dumps({"uid": uid, "email": email, "exp": int(time.time()) + TOKEN_DAYS * 86400}).encode())
        return f"{payload}.{self.site._sign('game:' + payload)}"

    def _read_token(self, token: str) -> dict | None:
        if not token or "." not in token:
            return None
        payload, sig = token.rsplit(".", 1)
        if not hmac.compare_digest(sig, self.site._sign("game:" + payload)):
            return None
        try:
            data = json.loads(_unb64(payload))
        except ValueError:
            return None
        if not data.get("uid") or data.get("exp", 0) < time.time():
            return None
        return data

    def _accounts(self, uid: str) -> list[dict]:
        return [{"access_code": r["access_code"], "username": r["username"]} for r in self.site._linked(uid)]

    def _pick_account(self, uid: str, code: str, username: str) -> dict:
        """The game account this login plays as: the game's own code when it is this member's or nobody's (then it
        is linked, so its scores stay), else the member's first one, else a new one."""
        now = int(time.time())
        with self.s.lock:
            if code and code not in ("0", "1"):
                row = self.s.one("SELECT access_code, username FROM users WHERE access_code=?", (code,))
                if row is not None:
                    owner = self.s.one("SELECT forge_uid FROM member_links WHERE access_code=?", (code,))
                    if owner is None and len(self._accounts(uid)) < MAX_GAME_ACCOUNTS:
                        self.s.run("INSERT OR IGNORE INTO member_links VALUES (?,?,?)", (code, uid, now))
                        owner = {"forge_uid": uid}
                    if owner is not None and owner["forge_uid"] == uid:
                        return {"access_code": row["access_code"], "username": row["username"]}
            linked = self._accounts(uid)
            if linked:
                return linked[0]
            while True:
                new = str(secrets.randbelow(9 * 10**19) + 10**19)  # 20 digits, as zdon_server.new_access_code
                if not self.s.one("SELECT 1 FROM users WHERE access_code=?", (new,)):
                    break
            name = (username or "").strip()[:32] or "Player"
            self.s.run("INSERT INTO users(access_code, username, idm, created_at) VALUES (?,?,?,?)",
                       (new, name, None, now))
            self.s.run("INSERT OR IGNORE INTO member_links VALUES (?,?,?)", (new, uid, now))
            return {"access_code": new, "username": name}

    # --- handlers: (params, client ip) -> (status, json) ------------------------------------------------------------

    def login(self, p: dict, ip: str = ""):
        if not self.ready():
            return 503, {"detail": "login is not set up on this server", "code": "not_set_up"}
        email = (p.get("email") or "").strip()[:254]
        password = p.get("password") or ""
        if not email or not password or len(password) > 1024:
            return 401, {"detail": "wrong email or password", "code": "wrong"}
        body = json.dumps({"email": email, "password": password, "ip": ip}).encode()
        status, d = self._forge("POST", f"/partner/{self._client()}/login", body=body)
        if status == 0 or status >= 500:
            return 503, {"detail": "TaikoForge did not answer", "code": "unreachable"}
        if status != 200 or not d.get("sub"):
            code = str(d.get("code") or ("wrong" if status == 401 else "refused"))
            return (status if status in (401, 403, 429) else 502), {"detail": str(d.get("error") or ""), "code": code}
        uid, email = str(d["sub"]), str(d.get("email") or email)
        now = int(time.time())
        with self.s.lock:
            self.s.run("""INSERT INTO members(forge_uid, email, created_at, last_login) VALUES (?,?,?,?)
                          ON CONFLICT(forge_uid) DO UPDATE SET email=excluded.email, last_login=excluded.last_login""",
                       (uid, email, now, now))
        acct = self._pick_account(uid, (p.get("access_code") or "").strip(), p.get("username") or "")
        return 200, {"token": self._token(uid, email), "email": email, "access_code": acct["access_code"],
                     "username": acct["username"], "accounts": self._accounts(uid)}

    def session(self, p: dict, ip: str = ""):
        data = self._read_token(p.get("token") or "")
        if data is None:
            return 401, {"detail": "log in again", "code": "expired"}
        uid = str(data["uid"])
        status, d = self._forge("GET", f"/partner/{self._client()}/member",
                                urllib.parse.urlencode({"uid": uid}), timeout=8) if self.ready() else (0, {})
        if status in (403, 404):
            return 403, {"detail": "this TaikoForge account cannot be used", "code": "blocked"}
        email = str(d.get("email") or data.get("email") or "") if status == 200 else str(data.get("email") or "")
        return 200, {"email": email, "accounts": self._accounts(uid)}

    # ---------------------------------------------------------------------------------------------------------------
    # the member's TaikoForge charts, for the game's TaikoForge folder (src/libs/network/zdon_forge_songs.cpp)

    def songs(self, p: dict, ip: str = ""):
        data = self._read_token(p.get("token") or "")
        if data is None or not self.ready():
            return 401, {"detail": "log in again", "code": "expired"}
        from zdon_site import ForgeError
        try:
            _, body, _ = self.site.forge_partner(str(data["uid"]), "")
            d = json.loads(body or b"{}")
        except ForgeError as e:
            return e.status, {"detail": str(e)}
        except ValueError:
            return 502, {"detail": "TaikoForge: not JSON"}
        # paid: a starter-or-above plan (audio kept while it lasts); retention_days: how long audio stays otherwise
        return 200, {"songs": d.get("songs", []), "paid": bool(d.get("paid")),
                     "retention_days": d.get("retention_days", 30)}

    def song_file(self, p: dict, ip: str = ""):
        data = self._read_token(p.get("token") or "")
        if data is None or not self.ready():
            return 401, {"detail": "log in again", "code": "expired"}
        song, file = p.get("id", ""), p.get("file", "")
        if not SONG_ID.match(song) or file not in ("tja", "audio"):
            return 404, {"detail": "not found"}
        from zdon_site import ForgeError
        try:
            _, body, _ = self.site.forge_partner(str(data["uid"]), f"/{song}/{file}")
        except ForgeError as e:
            return e.status, {"detail": str(e)}
        return 200, body
