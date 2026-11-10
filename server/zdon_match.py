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
    GET  /match/ws     code since    a WebSocket for the song (the game falls back to /match/sync without one):
        game -> {"t":"s","from","hits","pos","score","combo","t0"}       my new hits and position (as /match/sync)
        server -> {"t":"hello","have"} once, then {"t":"o","at","hits","pos","score","combo","t0"} the opponent's
                  new hits (pushed the moment they arrive), {"t":"a","have"} my hits it holds,
                  {"t":"f","final"} the opponent finished, {"t":"l"} the opponent left
    `t0` is the sender's wall clock (ms) at chart ms 0: the receiving game logs how late the hits arrived.
A guest without the host's chart asks for it (ready has_song=0 want=1: state "transfer"); the host's game zips the
song's folder and sends it, the guest's downloads it and checks the chart's hash before adding it to its library:
    POST /match/upload       code hash name sha256 (+ the zip as the body)     the host
    POST /match/upload_fail  code                                             the host could not send it
    GET  /match/download     code                    -> the zip                the guest
The round starts when the guest is ready (the host is ready by picking); each game starts on its own. The opponent's
lane scrolls with the local music and judges the opponent's hits as they arrive; it counts notes as missed only up to
the opponent's reported position (`pos`, sent with every hit before it).
"""
from __future__ import annotations

import hashlib
import json
import secrets
import threading
import time

ROOM_IDLE_S = 30.0      # a player not heard from for this long has left
ROOM_MAX_HITS = 20000   # per player per round
SONG_UPLOAD_MAX = 90 * 1024 * 1024  # a song sent to the guest (under Cloudflare's 100 MB request limit)


def now() -> float:
    return time.monotonic()


class Side:
    def __init__(self, code: str, profile: dict):
        self.code = code
        self.profile = profile          # username, title, colours: what the opponent's lane shows
        self.seen = now()
        self.ws = None                  # the WebSocket of the song being played (zdon_ws.WsConn), if any
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
        self.t0 = None                  # the player's wall clock (ms) at chart ms 0


class Room:
    def __init__(self, code: str, quick: bool, host: Side):
        self.code = code
        self.quick = quick
        self.host = host
        self.guest: Side | None = None
        self.state = "waiting"          # waiting (no guest) / choosing / song / playing / finished
        self.song: dict | None = None   # hash, title, difficulty (the host's)
        self.round = 0
        self.transfer: dict | None = None   # the song being sent to the guest: hash, name, data

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
        for side in (r.host, r.guest):  # the one staying hears it at once
            if side and side.code != access_code and side.ws:
                side.ws.send_text('{"t":"l"}')
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
        r.transfer = None
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
            if r.state == "transfer":
                out["transfer"] = {"ready": r.transfer is not None,
                                   "size": len(r.transfer["data"]) if r.transfer else 0}
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
            r.transfer = None
            r.host.difficulty = r.song["difficulty"]
            r.host.mods = p.get("mods", "")[:500]
            r.host.ready = True
            r.state = "song"
            r.round += 1
            return 200, {"ok": True}

    def ready(self, p):
        with self.lock:
            r, me, opp = self._room(p)
            if not r or not me or me is r.host or r.state not in ("song", "transfer"):
                return 409, {"detail": "not_now"}
            me.has_song = p.get("has_song") in ("1", "true")
            if not me.has_song:
                if p.get("want") in ("1", "true") and r.state == "song":
                    r.state = "transfer"    # the host sends it
                    r.transfer = None
                else:
                    r.state = "choosing"    # the host picks again
                    r.transfer = None
                return 200, {"ok": True}
            r.transfer = None
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
            self._take(me, p)
            out = {"have": len(me.hits), "state": r.state, "round": r.round}
            if opp:
                since = int(p.get("since", "0") or 0)
                out.update(hits=",".join(opp.hits[since:]), total=len(opp.hits), pos=opp.pos, score=opp.score,
                           combo=opp.combo, final=opp.final, left=False, t0=opp.t0)
            else:
                out.update(hits="", total=0, pos=None, score=0, combo=0, final=None, left=True)
            return 200, out

    @staticmethod
    def _take(me: Side, p) -> list[str]:
        """A sync's new hits and position (the hits only when they follow on from what is here); the hits added."""
        start = int(p.get("from", "0") or 0)
        hits = [h for h in (p.get("hits") or "").split(",") if h]
        added = []
        if start == len(me.hits) and len(me.hits) + len(hits) <= ROOM_MAX_HITS:
            me.hits.extend(hits)    # a resend of hits already here (a lost answer) is ignored
            added = hits
        if p.get("pos") not in (None, ""):
            me.pos = float(p["pos"])
        me.score = int(p.get("score", "0") or 0)
        me.combo = int(p.get("combo", "0") or 0)
        if p.get("t0") not in (None, ""):
            me.t0 = float(p["t0"])
        return added

    def ws_session(self, p, conn):
        """A WebSocket for one song: what this player sends is pushed to the opponent's at once."""
        with self.lock:
            r, me, opp = self._room(p)
            if not r or not me:
                conn.send_text('{"t":"err","detail":"no_room"}')
                return
            if me.ws and me.ws is not conn:
                me.ws.close()
            me.ws = conn
            since = int(p.get("since", "0") or 0)
            hello = {"t": "hello", "have": len(me.hits)}
            first = None
            if opp:
                first = {"t": "o", "at": since, "hits": ",".join(opp.hits[since:]), "pos": opp.pos,
                         "score": opp.score, "combo": opp.combo, "t0": opp.t0}
            gone = opp is None
            final = opp.final if opp else None
        conn.send_text(json.dumps(hello))
        if first:
            conn.send_text(json.dumps(first))
        if final:
            conn.send_text(json.dumps({"t": "f", "final": final}))
        if gone:
            conn.send_text('{"t":"l"}')
        try:
            while True:
                text = conn.recv()
                if text is None:
                    break
                try:
                    msg = json.loads(text)
                except ValueError:
                    continue
                if not isinstance(msg, dict) or msg.get("t") != "s":
                    continue
                q = {k: str(v) for k, v in msg.items() if v is not None}
                with self.lock:
                    r2 = self.rooms.get(r.code)
                    me2, opp = r2.side(me.code) if r2 else (None, None)
                    if me2 is not me:
                        break       # this player left the room
                    me.seen = now()
                    at = len(me.hits)
                    added = self._take(me, q)
                    push = None
                    if opp and opp.ws:
                        push = (opp.ws, {"t": "o", "at": at if added else len(me.hits), "hits": ",".join(added),
                                         "pos": me.pos, "score": me.score, "combo": me.combo, "t0": me.t0})
                    have = len(me.hits)
                if push:
                    push[0].send_text(json.dumps(push[1]))
                took = bool(added) or not q.get("hits")
                conn.send_text('{"t":"a","have":%d,"took":%s}' % (have, "true" if took else "false"))
        finally:
            with self.lock:
                if me.ws is conn:
                    me.ws = None

    def upload(self, p, body: bytes):
        with self.lock:
            r, me, _ = self._room(p)
            if not r or me is not r.host or r.state != "transfer" or not r.song:
                return 409, {"detail": "not_now"}
            if p.get("hash") != r.song["hash"]:
                return 409, {"detail": "other_song"}
            if not body or len(body) > SONG_UPLOAD_MAX or hashlib.sha256(body).hexdigest() != p.get("sha256", ""):
                return 400, {"detail": "bad_body"}
            r.transfer = {"hash": r.song["hash"], "name": p.get("name", "")[:120], "data": body}
            return 200, {"ok": True}

    def upload_fail(self, p):
        with self.lock:
            r, me, opp = self._room(p)
            if r and me is r.host and r.state == "transfer":
                r.state = "choosing"    # the host picks again (the guest's has_song stays false)
                r.transfer = None
        return 200, {"ok": True}

    def download(self, p):
        with self.lock:
            r, me, _ = self._room(p)
            if not r or not me or me is r.host or r.state != "transfer" or not r.transfer:
                return 409, {"detail": "not_now"}
            return 200, r.transfer["data"]

    def finish(self, p):
        with self.lock:
            r, me, opp = self._room(p)
            if not r or not me:
                return 404, {"detail": "no_room"}
            me.final = {k: int(p.get(k, "0") or 0) for k in ("score", "good", "ok", "bad", "drumroll", "max_combo")}
            if opp is None or opp.final is not None:
                r.state = "finished"
            if opp and opp.ws:
                opp.ws.send_text(json.dumps({"t": "f", "final": me.final}))
            return 200, {"ok": True}
