"""Tests for the nameplate's dan on the site and the server (users.plate_dan / plate_gold / plate_rainbow).

    cd server && python -m unittest test_zdon_plate_dan -v

On the scores.db test's server (a temporary data folder; never zdon_data or zdon_server.toml).
"""
from __future__ import annotations

import json
import sqlite3
import sys
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_zdon_score_import as base  # noqa: E402

CODE_B = base.CODE_B


class PlateDanTest(base.ImportTest):
    test_1_import_keeps_best = test_2_other_player_in_file = test_3_refusals = None

    def api(self, path: str, params: dict) -> tuple[int, dict]:
        status, body = self.request(path, None, urllib.parse.urlencode(params).encode(),
                                    "application/x-www-form-urlencoded")
        return status, json.loads(body)

    def plate(self, code: str) -> tuple[int, int, int]:
        r = self.store.one("SELECT plate_dan, plate_gold, plate_rainbow FROM users WHERE access_code=?", (code,))
        return r["plate_dan"], r["plate_gold"], r["plate_rainbow"]

    def dan(self, code: str, index: int, odai: int, gaiden: bool = False) -> None:
        status, _ = self.api("/submit_dan", {"access_code": code, "dan_index": index, "odai_result": odai,
                                             "is_gaiden": "true" if gaiden else "false", "title": "x", "score": 1})
        self.assertEqual(status, 200)

    def test_1_dan_results_raise_it(self):
        self.assertEqual(self.plate(CODE_B), (-1, 0, 0))
        user = json.loads(self.request("/user?access_code=" + CODE_B, None)[1])
        self.assertNotIn("dan", user)
        self.dan(CODE_B, 7, 0)                 # failed: nothing
        self.assertEqual(self.plate(CODE_B), (-1, 0, 0))
        self.dan(CODE_B, 7, 2)                 # a red pass
        self.assertEqual(self.plate(CODE_B), (7, 0, 0))
        self.dan(CODE_B, 7, 6)                 # the same dan, all perfect: gold and rainbow added
        self.assertEqual(self.plate(CODE_B), (7, 1, 1))
        self.dan(CODE_B, 6, 2)                 # lower: nothing
        self.dan(CODE_B, 9, 2, gaiden=True)    # gaiden: nothing
        self.assertEqual(self.plate(CODE_B), (7, 1, 1))
        self.dan(CODE_B, 8, 3)                 # higher: replaces gold and rainbow
        self.assertEqual(self.plate(CODE_B), (8, 0, 0))
        user = json.loads(self.request("/user?access_code=" + CODE_B, None)[1])
        self.assertEqual((user["dan"], user["gold"], user["rainbow"]), (8, False, False))

    def test_2_import_carries_it(self):
        p = Path(self.tmp.name) / "dan.db"
        con = sqlite3.connect(p)
        con.executescript(base.GAME_SCHEMA.replace("title TEXT NOT NULL);",
                                                   "title TEXT NOT NULL, dan INTEGER NOT NULL DEFAULT -1,"
                                                   " gold BOOL NOT NULL DEFAULT 0, rainbow BOOL NOT NULL DEFAULT 0);"))
        con.execute("INSERT INTO players(player_id, username, title, dan, gold, rainbow) VALUES (?,?,?,?,?,?)",
                    (base.CODE_A, "Me", "", 5, 1, 1))
        con.execute("""INSERT INTO scores(player_id, hash, difficulty, crown, rank, score, good, ok, bad, drumroll,
                       max_combo) VALUES (?,?,3,1,1,500000,1,1,1,1,1)""", (base.CODE_A, "9" * 32))
        con.commit()
        con.close()
        status, page = self.upload("uid-a", p.read_bytes())
        status, page = self.run_import("uid-a", self.token(page), base.CODE_A)
        self.assertEqual(status, 200)
        self.assertEqual(self.plate(base.CODE_A), (5, 1, 1))

        # my page draws it: the emblem (gold), the rainbow row, the name beside the emblem
        status, page = self.request("/me", "uid-a")
        self.assertIn("/cosm/plate/dan_emblem_gold.png", page)
        self.assertIn("background-position:-64px -64px", page)       # dan 5: column 1, row 2
        self.assertIn('class="ft rb"', page)
        self.assertIn('data-cx="191.75" data-max="121.5"', page)       # nameplate.lua's name area beside a dan


if __name__ == "__main__":
    import unittest
    unittest.main()
