#!/usr/bin/env python3
"""Z-DON self-hosted server: the game's online API, update distribution and skin hosting in one process.

Only the Python standard library (3.11+) is needed. Data lives in one folder (SQLite + published files).

    python zdon_server.py serve   [--config zdon_server.toml]
    python zdon_server.py publish <install dir> [--channel windows] [--version V] [--with-skins]
    python zdon_server.py publish-skin <skin dir> [--name NAME] [--branch main]
    python zdon_server.py users | jump <access code> (--hash H | --score-id N) | rename <access code> <name>

See server/README.md.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import hashlib
import hmac
import html
import json
import mimetypes
import os
import random
import secrets
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
import tomllib
import urllib.parse
import zipfile
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from http.cookies import SimpleCookie
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from zdon_site import Site  # noqa: E402

HERE = Path(__file__).resolve().parent

DEFAULT_CONFIG = {
    "host": "0.0.0.0",
    "port": 8730,
    # the key the game signs requests with (config.toml [network] zdon_server_key); empty = no signature check
    "auth_key": "",
    "data_dir": "zdon_data",
    "signature_window_s": 300,
    "min_client_version": "",
    "tournament": False,
    "campaigns": [],
    # owned-cosmetic lists: true = every costume is unlocked for everyone (the offline behaviour)
    "force_unlock": True,
    "kinotake": False,
    "gacha_puchichara": [],
    "medals": {"per_play": 1, "daily_bonus": 3, "max": 9999},
    # [[shop]] type = 2 dress / 3 head / 4 body / 5 puchichara / 6 title, id, cost, name (titles)
    "shop": [],
    "max_body_mb": 32,
    # the website (zdon_site.py); members come from TaikoForge's single sign-on
    "site": {"url": "", "name": "Z-DON", "sso_url": "", "sso_secret": "", "forge_url": "https://forge.taiko.ac",
             "session_secret": ""},
}

PROTECTED_INSTALL_NAMES = {"config.toml", "scores.db", "latest.log", "record.log"}
PROTECTED_INSTALL_DIRS = {"cache", "Songs", "Records", "logs", ".zdon_update_tmp"}
DEFAULT_PUBLISH = ["YataiDON.exe", "*.dll", "shader/**", "zdon_update.ps1", "zdon_update.bat"]


def log(msg: str) -> None:
    print(f"[{dt.datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


# ----------------------------------------------------------------------------------------------------------------------
# config / storage


def load_config(path: Path | None) -> dict:
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))
    if path is None:
        path = HERE / "zdon_server.toml"
    if path.exists():
        with open(path, "rb") as f:
            user = tomllib.load(f)
        for k, v in user.items():
            if isinstance(v, dict) and isinstance(cfg.get(k), dict):
                cfg[k].update(v)
            else:
                cfg[k] = v
        cfg["_config_path"] = str(path)
    path = path.resolve()
    data_dir = Path(cfg["data_dir"])
    if not data_dir.is_absolute():
        data_dir = (path.parent if path.exists() else HERE) / data_dir
    cfg["data_dir"] = data_dir
    env_key = os.environ.get("ZDON_SERVER_KEY")
    if env_key:
        cfg["auth_key"] = env_key
    if os.environ.get("ZDON_SSO_SECRET"):
        cfg["site"]["sso_secret"] = os.environ["ZDON_SSO_SECRET"]
    return cfg


SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    access_code TEXT PRIMARY KEY,
    username TEXT NOT NULL,
    idm TEXT UNIQUE,
    title TEXT,
    title_bg INTEGER,
    chara_color_1 TEXT, chara_color_2 TEXT, chara_color_3 TEXT,
    chara_head_index INTEGER, chara_body_index INTEGER, chara_cos_index INTEGER, chara_is_costume INTEGER,
    puchichara INTEGER,
    import_requested INTEGER NOT NULL DEFAULT 0,
    medals INTEGER NOT NULL DEFAULT 0,
    medal_day TEXT,
    ai_tokens INTEGER NOT NULL DEFAULT 0,
    ai_wins INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS scores (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    access_code TEXT NOT NULL,
    hash TEXT NOT NULL,
    difficulty INTEGER NOT NULL,
    score INTEGER, good INTEGER, ok INTEGER, bad INTEGER, drumroll INTEGER, max_combo INTEGER,
    crown INTEGER, rank INTEGER,
    input_log TEXT, played_at INTEGER, modifiers TEXT,
    chara_is_costume INTEGER, chara_cos_index INTEGER,
    source_folder TEXT, gauge INTEGER, judge_params TEXT,
    received_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS scores_by_song ON scores(hash, difficulty, score);
CREATE INDEX IF NOT EXISTS scores_by_user ON scores(access_code, hash, difficulty);
CREATE TABLE IF NOT EXISTS dan_results (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    access_code TEXT NOT NULL, course_id TEXT, title TEXT,
    dan_index INTEGER, dan_index_max INTEGER, dan_color INTEGER, dan_rank INTEGER, is_gaiden INTEGER,
    odai_result INTEGER, score INTEGER, max_combo INTEGER, gauge_length REAL, arrival INTEGER,
    songs TEXT, played_at INTEGER, received_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS jumps (
    access_code TEXT PRIMARY KEY, hash TEXT, score_id INTEGER
);
CREATE TABLE IF NOT EXISTS kinotake (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    access_code TEXT NOT NULL, team INTEGER, base INTEGER, total INTEGER, day TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS owned (
    access_code TEXT NOT NULL, type INTEGER NOT NULL, id INTEGER NOT NULL,
    PRIMARY KEY (access_code, type, id)
);
CREATE TABLE IF NOT EXISTS revives (
    id INTEGER PRIMARY KEY AUTOINCREMENT, access_code TEXT, success INTEGER, at INTEGER
);
"""


