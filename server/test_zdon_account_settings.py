"""The account's play options and song lists (POST /update_settings, /update_song_list; read back in GET /user).

    cd server && python -m unittest test_zdon_account_settings -v

Temporary data folder only; never zdon_data or zdon_server.toml.
"""
from __future__ import annotations

import json
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zdon_server as Z  # noqa: E402

CODE = "11111111111111111111"


class AccountSettingsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        toml = root / "zdon_server.toml"
        toml.write_text(f'data_dir = "{(root / "data").as_posix()}"\n', encoding="utf-8")
        cfg = Z.load_config(toml)
        self.store = Z.Store(cfg["data_dir"] / "zdon.db")
        self.api = Z.Api(cfg, self.store)
        self.store.run("INSERT INTO users(access_code, username, idm, created_at) VALUES (?,?,?,?)",
                       (CODE, "Main", None, int(time.time())))

    def tearDown(self):
        self.store.db.close()
        self.tmp.cleanup()

    def test_settings_round_trip(self):
        self.assertEqual(self.api.update_settings({"access_code": CODE, "key": "modifier_speed", "value": "3"})[0], 200)
        self.assertEqual(self.api.update_settings({"access_code": CODE, "key": "modifier_auto", "value": "1"})[0], 200)
        self.assertEqual(self.api.update_settings({"access_code": CODE, "key": "modifier_speed", "value": "5"})[0], 200)
        status, out = self.api.user({"access_code": CODE})
        self.assertEqual(status, 200)
        self.assertEqual(out["settings"], {"modifier_speed": "5", "modifier_auto": "1"})

    def test_settings_refusals(self):
        self.assertEqual(self.api.update_settings({"access_code": "999", "key": "modifier_auto", "value": "1"})[0], 404)
        self.assertEqual(self.api.update_settings({"access_code": CODE, "key": "Bad Key", "value": "1"})[0], 400)
        self.assertEqual(self.api.update_settings({"access_code": CODE, "key": "k", "value": "x" * 300})[0], 400)
        for i in range(Z.Api.MAX_SETTINGS):
            self.assertEqual(self.api.update_settings({"access_code": CODE, "key": f"k{i}", "value": "1"})[0], 200)
        self.assertEqual(self.api.update_settings({"access_code": CODE, "key": "one_more", "value": "1"})[0], 400)
        self.assertEqual(self.api.update_settings({"access_code": CODE, "key": "k0", "value": "2"})[0], 200)

    def test_song_lists(self):
        fav = ["a" * 32, "b" * 32]
        self.assertEqual(self.api.update_song_list({"access_code": CODE, "kind": "favorites",
                                                    "hashes": json.dumps(fav)})[0], 200)
        self.assertEqual(self.api.update_song_list({"access_code": CODE, "kind": "recent", "hashes": "[]"})[0], 200)
        out = self.api.user({"access_code": CODE})[1]
        self.assertEqual((out["favorites"], out["recent"]), (fav, []))
        self.assertEqual(self.api.update_song_list({"access_code": CODE, "kind": "other", "hashes": "[]"})[0], 400)
        self.assertEqual(self.api.update_song_list({"access_code": CODE, "kind": "recent", "hashes": "{"})[0], 400)
        self.assertEqual(self.api.update_song_list({"access_code": CODE, "kind": "recent", "hashes": "[1]"})[0], 400)

    def test_user_without_any(self):
        out = self.api.user({"access_code": CODE})[1]
        self.assertEqual(out["settings"], {})
        self.assertNotIn("favorites", out)


if __name__ == "__main__":
    unittest.main()
