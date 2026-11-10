"""Live online battles for zdon_server.py: rooms (a 4-digit code, or quick match), the song both play, and the
hits each side streams to the other while playing. Kept in memory: a server restart ends the rooms.

Flow (every call carries the player's access_code; the game signs them like the rest of the API):
    POST /match/create            -> {code}          a room for a friend (the creator is the host)
    POST /match/quick             -> {code}          joins a waiting quick room, else opens one
    POST /match/join   code       -> {code}          the guest
    POST /match/leave  code
    GET  /match/state  code       -> the room as this player sees it (polled in the lobby)
    POST /match/song   code hash title difficulty mods    the host's pick (a new round after a finished one)
    POST /match/ready  code has_song difficulty mods      the guest: has the chart? which course?
    POST /match/sync   code from hits pos score combo since
                                  -> the opponent's hits from `since`, its position and score (~10 a second in play)
    POST /match/finish code score good ok bad drumroll max_combo
The round starts when the guest is ready (the host is ready by picking); each game starts on its own and the
opponent's lane follows the opponent's reported position, a moment behind, so the two needn't start together.
"""
from __future__ import annotations

import secrets
import threading
import time

ROOM_IDLE_S = 30.0      # a player not heard from for this long has left
ROOM_MAX_HITS = 20000   # per player per round


def now() -> float:
    return time.monotonic()


class Side:
    def __init__(self, code: str, profile: dict):
        self.code = code
        self.profile = profile          # username, title, colours: what the opponent's lane shows
        self.seen = now()
        self.reset_round()

    def reset_round(self):
        self.has_song = None            # guest: None = not answered yet
        self.difficulty = None
        self.mods = ""                  # the play's chart options (JSON: inverse / random / seed / subdiff)
        self.ready = False
        self.hits: list[str] = []       # "ms:type"
        self.pos = None                 # chart ms when last synced
        self.score = 0
        self.combo = 0
        self.final = None               # dict once finished


class Room:
    def __init__(self, code: str, quick: bool, host: Side):
        self.code = code
        self.quick = quick
        self.host = host
        self.guest: Side | None = None
        self.state = "waiting"          # waiting (no guest) / choosing / song / playing / finished
        self.song: dict | None = None   # hash, title, difficulty (the host's)
        self.round = 0

    def side(self, access_code: str) -> tuple[Side | None, Side | None]:
        if self.host.code == access_code:
            return self.host, self.guest
        if self.guest and self.guest.code == access_code:
            return self.guest, self.host
        return None, None


