"""Tests for the Donder Hiroba import on my page (zdon_hiroba.py, zdon_site_import.py).

    cd server && python -m unittest test_zdon_hiroba_import -v

A whole server on a random local port with a temporary data folder and a small song manifest; never touches
zdon_data or zdon_server.toml.
"""
from __future__ import annotations

import base64
import html
import json
import re
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zdon_hiroba as H  # noqa: E402
import test_zdon_score_import as base  # noqa: E402
from test_zdon_score_import import CODE_A  # noqa: E402

# a small ESE: "Foo‐Bar" (Hiroba writes U+2010), "Twice" in two charts, one main (folder name), "Many" in two
# side versions, "NoUra" without an ura course
MANIFEST = {
    "stubs": {
        "01 Pop/Foo Bar/Foo Bar.tja": "TITLE:Foo-Bar\nTITLEJA:ふー－ばー\nSUBTITLE:--x\nCOURSE:Oni\n#START\nTITLE:no\n#END\n",
        "01 Pop/Twice/Twice.tja": "TITLE:Twice\n",
        "01 Pop/Twice/Twice Old.tja": "TITLE:Twice\n",
        "01 Pop/Many/Many A.tja": "TITLE:Many\n",
        "01 Pop/Many/Many B.tja": "TITLE:Many\n",
        "01 Pop/NoUra/NoUra.tja": "TITLEJA:のうら\n",
    },
    "hashes": {
        "01 Pop/Foo Bar/Foo Bar.tja": ["f0" * 16, "f1" * 16, "f2" * 16, "f3" * 16, "f4" * 16],
        "01 Pop/Twice/Twice.tja": ["", "", "", "73" * 16, ""],
        "01 Pop/Twice/Twice Old.tja": ["", "", "", "70" * 16, ""],
        "01 Pop/Many/Many A.tja": ["", "", "", "a3" * 16, ""],
        "01 Pop/Many/Many B.tja": ["", "", "", "b3" * 16, ""],
        "01 Pop/NoUra/NoUra.tja": ["", "", "", "c3" * 16, ""],
    },
}
FOO_ONI = "f3" * 16


def payload_doc() -> dict:
    return {"taikoProfile": {"nickname": "どん", "taikoNo": "123456789012"},
            "clearData": [{"title": "ｆｏｏ‐ｂａｒ", "songNo": "1", "difficulty": {"oni": {"crown": "gold"}}}],
            "scoreData": {
                "1": {"title": "Foo‐Bar", "songNo": "1", "difficulty": {
                    "oni": {"crown": "gold", "badge": None, "score": 955000, "good": 800, "ok": 10, "bad": 0,
                            "maxCombo": 810, "roll": 30, "count": {"play": 3}},
                    "ura": {"crown": "silver", "badge": "pink", "score": 905000, "good": 600, "ok": 50, "bad": 3,
                            "maxCombo": 300, "roll": 0}}},
                "2": {"title": "Twice", "songNo": "2", "difficulty": {"oni": {"crown": "donderfull", "score": 1001000}}},
                "3": {"title": "Many", "songNo": "3", "difficulty": {"oni": {"crown": "gold", "score": 900000}}},
                "4": {"title": "Nothing Here", "songNo": "4", "difficulty": {"oni": {"crown": "gold", "score": 1}}},
                "5": {"title": "のうら", "songNo": "5", "difficulty": {"ura": {"crown": "gold", "score": 2}}},
                "6": {"title": "Bad", "songNo": "6", "difficulty": {"oni": {"crown": "rainbowish", "score": 5}}},
            }}


def payload_text(doc: dict) -> bytes:
    # lzutf8 leaves UTF-8 text as it is (literals), so plain JSON is a valid stream
    return base64.b64encode(json.dumps(doc, ensure_ascii=False).encode()).decode().encode()