class Store:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=NORMAL")
        self.db.executescript(SCHEMA)
        self.lock = threading.RLock()

    def q(self, sql: str, args=()) -> list[sqlite3.Row]:
        with self.lock:
            return self.db.execute(sql, args).fetchall()

    def one(self, sql: str, args=()) -> sqlite3.Row | None:
        rows = self.q(sql, args)
        return rows[0] if rows else None

    def run(self, sql: str, args=()) -> int:
        with self.lock:
            return self.db.execute(sql, args).lastrowid


def today() -> str:
    return dt.date.today().isoformat()


def as_int(v, default=0) -> int:
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return default


def as_bool(v) -> bool:
    return str(v).lower() in ("1", "true", "yes", "on")


def new_access_code(store: Store) -> str:
    while True:
        code = str(secrets.randbelow(9 * 10**19) + 10**19)  # 20 digits
        if not store.one("SELECT 1 FROM users WHERE access_code=?", (code,)):
            return code


# ----------------------------------------------------------------------------------------------------------------------
# request signing (src/libs/network/network.cpp signed_headers)


class Nonces:
    def __init__(self, window: int):
        self.window = window
        self.seen: dict[str, float] = {}
        self.lock = threading.Lock()

    def fresh(self, nonce: str) -> bool:
        now = time.time()
        with self.lock:
            if len(self.seen) > 50000:
                self.seen = {n: t for n, t in self.seen.items() if now - t < 2 * self.window}
            if nonce in self.seen:
                return False
            self.seen[nonce] = now
            return True


def canonical(method: str, path: str, params: dict[str, str], ts: str, nonce: str) -> bytes:
    body = "".join(f"{k}={params[k]}&" for k in sorted(params, key=lambda s: s.encode()))
    return f"{method}\n{path}\n{body}\n{ts}\n{nonce}".encode()


def signature_ok(key: str, method: str, path: str, params: dict[str, str], headers, window: int, nonces: Nonces):
    ts, nonce, sig = headers.get("X-Timestamp"), headers.get("X-Nonce"), headers.get("X-Signature")
    if not ts or not nonce or not sig:
        return False, "unsigned"
    if abs(time.time() - as_int(ts, 0)) > window:
        return False, "clock"
    # The client signs some keys it does not send (an empty idm) and sends some it does not sign (empty
    # source_folder / gauge / judge_params): try the received set, without its empty values, and with idm added.
    candidates = [params, {k: v for k, v in params.items() if v != ""}]
    if "idm" not in params:
        candidates.append({**params, "idm": ""})
    for p in candidates:
        want = hmac.new(key.encode(), canonical(method, path, p, ts, nonce), hashlib.sha256).hexdigest()
        if hmac.compare_digest(want, sig.lower()):
            if not nonces.fresh(nonce):
                return False, "replayed"
            return True, ""
    return False, "signature"


# ----------------------------------------------------------------------------------------------------------------------
# the game API


