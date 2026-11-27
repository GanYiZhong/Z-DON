"""Which game account a TaikoForge login plays as (zdon_game_login.GameLogin._pick_account).

    cd server && python -m unittest test_zdon_game_login_accounts -v

Temporary data folder only; never zdon_data or zdon_server.toml.
"""
from __future__ import annotations

import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zdon_server as Z  # noqa: E402
from zdon_game_login import GameLogin  # noqa: E402


class PickAccountTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        toml = root / "zdon_server.toml"
        toml.write_text(f'data_dir = "{(root / "data").as_posix()}"\n[site]\nsession_secret = "test-secret"\n',
                        encoding="utf-8")
        cfg = Z.load_config(toml)
        self.store = Z.Store(cfg["data_dir"] / "zdon.db")
        api = Z.Api(cfg, self.store)
        site = Z.Site(cfg, self.store, api.nonces)
        self.login = GameLogin(cfg, self.store, site)

    def tearDown(self):
        self.store.db.close()
        self.tmp.cleanup()

    def user(self, code: str, name: str, linked_to: str | None = None, at: int = 0):
        self.store.run("INSERT INTO users(access_code, username, idm, created_at) VALUES (?,?,?,?)",
                       (code, name, None, int(time.time())))
        if linked_to:
            self.store.run("INSERT INTO member_links VALUES (?,?,?)", (code, linked_to, at))

    def links(self, uid: str) -> list[str]:
        return [r["access_code"] for r in self.store.q(
            "SELECT access_code FROM member_links WHERE forge_uid=? ORDER BY linked_at", (uid,))]

    def test_member_with_an_account_gets_it_on_another_install(self):
        self.user("11111111111111111111", "Main", "uid-1", at=1)
        self.user("22222222222222222222", "Fresh")  # another copy of the game, nobody's
        got = self.login._pick_account("uid-1", "22222222222222222222", "Fresh")
        self.assertEqual(got["access_code"], "11111111111111111111")
        self.assertEqual(self.links("uid-1"), ["11111111111111111111"])  # the fresh code is not linked

    def test_own_second_account_is_kept(self):
        self.user("11111111111111111111", "Main", "uid-1", at=1)
        self.user("33333333333333333333", "Second", "uid-1", at=2)
        got = self.login._pick_account("uid-1", "33333333333333333333", "Second")
        self.assertEqual(got["access_code"], "33333333333333333333")

    def test_someone_elses_code_switches_to_the_members(self):
        self.user("11111111111111111111", "Main", "uid-1", at=1)
        self.user("44444444444444444444", "Other", "uid-2", at=1)
        got = self.login._pick_account("uid-1", "44444444444444444444", "Other")
        self.assertEqual(got["access_code"], "11111111111111111111")

    def test_first_login_links_the_games_code(self):
        self.user("55555555555555555555", "Local")
        got = self.login._pick_account("uid-1", "55555555555555555555", "Local")
        self.assertEqual(got["access_code"], "55555555555555555555")
        self.assertEqual(self.links("uid-1"), ["55555555555555555555"])

    def test_first_login_with_a_placeholder_gets_a_new_account(self):
        got = self.login._pick_account("uid-1", "0", "Don")
        self.assertEqual(len(got["access_code"]), 20)
        self.assertEqual(self.links("uid-1"), [got["access_code"]])


if __name__ == "__main__":
    unittest.main()
