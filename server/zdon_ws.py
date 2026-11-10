"""A minimal WebSocket (RFC 6455) server side for zdon_server.py's live battles: text frames, ping/pong, close.
Messages are small JSON strings; a connection lives in its handler thread for one song."""
from __future__ import annotations

import base64
import hashlib
import socket
import struct
import threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_MESSAGE = 1 << 20   # a client message bigger than this closes the connection


def accept_key(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


class WsConn:
    def __init__(self, sock: socket.socket, rfile, wfile, idle_s: float = 30.0):
        self.sock = sock
        self.rfile = rfile
        self.wfile = wfile
        self.lock = threading.Lock()   # writes come from this thread and from the opponent's
        self.closed = False
        sock.settimeout(idle_s)        # nothing from the client for this long: gone
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass

    def _frame(self, opcode: int, payload: bytes) -> bytes:
        n = len(payload)
        if n < 126:
            head = struct.pack("!BB", 0x80 | opcode, n)
        elif n < 65536:
            head = struct.pack("!BBH", 0x80 | opcode, 126, n)
        else:
            head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
        return head + payload

    def _write(self, data: bytes) -> bool:
        with self.lock:
            if self.closed:
                return False
            try:
                self.wfile.write(data)
                self.wfile.flush()
                return True
            except OSError:
                self.closed = True
                return False

    def send_text(self, text: str) -> bool:
        return self._write(self._frame(0x1, text.encode()))

    def close(self):
        self._write(self._frame(0x8, b""))
        with self.lock:
            self.closed = True

    def _read(self, n: int) -> bytes:
        data = self.rfile.read(n)
        if data is None or len(data) < n:
            raise ConnectionError("closed")
        return data

    def recv(self) -> str | None:
        """The next text message, or None once the connection is closed (or idle too long)."""
        parts: list[bytes] = []
        try:
            while True:
                b1, b2 = self._read(2)
                fin, opcode = b1 & 0x80, b1 & 0x0F
                n = b2 & 0x7F
                if n == 126:
                    n = struct.unpack("!H", self._read(2))[0]
                elif n == 127:
                    n = struct.unpack("!Q", self._read(8))[0]
                if n > MAX_MESSAGE:
                    raise ConnectionError("too big")
                mask = self._read(4) if b2 & 0x80 else b""
                payload = self._read(n) if n else b""
                if mask:
                    payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
                if opcode == 0x8:                       # close
                    self.close()
                    return None
                if opcode == 0x9:                       # ping
                    self._write(self._frame(0xA, payload))
                    continue
                if opcode == 0xA:                       # pong
                    continue
                parts.append(payload)
                if sum(map(len, parts)) > MAX_MESSAGE:
                    raise ConnectionError("too big")
                if fin:
                    return b"".join(parts).decode("utf-8", "replace")
        except (OSError, ConnectionError, ValueError):
            with self.lock:
                self.closed = True
            return None