class Api:
    UNSIGNED = {("GET", "/reward_shop"), ("GET", "/ai_battle"), ("GET", "/rewards")}

    def __init__(self, cfg: dict, store: Store):
        self.cfg = cfg
        self.s = store
        self.nonces = Nonces(int(cfg["signature_window_s"]))
        self.routes = {
            ("GET", "/health"): self.health,
            ("GET", "/user"): self.user,
            ("POST", "/register_user"): self.register_user,
            ("POST", "/update_username"): self.update_username,
            ("POST", "/update_costume"): self.update_costume,
            ("POST", "/clear_import_flag"): self.clear_import_flag,
            ("POST", "/submit_score"): self.submit_score,
            ("POST", "/submit_dan"): self.submit_dan,
            ("GET", "/song_ranking"): self.song_ranking,
            ("GET", "/replay"): self.replay,
            ("GET", "/poll_jump"): self.poll_jump,
            ("POST", "/gacha"): self.gacha,
            ("POST", "/kinotake"): self.kinotake,
            ("POST", "/don_points"): self.don_points,
            ("POST", "/medals"): self.medals,
            ("POST", "/revive_result"): self.revive_result,
            ("POST", "/ai_battle"): self.ai_battle_submit,
            ("GET", "/ai_battle"): self.ai_battle_status,
            ("GET", "/reward_shop"): self.reward_shop,
            ("POST", "/reward_shop/buy"): self.reward_shop_buy,
            ("GET", "/rewards"): self.rewards,
            ("POST", "/campaign_costume"): self.campaign_costume,
        }

    # each handler: (params) -> (status, json-able | str)
    def handle(self, method: str, path: str, params: dict[str, str], headers):
        fn = self.routes.get((method, path))
        if fn is None:
            return None
        key = self.cfg["auth_key"]
        if key and (method, path) not in self.UNSIGNED:
            ok, why = signature_ok(key, method, path, params, headers, int(self.cfg["signature_window_s"]),
                                   self.nonces)
            if not ok:
                return 401, {"detail": f"bad request signature ({why})"}
        return fn(params)

    def _user(self, p) -> sqlite3.Row | None:
        code = p.get("access_code", "")
        return self.s.one("SELECT * FROM users WHERE access_code=?", (code,)) if code else None

    def health(self, p):
        out = {"status": "ok", "tournament": bool(self.cfg["tournament"]), "campaigns": list(self.cfg["campaigns"])}
        if self.cfg["min_client_version"]:
            out["min_client_version"] = str(self.cfg["min_client_version"])
        if self.cfg["kinotake"]:
            out["kinotake"] = self._kinotake_today()
        return 200, out

    def register_user(self, p):
        username = (p.get("username") or "Player").strip()[:32] or "Player"
        idm = p.get("idm") or None
        if idm:
            row = self.s.one("SELECT access_code FROM users WHERE idm=?", (idm,))
            if row:
                return 200, row["access_code"]
        code = new_access_code(self.s)
        self.s.run("INSERT INTO users(access_code, username, idm, created_at) VALUES (?,?,?,?)",
                   (code, username, idm, int(time.time())))
        log(f"registered {username!r}{' (card)' if idm else ''}")
        return 200, code

    def user(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        out = {"username": u["username"], "import_requested": bool(u["import_requested"])}
        for k in ("title", "chara_color_1", "chara_color_2", "chara_color_3"):
            if u[k] is not None:
                out[k] = u[k]
        if u["title_bg"] is not None:
            out["title_bg"] = u["title_bg"]
        if u["chara_head_index"] is not None:
            out.update(chara_head_index=u["chara_head_index"], chara_body_index=u["chara_body_index"],
                       chara_cos_index=u["chara_cos_index"], chara_is_costume=bool(u["chara_is_costume"]))
        out["puchichara"] = u["puchichara"]
        rows = self.s.q(
            """SELECT s.hash, s.difficulty, s.score, s.good, s.ok, s.bad, s.drumroll, s.max_combo,
                      b.crown, b.rank
               FROM scores s
               JOIN (SELECT hash, difficulty, MAX(score) AS best, MAX(crown) AS crown, MAX(rank) AS rank
                     FROM scores WHERE access_code=? GROUP BY hash, difficulty) b
                 ON s.hash=b.hash AND s.difficulty=b.difficulty AND s.score=b.best
               WHERE s.access_code=? GROUP BY s.hash, s.difficulty""", (u["access_code"], u["access_code"]))
        out["scores"] = [dict(r) for r in rows]
        return 200, out

    def update_username(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        name = (p.get("username") or "").strip()[:32]
        if not name:
            return 400, {"detail": "empty username"}
        self.s.run("UPDATE users SET username=? WHERE access_code=?", (name, u["access_code"]))
        return 200, {"ok": True}

    def update_costume(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        self.s.run("""UPDATE users SET chara_head_index=?, chara_body_index=?, chara_cos_index=?, chara_is_costume=?
                      WHERE access_code=?""",
                   (as_int(p.get("chara_head_index")), as_int(p.get("chara_body_index")),
                    as_int(p.get("chara_cos_index")), int(as_bool(p.get("chara_is_costume"))), u["access_code"]))
        return 200, {"ok": True}

    def clear_import_flag(self, p):
        self.s.run("UPDATE users SET import_requested=0 WHERE access_code=?", (p.get("access_code", ""),))
        return 200, {"ok": True}

    def submit_score(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        if not p.get("hash"):
            return 400, {"detail": "no hash"}
        try:
            log_json = json.dumps(json.loads(p.get("input_log") or "[]"), separators=(",", ":"))
        except ValueError:
            return 400, {"detail": "bad input_log"}
        sid = self.s.run(
            """INSERT INTO scores(access_code, hash, difficulty, score, good, ok, bad, drumroll, max_combo, crown, rank,
                                  input_log, played_at, modifiers, chara_is_costume, chara_cos_index, source_folder,
                                  gauge, judge_params, received_at)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (u["access_code"], p["hash"], as_int(p.get("difficulty")), as_int(p.get("score")), as_int(p.get("good")),
             as_int(p.get("ok")), as_int(p.get("bad")), as_int(p.get("drumroll")), as_int(p.get("max_combo")),
             as_int(p.get("crown")), as_int(p.get("rank")), log_json, as_int(p.get("played_at"), None),
             p.get("modifiers") or "", int(as_bool(p.get("chara_is_costume"))), as_int(p.get("chara_cos_index")),
             p.get("source_folder") or None, as_int(p.get("gauge"), None) if p.get("gauge") else None,
             p.get("judge_params") or None, int(time.time())))
        if p.get("zdon_title"):
            self.s.run("""INSERT INTO songs(hash, title, subtitle, updated_at) VALUES (?,?,?,?)
                          ON CONFLICT(hash) DO UPDATE SET title=excluded.title, subtitle=excluded.subtitle,
                          updated_at=excluded.updated_at""",
                       (p["hash"], p["zdon_title"][:200], (p.get("zdon_subtitle") or "")[:200], int(time.time())))
        log(f"score #{sid}: {u['username']} {p.get('zdon_title') or p['hash'][:10]} d{p.get('difficulty')} {p.get('score')}")
        return 200, {"ok": True, "score_id": sid}

    def submit_dan(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        self.s.run(
            """INSERT INTO dan_results(access_code, course_id, title, dan_index, dan_index_max, dan_color, dan_rank,
                                       is_gaiden, odai_result, score, max_combo, gauge_length, arrival, songs,
                                       played_at, received_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (u["access_code"], p.get("course_id"), p.get("title"), as_int(p.get("dan_index")),
             as_int(p.get("dan_index_max")), as_int(p.get("dan_color")), as_int(p.get("dan_rank")),
             int(as_bool(p.get("is_gaiden"))), as_int(p.get("odai_result")), as_int(p.get("score")),
             as_int(p.get("max_combo")), float(p.get("gauge_length") or 0), as_int(p.get("arrival")),
             p.get("songs") or "[]", as_int(p.get("played_at")), int(time.time())))
        return 200, {"ok": True}

    def _ranking(self, song_hash: str, limit: int, difficulty: int | None = None):
        if difficulty is None:
            row = self.s.one("SELECT MAX(difficulty) AS d FROM scores WHERE hash=?", (song_hash,))
            if row is None or row["d"] is None:
                return None, []
            difficulty = row["d"]
        rows = self.s.q("""SELECT u.username, MAX(s.score) AS score FROM scores s JOIN users u USING(access_code)
                           WHERE s.hash=? AND s.difficulty=? GROUP BY s.access_code
                           ORDER BY score DESC LIMIT ?""", (song_hash, difficulty, limit))
        return difficulty, [{"username": r["username"], "score": r["score"]} for r in rows]

    def song_ranking(self, p):
        difficulty, rows = self._ranking(p.get("hash", ""), max(1, min(100, as_int(p.get("limit"), 3))))
        if difficulty is None:
            return 404, {"detail": "no scores for this song"}
        return 200, {"difficulty": difficulty, "ranking": rows}

    def replay(self, p):
        r = self.s.one("""SELECT s.*, u.username, u.title, u.title_bg, u.chara_color_1, u.chara_color_2,
                                 u.chara_color_3, u.chara_head_index, u.chara_body_index
                          FROM scores s JOIN users u USING(access_code) WHERE s.id=?""", (as_int(p.get("score_id")),))
        if r is None:
            return 404, {"detail": "no such score"}
        out = {"hash": r["hash"], "difficulty": r["difficulty"], "input_log": json.loads(r["input_log"] or "[]"),
               "username": r["username"], "chara_is_costume": bool(r["chara_is_costume"]),
               "chara_cos_index": r["chara_cos_index"]}
        for k in ("title", "title_bg", "chara_color_1", "chara_color_2", "chara_color_3", "chara_head_index",
                  "chara_body_index"):
            if r[k] is not None:
                out[k] = r[k]
        return 200, out

    def poll_jump(self, p):
        code = p.get("access_code", "")
        with self.s.lock:
            row = self.s.one("SELECT hash, score_id FROM jumps WHERE access_code=?", (code,))
            self.s.run("DELETE FROM jumps WHERE access_code=?", (code,))
        out = {}
        if row is not None:
            if row["hash"]:
                out["hash"] = row["hash"]
            if row["score_id"] is not None:
                out["score_id"] = row["score_id"]
        return 200, out

    def gacha(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        if not self.cfg["tournament"]:
            return 403, {"detail": "not_tournament"}
        pool = [as_int(x) for x in self.cfg["gacha_puchichara"]]
        if not pool:
            return 503, {"detail": "offline_gacha"}
        pick = random.choice(pool)
        self.s.run("INSERT OR IGNORE INTO owned VALUES (?,?,?)", (u["access_code"], 5, pick))
        self.s.run("UPDATE users SET puchichara=? WHERE access_code=?", (pick, u["access_code"]))
        return 200, {"puchichara": pick}

    def _kinotake_today(self):
        rows = self.s.q("SELECT team, SUM(total) AS t FROM kinotake WHERE day=? GROUP BY team", (today(),))
        tot = {r["team"]: r["t"] or 0 for r in rows}
        return {"kinoko": tot.get(0, 0), "takenoko": tot.get(1, 0)}

    def kinotake(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        team = as_int(p.get("team"), -1)
        if team not in (0, 1):
            return 400, {"detail": "bad team"}
        base = max(0, as_int(p.get("score")))
        assist = 2 if as_bool(p.get("costume")) else 1
        total = base * assist
        self.s.run("INSERT INTO kinotake(access_code, team, base, total, day) VALUES (?,?,?,?,?)",
                   (u["access_code"], team, base, total, today()))
        mine = self.s.one("SELECT SUM(total) AS t FROM kinotake WHERE access_code=?", (u["access_code"],))["t"]
        return 200, {"base": base, "total": total, "mine": mine or 0, "assist_rate": assist, **self._kinotake_today()}

    def don_points(self, p):
        if self._user(p) is None:
            return 404, {"detail": "unknown access code"}
        songs = [max(0, as_int(g)) // 10 for g in (p.get("gauges") or "").split(",") if g.strip()]
        return 200, {"current": sum(songs), "aim": -1, "songs": songs, "titles": [], "presents": []}

    def medals(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        m = self.cfg["medals"]
        cap = as_int(m.get("max"), 9999)
        daily = u["medal_day"] != today()
        add = as_int(m.get("per_play"), 1) + (as_int(m.get("daily_bonus"), 0) if daily else 0)
        now = u["medals"]
        new = min(cap, now + add)
        self.s.run("UPDATE users SET medals=?, medal_day=? WHERE access_code=?", (new, today(), u["access_code"]))
        return 200, {"now": now, "add": new - now, "max": cap, "daily": daily, "weekly": False, "monthly": False}

    def revive_result(self, p):
        self.s.run("INSERT INTO revives(access_code, success, at) VALUES (?,?,?)",
                   (p.get("access_code"), int(as_bool(p.get("success"))), int(time.time())))
        return 200, {"ok": True}

    @staticmethod
    def _ai_rank(tokens: int) -> tuple[int, int]:
        rank = min(10, tokens // 10)
        return rank, (-1 if rank >= 10 else (rank + 1) * 10)

    def ai_battle_submit(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        won = as_bool(p.get("won"))
        tokens_before, wins_before = u["ai_tokens"], u["ai_wins"]
        tokens_after = tokens_before + (max(1, as_int(p.get("stars"), 1)) if won else 0)
        wins_after = wins_before + int(won)
        self.s.run("UPDATE users SET ai_tokens=?, ai_wins=? WHERE access_code=?",
                   (tokens_after, wins_after, u["access_code"]))
        rb, nb = self._ai_rank(tokens_before)
        ra, na = self._ai_rank(tokens_after)
        return 200, {"rank_before": rb, "rank_after": ra, "tokens_before": tokens_before,
                     "tokens_after": tokens_after, "next_rank_tokens": nb, "next_rank_after_tokens": na,
                     "wins_before": wins_before, "wins_after": wins_after, "fulfilled": won,
                     "first": wins_before == 0 and won, "challenge": as_bool(p.get("assigned")),
                     "next_reward_wins": -1, "all_rewards": True, "bests": []}

    def ai_battle_status(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        rank, nxt = self._ai_rank(u["ai_tokens"])
        return 200, {"rank": rank, "tokens": u["ai_tokens"], "next_rank_tokens": nxt, "wins": u["ai_wins"],
                     "next_reward_wins": -1}

    def _owned(self, code: str, typ: int, item: int) -> bool:
        return self.s.one("SELECT 1 FROM owned WHERE access_code=? AND type=? AND id=?", (code, typ, item)) is not None

    def reward_shop(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        goods = []
        for g in self.cfg["shop"]:
            typ, item = as_int(g.get("type"), -1), as_int(g.get("id"), -1)
            entry = {"type": typ, "id": item, "cost": as_int(g.get("cost")),
                     "owned": self._owned(u["access_code"], typ, item)}
            if g.get("name"):
                entry["name"] = str(g["name"])
            goods.append(entry)
        return 200, {"medals": u["medals"], "max": as_int(self.cfg["medals"].get("max"), 9999), "goods": goods}

    def reward_shop_buy(self, p):
        u = self._user(p)
        if u is None:
            return 404, {"detail": "unknown access code"}
        typ, item = as_int(p.get("type"), -1), as_int(p.get("item_id"), -1)
        good = next((g for g in self.cfg["shop"] if as_int(g.get("type"), -1) == typ and as_int(g.get("id"), -1) == item),
                    None)
        if good is None:
            return 409, {"detail": "lineup"}
        with self.s.lock:
            if self._owned(u["access_code"], typ, item):
                return 409, {"detail": "already"}
            medals = self.s.one("SELECT medals FROM users WHERE access_code=?", (u["access_code"],))["medals"]
            cost = as_int(good.get("cost"))
            if medals < cost:
                return 409, {"detail": "shortage"}
            self.s.run("UPDATE users SET medals=? WHERE access_code=?", (medals - cost, u["access_code"]))
            self.s.run("INSERT INTO owned VALUES (?,?,?)", (u["access_code"], typ, item))
        return 200, {"medals": medals - cost}

    def rewards(self, p):
        u = self._user(p)
        if u is None or self.cfg["force_unlock"]:
            return 200, {"force_unlock": True}
        lists = {"costume": 2, "head": 3, "body": 4}
        out = {"force_unlock": False}
        for name, typ in lists.items():
            out[name] = [r["id"] for r in self.s.q("SELECT id FROM owned WHERE access_code=? AND type=?",
                                                     (u["access_code"], typ))]
        return 200, out

    def campaign_costume(self, p):
        u = self._user(p)
        if u is not None:
            self.s.run("INSERT OR IGNORE INTO owned VALUES (?,?,?)", (u["access_code"], 2, as_int(p.get("cos"))))
        return 200, {"ok": True}


# ----------------------------------------------------------------------------------------------------------------------
# published files: game updates (update/<channel>/...) and skins (skins/<name>/<branch>/...)


def safe_join(root: Path, rel: str) -> Path | None:
    rel = urllib.parse.unquote(rel)
    parts = [x for x in rel.replace("\\", "/").split("/") if x not in ("", ".")]
    if any(x == ".." or ":" in x for x in parts):
        return None
    path = root.joinpath(*parts) if parts else root
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return None
    return path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_manifest(root: Path, files: list[str]) -> None:
    lines = []
    for rel in sorted(files):
        lines.append(f"{sha256_file(root / rel)}  {rel}\n")
    (root / "checksums.sha256").write_text("".join(lines), encoding="utf-8", newline="\n")


def collect(src: Path, patterns: list[str], excludes: list[str]) -> list[str]:
    out = []
    for p in sorted(src.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(src).as_posix()
        top = rel.split("/", 1)[0]
        if rel in PROTECTED_INSTALL_NAMES or top in PROTECTED_INSTALL_DIRS or rel.startswith(".zdon"):
            continue
        if any(ch.isspace() for ch in rel):
            log(f"skipped (whitespace in path; the skin updater can't fetch it): {rel}")
            continue
        if not any(fnmatch.fnmatchcase(rel, pat) or (pat.endswith("/**") and rel.startswith(pat[:-2]))
                   for pat in patterns):
            continue
        if any(fnmatch.fnmatchcase(rel, pat) for pat in excludes):
            continue
        out.append(rel)
    return out


def make_zip(root: Path, files: list[str], out: Path) -> None:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for rel in sorted(files):
            z.write(root / rel, rel)


def publish_tree(sources: dict[str, Path], dest: Path, version: str | None) -> None:
    """Copies sources (relative path -> file) into a fresh folder next to dest, writes its manifest, then swaps it in.
    With a version (a game channel) the files go under files/ and version.txt sits beside the manifest."""
    staging = dest.with_name(dest.name + ".new")
    old = dest.with_name(dest.name + ".old")
    shutil.rmtree(staging, ignore_errors=True)
    shutil.rmtree(old, ignore_errors=True)
    files_root = staging / "files" if version is not None else staging
    for rel, src in sources.items():
        target = files_root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
    write_manifest(files_root, list(sources))
    if version is not None:
        os.replace(files_root / "checksums.sha256", staging / "checksums.sha256")
        (staging / "version.txt").write_text(version + "\n", encoding="utf-8", newline="\n")
        # the website's first-install download: the same files as one zip
        make_zip(files_root, list(sources), staging / "package.zip")
    else:
        # a skin: <name>/<branch>.zip beside the branch folder
        tmp_zip = dest.with_name(dest.name + ".zip.new")
        make_zip(staging, list(sources), tmp_zip)
        os.replace(tmp_zip, dest.with_name(dest.name + ".zip"))
    if dest.exists():
        os.replace(dest, old)
    os.replace(staging, dest)
    shutil.rmtree(old, ignore_errors=True)


# ----------------------------------------------------------------------------------------------------------------------
# HTTP


def skin_index(data: Path, base: str) -> list[dict]:
    out = []
    root = data / "skins"
    if not root.is_dir():
        return out
    for skin in sorted(p for p in root.iterdir() if p.is_dir()):
        meta_file = skin / "meta.json"
        meta = json.loads(meta_file.read_text(encoding="utf-8")) if meta_file.is_file() else {}
        branch = meta.get("branch", "main")
        if not (skin / branch / "checksums.sha256").is_file():
            continue
        out.append({"name": skin.name, "url": f"{base}/skins/{urllib.parse.quote(skin.name)}", "branch": branch,
                    "parent": meta.get("parent", ""), "files": meta.get("files", 0), "size": meta.get("size", 0)})
    # parents before their children, so a player picking a child also sees what it needs first
    names = {e["name"] for e in out}
    ordered, placed = [], set()
    while len(ordered) < len(out):
        progressed = False
        for e in out:
            if e["name"] not in placed and (not e["parent"] or e["parent"] in placed or e["parent"] not in names):
                ordered.append(e)
                placed.add(e["name"])
                progressed = True
        if not progressed:
            ordered += [e for e in out if e["name"] not in placed]
            break
    return ordered


def site_base(handler) -> str:
    url = str(handler.server_cfg["site"].get("url", "")).rstrip("/")
    return url or f"http://{handler.headers.get('Host', '127.0.0.1')}"


def make_handler(cfg: dict, api: Api, site):
    data: Path = cfg["data_dir"]
    max_body = int(cfg["max_body_mb"]) * 1024 * 1024

    class Handler(BaseHTTPRequestHandler):
        server_version = "ZDONServer/1"
        server_cfg = cfg
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            if os.environ.get("ZDON_SERVER_VERBOSE"):
                log(f"{self.client_address[0]} {fmt % args}")

        def _send(self, status: int, body: bytes, ctype: str, extra: dict | None = None):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _json(self, status: int, obj):
            if isinstance(obj, str):
                self._send(status, obj.encode(), "text/plain; charset=utf-8")
            else:
                self._send(status, json.dumps(obj, ensure_ascii=False).encode(), "application/json; charset=utf-8")

        def _params(self, url) -> dict[str, str] | None:
            params = {k: v[-1] for k, v in urllib.parse.parse_qs(url.query, keep_blank_values=True).items()}
            if self.command == "POST":
                length = as_int(self.headers.get("Content-Length"), 0)
                if length > max_body:
                    return None
                raw = self.rfile.read(length) if length > 0 else b""
                ctype = self.headers.get("Content-Type", "")
                if raw and "application/x-www-form-urlencoded" in ctype:
                    form = urllib.parse.parse_qs(raw.decode("utf-8", "replace"), keep_blank_values=True)
                    params.update({k: v[-1] for k, v in form.items()})
                elif raw and "application/json" in ctype:
                    try:
                        params.update({k: v if isinstance(v, str) else json.dumps(v)
                                       for k, v in json.loads(raw).items()})
                    except (ValueError, AttributeError):
                        return None
            return params

        def _file(self, path: Path | None):
            if path is None or not path.is_file():
                self._json(404, {"detail": "not found"})
                return
            size = path.stat().st_size
            ctype = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(size))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            if self.command == "HEAD":
                return
            with open(path, "rb") as f:
                shutil.copyfileobj(f, self.wfile, 1 << 20)

        def _published(self, path: str) -> bool:
            # /update/<channel>/(version.txt|checksums.sha256|files/<rel>)
            if path.startswith("/update/"):
                self._file(safe_join(data / "update", path[len("/update/"):]))
                return True
            # /skins/index.json: every published skin, for the game's first-run skin list
            if path == "/skins/index.json":
                self._json(200, skin_index(data, site_base(self)))
                return True
            # /skins/<name>/archive/<branch>.zip: the whole skin, for a first install
            if path.startswith("/skins/"):
                parts = path[len("/skins/"):].split("/")
                if len(parts) == 3 and parts[1] == "archive" and parts[2].endswith(".zip"):
                    root = safe_join(data / "skins", urllib.parse.unquote(parts[0]))
                    self._file(safe_join(root, urllib.parse.unquote(parts[2])) if root else None)
                    return True
            # /skins/<name>/(raw|media)/branch/<branch>/<rel>  (the layout the skin updater asks for)
            if path.startswith("/skins/"):
                parts = path[len("/skins/"):].split("/", 4)
                if len(parts) == 5 and parts[1] in ("raw", "media") and parts[2] == "branch":
                    name, branch, rel = parts[0], parts[3], parts[4]
                    root = safe_join(data / "skins", f"{urllib.parse.unquote(name)}/{urllib.parse.unquote(branch)}")
                    self._file(safe_join(root, rel) if root else None)
                    return True
                return False
            return False

        # --- what zdon_site.Site uses ---
        sess = None
        headers_out: list[str] = []

        @property
        def cookies(self) -> dict[str, str]:
            jar = SimpleCookie()
            try:
                jar.load(self.headers.get("Cookie", ""))
            except Exception:  # noqa: BLE001 - a malformed cookie header is no cookie
                return {}
            return {k: m.value for k, m in jar.items()}

        def form(self) -> dict[str, str]:
            length = min(as_int(self.headers.get("Content-Length"), 0), 64 * 1024)
            raw = self.rfile.read(length) if length > 0 else b""
            return {k: v[-1] for k, v in urllib.parse.parse_qs(raw.decode("utf-8", "replace"),
                                                                keep_blank_values=True).items()}

        def send(self, status: int, body: bytes, ctype: str, extra: dict | None = None):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("X-Content-Type-Options", "nosniff")
            for c in self.headers_out:
                self.send_header("Set-Cookie", c)
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def send_html(self, status: int, doc: str):
            self.send(status, doc.encode(), "text/html; charset=utf-8",
                      {"Cache-Control": "no-store", "X-Frame-Options": "DENY", "Referrer-Policy": "same-origin"})

        def redirect(self, location: str, cookies: list[str] | None = None):
            self.headers_out = list(self.headers_out) + list(cookies or [])
            self.send(303, b"", "text/plain", {"Location": location, "Cache-Control": "no-store"})

        def send_file(self, path: Path, download_name: str):
            self.headers_out = []
            quoted = urllib.parse.quote(download_name)
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", str(path.stat().st_size))
            self.send_header("Content-Disposition", f"attachment; filename*=UTF-8''{quoted}")
            self.end_headers()
            if self.command != "HEAD":
                with open(path, "rb") as f:
                    shutil.copyfileobj(f, self.wfile, 1 << 20)

        def _dispatch(self):
            url = urllib.parse.urlsplit(self.path)
            path = url.path.rstrip("/") or "/"
            self.headers_out = []
            if self.command in ("GET", "HEAD") and self._published(url.path):
                return
            if site.handles(path) and self.command in ("GET", "HEAD", "POST"):
                query = {k: v[-1] for k, v in urllib.parse.parse_qs(url.query, keep_blank_values=True).items()}
                try:
                    site.handle(self, "GET" if self.command == "HEAD" else self.command, path, query)
                except Exception as e:  # noqa: BLE001
                    log(f"site error in {self.command} {path}: {e!r}")
                    self.headers_out = []
                    self.send(500, "伺服器錯誤".encode(), "text/plain; charset=utf-8")
                return
            params = self._params(url)
            if params is None:
                self._json(413, {"detail": "body too large or malformed"})
                return
            try:
                res = api.handle("GET" if self.command == "HEAD" else self.command, path, params, self.headers)
            except Exception as e:  # noqa: BLE001 - one bad request must not take the server down
                log(f"error in {self.command} {path}: {e!r}")
                self._json(500, {"detail": "server error"})
                return
            if res is None:
                self._json(404, {"detail": "not found"})
            else:
                self._json(*res)

        do_GET = do_POST = do_HEAD = _dispatch

    return Handler


# ----------------------------------------------------------------------------------------------------------------------
# commands


def cmd_serve(cfg: dict, args) -> None:
    store = Store(cfg["data_dir"] / "zdon.db")
    api = Api(cfg, store)
    site = Site(cfg, store, api.nonces)
    host, port = args.host or cfg["host"], args.port or int(cfg["port"])
    # On Windows SO_REUSEADDR lets a second server bind a port another one still holds (and the old one keeps
    # answering), so a restart that overlaps would silently serve stale code: bind exclusively there.
    ThreadingHTTPServer.allow_reuse_address = os.name != "nt"
    httpd = ThreadingHTTPServer((host, port), make_handler(cfg, api, site))
    httpd.daemon_threads = True
    log(f"Z-DON server on http://{host}:{port}  (data: {cfg['data_dir']})")
    if not cfg["auth_key"]:
        log("auth_key is empty: requests are not checked (fine on a LAN, not on the internet)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        log("stopped")


def git_version(src: Path) -> str:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M")
    try:
        rev = subprocess.run(["git", "-C", str(src), "rev-parse", "--short", "HEAD"], capture_output=True, text=True,
                             timeout=5).stdout.strip()
        return f"{stamp}-{rev}" if rev else stamp
    except (OSError, subprocess.SubprocessError):
        return stamp


def cmd_publish(cfg: dict, args) -> None:
    src = Path(args.src).resolve()
    if not (src / "YataiDON.exe").exists() and not (src / "YataiDON").exists():
        sys.exit(f"{src}: no YataiDON executable here")
    patterns = list(args.include or DEFAULT_PUBLISH)
    if args.with_skins:
        patterns.append("Skins/**")
    sources = {rel: src / rel for rel in collect(src, patterns, args.exclude or [])}
    # the updater scripts ship from the repo when the install folder has none
    for name in ("zdon_update.ps1", "zdon_update.bat"):
        if name not in sources and (HERE.parent / name).exists():
            sources[name] = HERE.parent / name
    version = args.version or git_version(HERE.parent)
    dest = cfg["data_dir"] / "update" / args.channel
    # where zdon_update.bat looks when config.toml names no source (a build with a compiled-in server)
    site_url = str(cfg["site"].get("url", "")).rstrip("/")
    if site_url:
        url_file = cfg["data_dir"] / f".zdon_update_{args.channel}.url"
        url_file.parent.mkdir(parents=True, exist_ok=True)
        url_file.write_text(f"{site_url}/update/{args.channel}\n", encoding="utf-8", newline="\n")
        sources["zdon_update.url"] = url_file
    publish_tree(sources, dest, version)
    size = sum(p.stat().st_size for p in sources.values())
    print(f"published {len(sources)} files ({size / 1e6:.1f} MB) as {args.channel} {version}")


def cmd_publish_skin(cfg: dict, args) -> None:
    src = Path(args.src).resolve()
    if not (src / "skin.json").exists() and not (src / "Graphics").is_dir():
        sys.exit(f"{src}: doesn't look like a skin folder")
    name = args.name or src.name
    files = collect(src, ["*", "**"], [".skin-repo", ".skin-update-tmp/**", "*.bak", ".git/**"])
    files = [f for f in files if not f.startswith(".git/") and f != "checksums.sha256"]
    dest = cfg["data_dir"] / "skins" / name / args.branch
    dest.parent.mkdir(parents=True, exist_ok=True)
    publish_tree({rel: src / rel for rel in files}, dest, None)
    # the skin it inherits from (Graphics/skin_config.json "parent"), for the game's first-run list
    parent = args.parent
    if parent is None:
        try:
            conf = json.loads((src / "Graphics" / "skin_config.json").read_text(encoding="utf-8-sig"))
            parent = conf.get("parent") or (conf.get("screen") or {}).get("parent", "")
        except (OSError, ValueError, AttributeError):
            parent = ""
    meta = {"branch": args.branch, "parent": parent or "", "files": len(files),
            "size": sum((src / f).stat().st_size for f in files), "published": int(time.time())}
    (dest.parent / "meta.json").write_text(json.dumps(meta, ensure_ascii=False), encoding="utf-8")
    print(f"published skin {name} ({args.branch}): {len(files)} files, parent {parent or '(none)'}")
    print(f"in the game's Skins/{src.name}/.skin-repo put:\n  http://<this host>:{cfg['port']}/skins/{name}\n"
          f"  {args.branch}")


def cmd_users(cfg: dict, args) -> None:
    store = Store(cfg["data_dir"] / "zdon.db")
    for r in store.q("""SELECT u.access_code, u.username, u.idm IS NOT NULL AS card, COUNT(s.id) AS plays
                        FROM users u LEFT JOIN scores s USING(access_code) GROUP BY u.access_code
                        ORDER BY u.created_at"""):
        print(f"{r['access_code']}  {r['username']:<24} {'card' if r['card'] else '    '}  {r['plays']} plays")


def cmd_jump(cfg: dict, args) -> None:
    store = Store(cfg["data_dir"] / "zdon.db")
    if not store.one("SELECT 1 FROM users WHERE access_code=?", (args.access_code,)):
        sys.exit("unknown access code")
    store.run("INSERT OR REPLACE INTO jumps VALUES (?,?,?)", (args.access_code, args.hash, args.score_id))
    print("queued; the game picks it up on its next poll")


def cmd_rename(cfg: dict, args) -> None:
    store = Store(cfg["data_dir"] / "zdon.db")
    store.run("UPDATE users SET username=? WHERE access_code=?", (args.name, args.access_code))
    print("ok")


def main(argv=None) -> None:
    ap = argparse.ArgumentParser(description="Z-DON self-hosted server")
    ap.add_argument("--config", type=Path, help="zdon_server.toml (default: next to this script)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve", help="run the server")
    s.add_argument("--host")
    s.add_argument("--port", type=int)
    s = sub.add_parser("publish", help="publish a game build for the updater")
    s.add_argument("src", help="install folder (e.g. build/bin)")
    s.add_argument("--channel", default="windows")
    s.add_argument("--version")
    s.add_argument("--with-skins", action="store_true", help="ship Skins/ too (normally the skin updater's job)")
    s.add_argument("--include", action="append", help=f"glob to publish (repeatable; default {DEFAULT_PUBLISH})")
    s.add_argument("--exclude", action="append", help="glob to leave out (repeatable)")
    s = sub.add_parser("publish-skin", help="publish a skin folder for the skin updater")
    s.add_argument("src")
    s.add_argument("--name")
    s.add_argument("--branch", default="main")
    s.add_argument("--parent", help="the skin it inherits from (default: Graphics/skin_config.json)")
    sub.add_parser("users", help="list players and their access codes")
    s = sub.add_parser("jump", help="send a player's game to a song or a replay")
    s.add_argument("access_code")
    g = s.add_mutually_exclusive_group(required=True)
    g.add_argument("--hash")
    g.add_argument("--score-id", type=int)
    s = sub.add_parser("rename", help="change a player's name")
    s.add_argument("access_code")
    s.add_argument("name")
    args = ap.parse_args(argv)
    cfg = load_config(args.config)
    {"serve": cmd_serve, "publish": cmd_publish, "publish-skin": cmd_publish_skin, "users": cmd_users,
     "jump": cmd_jump, "rename": cmd_rename}[args.cmd](cfg, args)


if __name__ == "__main__":
    main()