class Matches:
    def __init__(self, profile_of):
        self.rooms: dict[str, Room] = {}
        self.lock = threading.Lock()
        self.profile_of = profile_of    # access_code -> dict | None (the users row)

    # ------------------------------------------------------------------------------------------------------------
    def _expire(self):
        t = now()
        for code in list(self.rooms):
            r = self.rooms[code]
            if r.guest and t - r.guest.seen > ROOM_IDLE_S:
                self._drop(r, r.guest.code)
            if code in self.rooms and t - r.host.seen > ROOM_IDLE_S:
                self._drop(r, r.host.code)

    def _drop(self, r: Room, access_code: str):
        if r.guest and r.guest.code == access_code:
            r.guest = None
        elif r.host.code == access_code:
            if r.guest:                 # the guest takes the room over
                r.host, r.guest = r.guest, None
            else:
                self.rooms.pop(r.code, None)
                return
        r.state = "waiting"
        r.song = None
        r.host.reset_round()

    def _new_code(self) -> str:
        while True:
            code = f"{secrets.randbelow(9000) + 1000}"
            if code not in self.rooms:
                return code

    def _leave_all(self, access_code: str):
        for r in list(self.rooms.values()):
            if r.side(access_code)[0]:
                self._drop(r, access_code)

    def _room(self, p) -> tuple[Room | None, Side | None, Side | None]:
        r = self.rooms.get(p.get("code", ""))
        if not r:
            return None, None, None
        me, opp = r.side(p.get("access_code", ""))
        if me:
            me.seen = now()
        return r, me, opp

    # ------------------------------------------------------------------------------------------------------------
    def create(self, p, quick=False):
        code = p.get("access_code", "")
        prof = self.profile_of(code)
        if not prof:
            return 404, {"detail": "unknown access code"}
        with self.lock:
            self._expire()
            self._leave_all(code)
            room = Room(self._new_code(), quick, Side(code, prof))
            self.rooms[room.code] = room
            return 200, {"code": room.code}

    def quick(self, p):
        code = p.get("access_code", "")
        prof = self.profile_of(code)
        if not prof:
            return 404, {"detail": "unknown access code"}
        with self.lock:
            self._expire()
            self._leave_all(code)
            for r in sorted(self.rooms.values(), key=lambda r: r.host.seen):
                if r.quick and r.guest is None and r.state == "waiting":
                    r.guest = Side(code, prof)
                    r.state = "choosing"
                    return 200, {"code": r.code}
        return self.create(p, quick=True)

    def join(self, p):
        code = p.get("access_code", "")
        prof = self.profile_of(code)
        if not prof:
            return 404, {"detail": "unknown access code"}
        with self.lock:
            self._expire()
            r = self.rooms.get(p.get("code", ""))
            if not r:
                return 404, {"detail": "no_room"}
            if r.side(code)[0]:
                return 200, {"code": r.code}
            if r.guest is not None:
                return 409, {"detail": "full"}
            self._leave_all(code)
            r.guest = Side(code, prof)
            r.state = "choosing"
            return 200, {"code": r.code}

    def leave(self, p):
        with self.lock:
            r, me, _ = self._room(p)
            if r and me:
                self._drop(r, me.code)
        return 200, {"ok": True}

    def state(self, p):
        with self.lock:
            self._expire()
            r, me, opp = self._room(p)
            if not r or not me:
                return 404, {"detail": "no_room"}
            out = {"code": r.code, "state": r.state, "role": "host" if me is r.host else "guest", "round": r.round,
                   "song": r.song, "you": {"ready": me.ready, "difficulty": me.difficulty}}
            if opp:
                out["opponent"] = {**opp.profile, "has_song": opp.has_song, "difficulty": opp.difficulty,
                                   "ready": opp.ready, "mods": opp.mods}
            return 200, out

    def song(self, p):
        with self.lock:
            r, me, opp = self._room(p)
            if not r or me is not r.host:
                return 403, {"detail": "not_host"}
            if not opp:
                return 409, {"detail": "no_opponent"}
            if r.state == "playing":
                return 409, {"detail": "playing"}
            r.song = {"hash": p.get("hash", ""), "title": p.get("title", "")[:200],
                      "difficulty": int(p.get("difficulty", "3") or 3)}
            r.host.reset_round()
            opp.reset_round()
            r.host.difficulty = r.song["difficulty"]
            r.host.mods = p.get("mods", "")[:500]
            r.host.ready = True
            r.state = "song"
            r.round += 1
            return 200, {"ok": True}

    def ready(self, p):
        with self.lock:
            r, me, opp = self._room(p)
            if not r or not me or me is r.host or r.state != "song":
                return 409, {"detail": "not_now"}
            me.has_song = p.get("has_song") in ("1", "true")
            if not me.has_song:
                r.state = "choosing"    # the host picks again
                return 200, {"ok": True}
            me.difficulty = int(p.get("difficulty", "3") or 3)
            me.mods = p.get("mods", "")[:500]
            me.ready = True
            r.state = "playing"
            return 200, {"ok": True}

    def sync(self, p):
        with self.lock:
            r, me, opp = self._room(p)
            if not r or not me:
                return 404, {"detail": "no_room"}
            start = int(p.get("from", "0") or 0)
            hits = [h for h in (p.get("hits") or "").split(",") if h]
            if start == len(me.hits) and len(me.hits) + len(hits) <= ROOM_MAX_HITS:
                me.hits.extend(hits)    # a resend of hits already here (a lost answer) is ignored
            if p.get("pos"):
                me.pos = float(p["pos"])
            me.score = int(p.get("score", "0") or 0)
            me.combo = int(p.get("combo", "0") or 0)
            out = {"have": len(me.hits), "state": r.state, "round": r.round}
            if opp:
                since = int(p.get("since", "0") or 0)
                out.update(hits=",".join(opp.hits[since:]), total=len(opp.hits), pos=opp.pos, score=opp.score,
                           combo=opp.combo, final=opp.final, left=False)
            else:
                out.update(hits="", total=0, pos=None, score=0, combo=0, final=None, left=True)
            return 200, out

    def finish(self, p):
        with self.lock:
            r, me, opp = self._room(p)
            if not r or not me:
                return 404, {"detail": "no_room"}
            me.final = {k: int(p.get(k, "0") or 0) for k in ("score", "good", "ok", "bad", "drumroll", "max_combo")}
            if opp is None or opp.final is not None:
                r.state = "finished"
            return 200, {"ok": True}
