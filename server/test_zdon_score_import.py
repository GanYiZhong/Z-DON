"""Tests for the scores.db import on my page (zdon_site_import.py).

    cd server && python -m unittest test_zdon_score_import -v

Starts the whole server (zdon_server.make_handler) on a random local port with a temporary data folder; never touches
zdon_data or zdon_server.toml.
"""
from __future__ import annotations

import html
import json
import re
import secrets
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zdon_server as Z  # noqa: E402
import zdon_site as S  # noqa: E402
from zdon_site_import import parse_multipart  # noqa: E402

CODE_A = "12345678901234567890"   # the member's game account; also the player_id in the file
CODE_B = "99999999999999999999"   # another member's account
H1, H2, H3, H4, H5 = ("a" * 32, "b" * 32, "c" * 32, "d" * 32, "e" * 32)

GAME_SCHEMA = """
CREATE TABLE players (player_id TEXT PRIMARY KEY, created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    username TEXT NOT NULL UNIQUE, title TEXT NOT NULL);
CREATE TABLE songs (song_id INTEGER PRIMARY KEY AUTOINCREMENT, hash_0 TEXT, hash_1 TEXT, hash_2 TEXT, hash_3 TEXT,
    hash_4 TEXT, title TEXT NOT NULL, subtitle TEXT, UNIQUE(hash_0, hash_1, hash_2, hash_3, hash_4));
CREATE TABLE scores (score_id INTEGER PRIMARY KEY AUTOINCREMENT, player_id TEXT NOT NULL REFERENCES players(player_id),
    hash TEXT, difficulty INTEGER NOT NULL, crown INTEGER NOT NULL, rank INTEGER NOT NULL, score INTEGER NOT NULL,
    good INTEGER NOT NULL, ok INTEGER NOT NULL, bad INTEGER NOT NULL, drumroll INTEGER NOT NULL,
    max_combo INTEGER NOT NULL, played_at INTEGER NOT NULL DEFAULT 0, modifiers TEXT NOT NULL DEFAULT '{}');
PRAGMA user_version = 10;
"""


def make_game_db(path: Path, rows: list[tuple], players=((CODE_A, "Me"), ("2", "Player 2"))) -> bytes:
    con = sqlite3.connect(path)
    con.executescript(GAME_SCHEMA)
    con.executemany("INSERT INTO players(player_id, username, title) VALUES (?,?,'')", players)
    con.execute("INSERT INTO songs(hash_0, hash_3, title, subtitle) VALUES (?,?,?,?)", (H3, H5, "New Song", "sub"))
    con.executemany("""INSERT INTO scores(player_id, hash, difficulty, crown, rank, score, good, ok, bad, drumroll,
                       max_combo, played_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)""", rows)
    con.commit()
    con.close()
    return path.read_bytes()


def multipart(fields: dict[str, str], files: dict[str, tuple[str, bytes]]) -> tuple[str, bytes]:
    boundary = "----zdontest" + secrets.token_hex(8)
    out = b""
    for k, v in fields.items():
        out += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n").encode()
    for k, (fn, data) in files.items():
        out += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"; filename=\"{fn}\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\n").encode() + data + b"\r\n"
    out += f"--{boundary}--\r\n".encode()
    return f"multipart/form-data; boundary={boundary}", out


class MultipartTest(unittest.TestCase):
    def test_binary_and_fields(self):
        data = bytes(range(256)) * 50 + b"\r\n--not-the-boundary\r\n\r\n"
        ctype, body = multipart({"csrf": "x", "code": "1"}, {"db": ("scores.db", data)})
        parts = parse_multipart(ctype, body)
        self.assertEqual(parts["csrf"], (None, b"x"))
        self.assertEqual(parts["db"], ("scores.db", data))

    def test_cut_off_or_wrong(self):
        ctype, body = multipart({"csrf": "x"}, {"db": ("a.db", b"123")})
        self.assertIsNone(parse_multipart(ctype, body[:-10]))
        self.assertIsNone(parse_multipart("application/x-www-form-urlencoded", body))
        self.assertIsNone(parse_multipart(ctype, b"garbage" + body))


class ImportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        toml = root / "zdon_server.toml"
        toml.write_text(f'data_dir = "{(root / "data").as_posix()}"\n[site]\nsession_secret = "test-secret"\n',
                        encoding="utf-8")
        cfg = Z.load_config(toml)
        cls.store = Z.Store(cfg["data_dir"] / "zdon.db")
        api = Z.Api(cfg, cls.store)
        cls.site = Z.Site(cfg, cls.store, api.nonces)
        cls.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Z.make_handler(cfg, api, cls.site))
        cls.httpd.daemon_threads = True
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.httpd.server_address[1]}"
        now = int(time.time())
        for code, name in ((CODE_A, "Alice"), (CODE_B, "Bob")):
            cls.store.run("INSERT INTO users(access_code, username, created_at) VALUES (?,?,?)", (code, name, now))
        cls.store.run("INSERT INTO member_links VALUES (?,?,?)", (CODE_A, "uid-a", now))
        cls.store.run("INSERT INTO member_links VALUES (?,?,?)", (CODE_B, "uid-b", now))
        # already on the server for Alice: H1 crown 1 800k (better than the file), H2 crown 1 900k (file has a
        # better crown), H4 crown 2 700k (the file has the same)
        for h, crown, score in ((H1, 1, 800000), (H2, 1, 900000), (H4, 2, 700000)):
            cls.store.run("""INSERT INTO scores(access_code, hash, difficulty, score, crown, input_log, received_at)
                             VALUES (?,?,3,?,?,'[]',?)""", (CODE_A, h, score, crown, now))
        rows = [
            (CODE_A, H1, 3, 1, 5, 700000, 500, 10, 2, 3, 400, 1700000000),   # worse than here: skipped
            (CODE_A, H2, 3, 2, 6, 850000, 600, 0, 0, 1, 512, 1700000100),    # full combo beats a clear: imported
            (CODE_A, H3, 3, 1, 5, 600000, 400, 50, 9, 0, 300, 1700000200),   # new chart, two plays: best one
            (CODE_A, H3, 3, 1, 5, 650000, 420, 40, 8, 0, 310, 1700000300),
            (CODE_A, H4, 3, 2, 6, 700000, 500, 5, 0, 0, 505, 0),             # the same: skipped
            (CODE_A, H5, 4, 3, 7, 1000000, 800, 0, 0, 2, 800, 0),            # new on the ura course
            (CODE_A, H5, 9, 3, 7, 1000000, 800, 0, 0, 2, 800, 0),            # difficulty out of range: invalid
            (CODE_A, "", 3, 1, 5, 1, 1, 1, 1, 1, 1, 0),                      # no hash: invalid
            ("2", H1, 3, 3, 7, 1000000, 600, 0, 0, 0, 600, 0),              # another player in the file
        ]
        cls.game_db = make_game_db(root / "scores.db", rows)

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()
        cls.store.db.close()
        cls.tmp.cleanup()

    # -- helpers ---------------------------------------------------------------------------------------------------

    def cookie(self, uid: str) -> str:
        payload = S.b64(json.dumps({"uid": uid, "email": f"{uid}@test", "exp": time.time() + 3600}).encode())
        return f"zdon_session={payload}.{self.site._sign(payload)}"

    def csrf(self, uid: str) -> str:
        return self.site._csrf({"uid": uid})

    def request(self, path: str, uid: str | None, body: bytes | None = None, ctype: str = "") -> tuple[int, str]:
        req = urllib.request.Request(self.base + path, data=body, method="POST" if body is not None else "GET")
        if uid:
            req.add_header("Cookie", self.cookie(uid))
        if ctype:
            req.add_header("Content-Type", ctype)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8")

    def upload(self, uid: str, data: bytes, code: str = CODE_A, csrf: str | None = None) -> tuple[int, str]:
        ctype, body = multipart({"csrf": csrf if csrf is not None else self.csrf(uid), "code": code},
                                {"db": ("scores.db", data)})
        return self.request("/me/import/upload", uid, body, ctype)

    def run_import(self, uid: str, token: str, player: str, code: str = CODE_A) -> tuple[int, str]:
        body = urllib.parse.urlencode({"csrf": self.csrf(uid), "code": code, "token": token,
                                       "player_id": player}).encode()
        return self.request("/me/import/run", uid, body, "application/x-www-form-urlencoded")

    @staticmethod
    def token(page: str) -> str:
        return re.search(r'name="token" value="([^"]+)"', page).group(1)

    def best(self, h: str, d: int = 3) -> tuple[int, int]:
        r = self.store.one("""SELECT crown, score FROM scores WHERE access_code=? AND hash=? AND difficulty=?
                              ORDER BY crown DESC, score DESC LIMIT 1""", (CODE_A, h, d))
        return (r["crown"], r["score"]) if r else None

    def uploads_left(self) -> list[Path]:
        return list((Path(self.tmp.name) / "data" / "score_uploads").glob("*.*"))

    # -- tests -----------------------------------------------------------------------------------------------------

    def test_1_import_keeps_best(self):
        status, page = self.request("/me", "uid-a")
        self.assertIn("/me/import?code=", page)
        status, page = self.request(f"/me/import?code={CODE_A}", "uid-a")
        self.assertEqual(status, 200)
        self.assertIn('enctype="multipart/form-data"', page)

        status, page = self.upload("uid-a", self.game_db)
        self.assertEqual(status, 200, page)
        self.assertIn("Player 2", page)
        # the account's own player is picked
        self.assertRegex(page, r'value="%s" checked' % CODE_A)
        token = self.token(page)
        self.assertEqual(len(self.uploads_left()), 1)

        status, page = self.run_import("uid-a", token, CODE_A)
        self.assertEqual(status, 200, page)
        text = html.unescape(re.sub(r"<[^>]+>", " ", page))
        self.assertRegex(text, r"匯入\s+3\b")
        self.assertRegex(text, r"相同的）\s+2\b")
        self.assertRegex(text, r"資料不完整）\s+2\b")
        self.assertIn("New Song", text)
        self.assertEqual(self.uploads_left(), [])

        self.assertEqual(self.best(H1), (1, 800000))        # not covered by the worse play
        self.assertEqual(self.best(H2), (2, 850000))        # the better crown
        self.assertEqual(self.best(H3), (1, 650000))        # best of the two
        self.assertEqual(self.best(H4), (2, 700000))
        self.assertEqual(self.best(H5, 4), (3, 1000000))
        self.assertEqual(self.store.one("SELECT COUNT(*) AS n FROM scores WHERE access_code=? AND hash=?",
                                        (CODE_A, H4))["n"], 1)
        r = self.store.one("SELECT played_at, received_at, input_log, good FROM scores WHERE access_code=? AND hash=?",
                           (CODE_A, H3))
        self.assertEqual((r["played_at"], r["received_at"], r["input_log"], r["good"]), (1700000300, 1700000300, "[]", 420))
        self.assertEqual(self.store.one("SELECT title FROM songs WHERE hash=?", (H5,))["title"], "New Song")
        self.assertEqual(self.store.one("SELECT COUNT(*) AS n FROM scores WHERE access_code=?", (CODE_B,))["n"], 0)
        log = self.store.one("SELECT * FROM score_imports ORDER BY id DESC LIMIT 1")
        self.assertEqual((log["added"], log["not_better"], log["invalid"]), (3, 2, 2))

        # the same file again adds nothing
        status, page = self.upload("uid-a", self.game_db)
        status, page = self.run_import("uid-a", self.token(page), CODE_A)
        self.assertRegex(html.unescape(re.sub(r"<[^>]+>", " ", page)), r"匯入\s+0\b")

    def test_2_other_player_in_file(self):
        status, page = self.upload("uid-a", self.game_db)
        status, page = self.run_import("uid-a", self.token(page), "2")
        self.assertEqual(self.best(H1), (3, 1000000))
        status, page = self.upload("uid-a", self.game_db)
        status, page = self.run_import("uid-a", self.token(page), "nobody")
        self.assertIn("請選檔案裡的一位玩家", page)

    def test_3_refusals(self):
        before = self.store.one("SELECT COUNT(*) AS n FROM scores")["n"]
        # not logged in
        status, _ = self.upload("", self.game_db, csrf="x")
        self.assertEqual(status, 403)
        # wrong csrf
        status, _ = self.upload("uid-a", self.game_db, csrf="bad")
        self.assertEqual(status, 403)
        # someone else's account
        status, page = self.upload("uid-b", self.game_db, code=CODE_A)
        self.assertIn("找不到這個遊戲帳號", page)
        # not SQLite
        status, page = self.upload("uid-a", b"hello" * 100)
        self.assertIn("不是 scores.db", page)
        # SQLite, but scores is a view
        p = Path(self.tmp.name) / "view.db"
        con = sqlite3.connect(p)
        con.executescript("CREATE TABLE players(player_id TEXT, username TEXT); CREATE TABLE t(x);"
                          "CREATE VIEW scores AS SELECT 1 AS player_id, 'a' AS hash, 3 AS difficulty, 1 AS crown, "
                          "1 AS score FROM t;")
        con.close()
        status, page = self.upload("uid-a", p.read_bytes())
        self.assertIn("沒有遊戲的成績表", page)
        # a token from member A doesn't work for member B
        status, page = self.upload("uid-a", self.game_db)
        token = self.token(page)
        status, page = self.run_import("uid-b", token, CODE_A, code=CODE_B)
        self.assertIn("過期", page)
        # too big
        old = self.site.upload_max
        self.site.upload_max = 1024
        try:
            status, _ = self.upload("uid-a", self.game_db)          # the file is over the limit
            self.assertEqual(status, 413)
            status, _ = self.upload("uid-a", self.game_db * 4)      # the request is: its body stays unread
            self.assertEqual(status, 413)
        finally:
            self.site.upload_max = old
        self.assertEqual(self.store.one("SELECT COUNT(*) AS n FROM scores")["n"], before)
        # the server still answers after the unread body
        status, _ = self.request("/me/import", "uid-a")
        self.assertEqual(status, 200)


if __name__ == "__main__":
    unittest.main()
