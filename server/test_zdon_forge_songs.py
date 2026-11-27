"""Tests for the game's TaikoForge charts routes (zdon_game_login.py GET /forge_songs, /forge_song) and the web
version's /forge/songs, both through zdon_site.Site.forge_partner, against a fake TaikoForge partner API.

    cd server && python -m unittest test_zdon_forge_songs -v

Temporary data folder, local ports only; never zdon_data, zdon_server.toml or forge.taiko.ac.
"""
from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zdon_server as Z  # noqa: E402
from zdon_game_login import GameLogin  # noqa: E402

SONG = "0123456789abcdef0123456789abcdef"
SEEN: list[tuple[str, dict]] = []


class FakeForge(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        SEEN.append((url.path, {"uid": q.get("uid"), "sig": bool(self.headers.get("X-TF-Sig"))}))
        if q.get("uid") != "uid-1":
            self.send_response(404)
            self.end_headers()
            return
        if url.path == "/partner/zdon/songs":
            body = json.dumps({"songs": [{"id": SONG, "title": "My Chart"}], "paid": True, "retention_days": 7}).encode()
            ctype = "application/json"
        elif url.path == f"/partner/zdon/songs/{SONG}/tja":
            body, ctype = "TITLE:My Chart\nWAVE:x.ogg\nCOURSE:Oni\n#START\n1,\n#END\n".encode(), "text/plain"
        elif url.path == f"/partner/zdon/songs/{SONG}/audio":
            body, ctype = b"OggS" + b"\0" * 100, "audio/ogg"
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class ForgeSongsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.forge = ThreadingHTTPServer(("127.0.0.1", 0), FakeForge)
        threading.Thread(target=cls.forge.serve_forever, daemon=True).start()
        forge_url = f"http://127.0.0.1:{cls.forge.server_address[1]}"
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        toml = root / "zdon_server.toml"
        toml.write_text(f'data_dir = "{(root / "data").as_posix()}"\n[site]\nsession_secret = "test-secret"\n'
                        f'sso_url = "{forge_url}/sso/zdon"\nsso_secret = "sso-test"\nforge_url = "{forge_url}"\n',
                        encoding="utf-8")
        cfg = Z.load_config(toml)
        cls.store = Z.Store(cfg["data_dir"] / "zdon.db")
        api = Z.Api(cfg, cls.store)
        cls.site = Z.Site(cfg, cls.store, api.nonces)
        cls.login = GameLogin(cfg, cls.store, cls.site)
        api.add_game_login(cls.login)
        cls.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Z.make_handler(cfg, api, cls.site))
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.httpd.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        for s in (cls.httpd, cls.forge):
            s.shutdown()
            s.server_close()
        cls.store.db.close()
        cls.tmp.cleanup()

    def get(self, path: str, params: dict) -> tuple[int, bytes, str]:
        try:
            with urllib.request.urlopen(f"{self.base}{path}?{urllib.parse.urlencode(params)}", timeout=10) as r:
                return r.status, r.read(), r.headers.get("Content-Type", "")
        except urllib.error.HTTPError as e:
            return e.code, e.read(), ""

    def test_list_and_files(self):
        token = self.login._token("uid-1", "a@test")
        status, body, _ = self.get("/forge_songs", {"token": token})
        self.assertEqual(status, 200, body)
        d = json.loads(body)
        self.assertEqual((d["songs"][0]["id"], d["paid"], d["retention_days"]), (SONG, True, 7))
        self.assertEqual(SEEN[-1], ("/partner/zdon/songs", {"uid": "uid-1", "sig": True}))
        status, body, ctype = self.get("/forge_song", {"token": token, "id": SONG, "file": "tja"})
        self.assertEqual(status, 200)
        self.assertIn(b"WAVE:x.ogg", body)
        status, body, ctype = self.get("/forge_song", {"token": token, "id": SONG, "file": "audio"})
        self.assertEqual((status, body[:4], ctype), (200, b"OggS", "application/octet-stream"))

    def test_refusals(self):
        token = self.login._token("uid-1", "a@test")
        self.assertEqual(self.get("/forge_songs", {"token": "bad.token"})[0], 401)
        self.assertEqual(self.get("/forge_songs", {})[0], 401)
        self.assertEqual(self.get("/forge_song", {"token": token, "id": "../x", "file": "tja"})[0], 404)
        self.assertEqual(self.get("/forge_song", {"token": token, "id": SONG, "file": "other"})[0], 404)
        # TaikoForge says no (another member's uid): passed on
        other = self.login._token("uid-2", "b@test")
        self.assertEqual(self.get("/forge_songs", {"token": other})[0], 404)
        self.assertEqual(self.get("/forge_song", {"token": other, "id": SONG, "file": "audio"})[0], 404)


if __name__ == "__main__":
    unittest.main()