class HirobaUnitTest(unittest.TestCase):
    def test_lzutf8_pointer(self):
        self.assertEqual(H.lzutf8_decompress(b"abc\xc3\x03"), b"abcabc")
        self.assertEqual(H.lzutf8_decompress("日本".encode()), "日本".encode())
        with self.assertRaises(H.BadHiroba):
            H.lzutf8_decompress(b"\xc3\x09")

    def test_formats(self):
        doc = payload_doc()
        for data in (payload_text(doc), json.dumps(doc).encode()):
            who, plays = H.charts(H.load(data))
            self.assertEqual(who, {"name": "どん", "taiko_no": "123456789012"})
            self.assertEqual(len(plays), 6)          # "Bad" has an unknown crown
        scrape = {"1": {"title": "Foo‐Bar", "levels": {"4": {"crown": 2, "score": 955000, "good": 1}, "2": {"crown": None}}}}
        for doc in (scrape, {"taikoNo": "1", "scores": scrape}):
            who, plays = H.charts(H.load(json.dumps(doc).encode()))
            self.assertEqual([(p["course"], p["crown"], p["score"]) for p in plays], [(3, 2, 955000)])
        for bad in (b"not base64 !!", b"[1,2]", b"{oops"):
            with self.assertRaises(H.BadHiroba):
                H.charts(H.load(bad))

    def test_rank(self):
        self.assertEqual([H.rank_for_score(s) for s in (0, 600000, 799999, 950000, 1000000)], [1, 2, 3, 6, 7])


class HirobaImportTest(base.ImportTest):
    """Runs on the scores.db test's server (setUpClass), with a song manifest added."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        songs = Path(cls.tmp.name) / "data" / "songs"
        songs.mkdir(parents=True, exist_ok=True)
        (songs / "song_manifest.json").write_text(json.dumps(MANIFEST, ensure_ascii=False), encoding="utf-8")
        cls.store.run("""INSERT INTO scores(access_code, hash, difficulty, score, crown, input_log, received_at)
                         VALUES (?,?,3,?,?,'[]',?)""", (CODE_A, "73" * 16, 1002000, 3, int(time.time())))

    # the scores.db tests run once, in ImportTest
    test_1_import_keeps_best = test_2_other_player_in_file = test_3_refusals = None

    def run_hiroba(self, token: str) -> tuple[int, str]:
        import urllib.parse
        body = urllib.parse.urlencode({"csrf": self.csrf("uid-a"), "code": CODE_A, "token": token,
                                       "kind": "hiroba"}).encode()
        return self.request("/me/import/run", "uid-a", body, "application/x-www-form-urlencoded")

    def test_hiroba(self):
        status, page = self.upload("uid-a", payload_text(payload_doc()))
        self.assertEqual(status, 200, page)
        text = html.unescape(re.sub(r"<[^>]+>", " ", page))
        self.assertIn("太鼓番 123456789012", text)
        self.assertRegex(text, r"對得上的歌\s+3\b")            # Foo-Bar, Twice (main chart), のうら
        self.assertRegex(text, r"可匯入的譜面\s+3\b")          # Foo oni + ura, Twice oni (のうら has no ura)
        self.assertIn("Many", text)
        self.assertIn("Nothing Here", text)
        token = self.token(page)
        self.assertEqual(len(self.uploads_left()), 1)

        status, page = self.run_hiroba(token)
        self.assertEqual(status, 200, page)
        text = html.unescape(re.sub(r"<[^>]+>", " ", page))
        self.assertRegex(text, r"匯入\s+2\b")
        self.assertRegex(text, r"相同的）\s+1\b")              # Twice: 1,002,000 DFC here beats 1,001,000
        self.assertEqual(self.uploads_left(), [])
        r = self.store.one("SELECT * FROM scores WHERE access_code=? AND hash=?", (CODE_A, FOO_ONI))
        self.assertEqual((r["difficulty"], r["crown"], r["score"], r["rank"], r["good"], r["drumroll"], r["max_combo"]),
                         (3, 2, 955000, 6, 800, 30, 810))
        self.assertIsNone(r["played_at"])
        self.assertEqual(self.store.one("SELECT title FROM songs WHERE hash=?", (FOO_ONI,))["title"], "Foo-Bar")
        self.assertEqual(self.store.one("SELECT COUNT(*) AS n FROM scores WHERE hash=?", ("70" * 16,))["n"], 0)

        # again: nothing new
        status, page = self.upload("uid-a", payload_text(payload_doc()))
        status, page = self.run_hiroba(self.token(page))
        self.assertRegex(html.unescape(re.sub(r"<[^>]+>", " ", page)), r"匯入\s+0\b")

    def test_hiroba_refusals(self):
        status, page = self.upload("uid-a", b"just some text")
        self.assertIn("不是 scores.db", page)
        status, page = self.upload("uid-a", json.dumps({"scoreData": {}}).encode())
        self.assertIn("找不到 Hiroba 的成績", page)
        # a scores.db token can't be run as a Hiroba one
        status, page = self.upload("uid-a", self.game_db)
        status, page = self.run_hiroba(self.token(page))
        self.assertIn("過期", page)


if __name__ == "__main__":
    unittest.main()
