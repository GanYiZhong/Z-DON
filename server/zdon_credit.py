"""Credit for online battles (the user's rules 2026-11-11), kept in zdon.db:

- everyone starts at 100 (also the most); finishing a whole match (best of three) gives +2
- leaving a match before it is decided (leaving the room, closing the game, a dropped connection) costs 5;
  surrendering a song costs nothing
- a report needs a reason; it costs the reported player 5 only once an admin confirms it on the site (/admin);
  a report the admin finds malicious costs the reporter 5; one report per player against the same player a day
- under 80: no quick match (room codes only); under 60: no online battle at all
"""
from __future__ import annotations

import json
import time

START = 100
MOST = 100
MATCH_BONUS = 2
LEAVE_COST = 5
REPORT_COST = 5
MALICIOUS_COST = 5
QUICK_MIN = 80
ONLINE_MIN = 60

SCHEMA = """
CREATE TABLE IF NOT EXISTS credit (
    access_code TEXT PRIMARY KEY, score INTEGER NOT NULL, updated INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS credit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT, access_code TEXT NOT NULL, delta INTEGER NOT NULL, reason TEXT NOT NULL,
    at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS reports (
    id INTEGER PRIMARY KEY AUTOINCREMENT, reporter TEXT NOT NULL, reported TEXT NOT NULL, reason TEXT NOT NULL,
    context TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'pending',
    resolved_at INTEGER, resolved_by TEXT
);
CREATE INDEX IF NOT EXISTS reports_by_status ON reports(status);
"""


class Credit:
    def __init__(self, store):
        self.s = store
        with store.lock:
            store.db.executescript(SCHEMA)

    def get(self, code: str) -> int:
        row = self.s.one("SELECT score FROM credit WHERE access_code=?", (code,))
        return int(row["score"]) if row else START

    def add(self, code: str, delta: int, reason: str) -> int:
        with self.s.lock:
            score = max(0, min(MOST, self.get(code) + delta))
            now = int(time.time())
            self.s.run("INSERT INTO credit(access_code, score, updated) VALUES (?,?,?) "
                       "ON CONFLICT(access_code) DO UPDATE SET score=excluded.score, updated=excluded.updated",
                       (code, score, now))
            self.s.run("INSERT INTO credit_log(access_code, delta, reason, at) VALUES (?,?,?,?)",
                       (code, delta, reason, now))
            return score

    # ------------------------------------------------------------------------------------------------------------
    def report(self, reporter: str, reported: str, reason: str, context: dict) -> int | None:
        """A new report (pending), or None if this reporter already reported this player today."""
        day_ago = int(time.time()) - 86400
        with self.s.lock:
            if self.s.one("SELECT 1 FROM reports WHERE reporter=? AND reported=? AND created>?",
                          (reporter, reported, day_ago)):
                return None
            return self.s.run("INSERT INTO reports(reporter, reported, reason, context, created) VALUES (?,?,?,?,?)",
                              (reporter, reported, reason, json.dumps(context, ensure_ascii=False),
                               int(time.time())))

    def pending(self) -> list:
        return self.s.q("SELECT * FROM reports WHERE status='pending' ORDER BY id")

    def recent(self, limit: int = 30) -> list:
        return self.s.q("SELECT * FROM reports WHERE status!='pending' ORDER BY resolved_at DESC LIMIT ?", (limit,))

    def resolve(self, report_id: int, verdict: str, admin: str) -> bool:
        """verdict: accepted (the reported player loses 5), rejected, malicious (the reporter loses 5)."""
        if verdict not in ("accepted", "rejected", "malicious"):
            return False
        with self.s.lock:
            row = self.s.one("SELECT * FROM reports WHERE id=? AND status='pending'", (report_id,))
            if not row:
                return False
            self.s.run("UPDATE reports SET status=?, resolved_at=?, resolved_by=? WHERE id=?",
                       (verdict, int(time.time()), admin, report_id))
            if verdict == "accepted":
                self.add(row["reported"], -REPORT_COST, f"report #{report_id} confirmed")
            elif verdict == "malicious":
                self.add(row["reporter"], -MALICIOUS_COST, f"report #{report_id} found malicious")
            return True
