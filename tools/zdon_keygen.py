#!/usr/bin/env python3
"""Z-DON key generator: the server's request-signing key and the TaikoForge single sign-on secret.

Each key is 256 bits built from several independent sources and algorithms, so no single weak part
can make it guessable:

  1. entropy pool: the OS CSPRNG (secrets / BCryptGenRandom) twice, CPU timing jitter, a random UUID,
     this machine's identity (Windows MachineGuid / /etc/machine-id, host name), the clock, the PID and
     anything passed with --extra
  2. condensed with three different hash families: SHA3-512, keyed BLAKE2b-512 and SHA-512
  3. HKDF-SHA512 (RFC 5869) with a salt from BLAKE2s(machine identity) + fresh random bytes, expanded
     separately per purpose ("auth", "sso"), so the two keys are unrelated
  4. XORed with another 32 bytes straight from the OS CSPRNG: the result is at least as strong as the
     OS generator even if every other step were broken

The machine identity is only mixed in, never the sole input: knowing this PC does not reveal the key.

Usage (from the repo root):
    python tools/zdon_keygen.py                 # create missing keys (never replaces existing ones)
    python tools/zdon_keygen.py --rotate auth   # replace the request-signing key (needs a new game build)
    python tools/zdon_keygen.py --rotate sso    # replace the SSO secret (TaikoForge must get the new one)
    python tools/zdon_keygen.py --show          # fingerprints of the current keys, to compare machines

Writes (all git-ignored; the previous file is kept as <file>.bak-<time>):
    .env                                NETWORK_URL / NETWORK_AUTH_KEY, compiled into the game
    server/zdon_server.toml             auth_key and [site] sso_secret (created from the example)
    server/zdon_secrets_taikoforge.env  the two lines TaikoForge's environment needs
Keys are never printed; only a short fingerprint (first 12 hex digits of their SHA-256).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import hmac
import os
import platform
import re
import secrets
import shutil
import socket
import sys
import time
import uuid
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ENV_FILE = REPO / ".env"
SERVER_TOML = REPO / "server" / "zdon_server.toml"
SERVER_EXAMPLE = REPO / "server" / "zdon_server.example.toml"
FORGE_ENV = REPO / "server" / "zdon_secrets_taikoforge.env"
DEFAULT_URL = "https://zdon.taiko.ac"


# ----------------------------------------------------------------------------------------------------------------------
# entropy


def machine_identity() -> bytes:
    parts = [platform.node(), socket.gethostname(), platform.platform(), platform.machine()]
    if sys.platform == "win32":
        try:
            import winreg

            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Cryptography",
                                0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
                parts.append(str(winreg.QueryValueEx(k, "MachineGuid")[0]))
        except OSError:
            pass
    for p in ("/etc/machine-id", "/var/lib/dbus/machine-id"):
        try:
            parts.append(Path(p).read_text().strip())
        except OSError:
            pass
    parts.append(hex(uuid.getnode()))
    return "\x1f".join(parts).encode()


def timing_jitter(samples: int = 512) -> bytes:
    """Low bits of how long small pieces of work take: scheduler, cache and clock noise."""
    out = bytearray()
    h = hashlib.sha256()
    for i in range(samples):
        t0 = time.perf_counter_ns()
        h.update(i.to_bytes(4, "little"))
        _ = h.digest()
        out += ((time.perf_counter_ns() - t0) & 0xFFFF).to_bytes(2, "little")
    return bytes(out)


def entropy_pool(extra: bytes) -> bytes:
    return b"".join([
        secrets.token_bytes(64),
        os.urandom(64),
        timing_jitter(),
        uuid.uuid4().bytes,
        machine_identity(),
        time.time_ns().to_bytes(8, "little"),
        time.perf_counter_ns().to_bytes(8, "little"),
        os.getpid().to_bytes(4, "little"),
        extra,
    ])


# ----------------------------------------------------------------------------------------------------------------------
# key derivation


def hkdf_sha512(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    prk = hmac.new(salt, ikm, hashlib.sha512).digest()
    okm, block, counter = b"", b"", 1
    while len(okm) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha512).digest()
        okm += block
        counter += 1
    return okm[:length]


def make_key(purpose: str, extra: bytes = b"") -> str:
    pool = entropy_pool(extra)
    condensed = (hashlib.sha3_512(pool).digest()
                 + hashlib.blake2b(pool, digest_size=64, key=secrets.token_bytes(32)).digest()
                 + hashlib.sha512(pool).digest())
    salt = hashlib.blake2s(machine_identity()).digest() + secrets.token_bytes(16)
    derived = hkdf_sha512(condensed, salt, f"Z-DON {purpose} key v1".encode(), 32)
    final = bytes(a ^ b for a, b in zip(derived, secrets.token_bytes(32)))
    return final.hex()


def fingerprint(key: str) -> str:
    return hashlib.sha256(key.encode()).hexdigest()[:12] if key else "(none)"


# ----------------------------------------------------------------------------------------------------------------------
# files


def backup(path: Path) -> None:
    if path.exists():
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        shutil.copy2(path, path.with_name(f"{path.name}.bak-{stamp}"))


def read_env(path: Path) -> dict[str, str]:
    out = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
            if m and not line.lstrip().startswith("#"):
                out[m.group(1)] = m.group(2).strip().strip("\"'")
    return out


def write_env(path: Path, values: dict[str, str]) -> None:
    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    done = set()
    for i, line in enumerate(lines):
        m = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
        if m and m.group(1) in values and not line.lstrip().startswith("#"):
            lines[i] = f"{m.group(1)}={values[m.group(1)]}"
            done.add(m.group(1))
    lines += [f"{k}={v}" for k, v in values.items() if k not in done]
    backup(path)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def toml_get(text: str, section: str, key: str) -> str:
    current = ""
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"^\[([^\[\]]+)\]$", s)
        if m:
            current = m.group(1).strip()
            continue
        m = re.match(rf'^{re.escape(key)}\s*=\s*["\'](.*?)["\']', s)
        if m and current == section:
            return m.group(1)
    return ""


def toml_set(text: str, section: str, key: str, value: str) -> str:
    """Sets key in [section] ("" = before the first table), keeping everything else as it was."""
    lines = text.splitlines()
    current, seen, insert_at = "", section == "", None
    for i, line in enumerate(lines):
        s = line.strip()
        m = re.match(r"^\[([^\[\]]+)\]$", s)
        if m:
            if current == section and insert_at is None:
                insert_at = i  # where the section ends, should the key be missing
            current = m.group(1).strip()
            seen = seen or current == section
            continue
        if current == section and re.match(rf"^{re.escape(key)}\s*=", s):
            lines[i] = f'{key} = "{value}"'
            return "\n".join(lines) + "\n"
    new = f'{key} = "{value}"'
    if not seen:
        lines += ["", f"[{section}]", new]
    elif insert_at is None or current == section:
        lines.append(new)
    else:
        lines.insert(insert_at, new)
    return "\n".join(lines) + "\n"


# ----------------------------------------------------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rotate", choices=["auth", "sso", "all"], help="replace existing keys")
    ap.add_argument("--show", action="store_true", help="print the current keys' fingerprints and exit")
    ap.add_argument("--url", default=None, help=f"server URL compiled into the game (default {DEFAULT_URL})")
    ap.add_argument("--extra", default="", help="extra text mixed into the entropy pool (e.g. keyboard mashing)")
    args = ap.parse_args()

    env = read_env(ENV_FILE)
    toml_text = SERVER_TOML.read_text(encoding="utf-8") if SERVER_TOML.exists() else (
        SERVER_EXAMPLE.read_text(encoding="utf-8") if SERVER_EXAMPLE.exists() else "")
    auth = toml_get(toml_text, "", "auth_key") or env.get("NETWORK_AUTH_KEY", "")
    sso = toml_get(toml_text, "site", "sso_secret")

    if args.show:
        print(f"auth key (game .env / server auth_key): {fingerprint(env.get('NETWORK_AUTH_KEY', ''))} / "
              f"{fingerprint(toml_get(toml_text, '', 'auth_key'))}")
        print(f"SSO secret (server [site] / TaikoForge file): {fingerprint(sso)} / "
              f"{fingerprint(read_env(FORGE_ENV).get('TF_SSO_SECRET_ZDON', ''))}")
        return

    extra = args.extra.encode()
    rotate = args.rotate or ""
    changed = []
    if not auth or rotate in ("auth", "all"):
        auth = make_key("auth", extra)
        changed.append("auth")
    if not sso or rotate in ("sso", "all"):
        sso = make_key("sso", extra)
        changed.append("sso")
    if not changed:
        print("Both keys already exist; nothing changed (use --rotate to replace them, --show to compare).")
        return

    url = args.url or env.get("NETWORK_URL") or DEFAULT_URL
    write_env(ENV_FILE, {"NETWORK_URL": url, "NETWORK_AUTH_KEY": auth})
    toml_text = toml_set(toml_text, "", "auth_key", auth)
    toml_text = toml_set(toml_text, "site", "sso_secret", sso)
    backup(SERVER_TOML)
    SERVER_TOML.write_text(toml_text, encoding="utf-8", newline="\n")
    site_url = toml_get(toml_text, "site", "url") or url
    backup(FORGE_ENV)
    FORGE_ENV.write_text("# Add to TaikoForge's environment (forge.taiko.ac), then restart it. Keep this file private.\n"
                         f"TF_SSO_CLIENTS=zdon={site_url.rstrip('/')}/auth/callback\n"
                         f"TF_SSO_SECRET_ZDON={sso}\n", encoding="utf-8", newline="\n")

    print(f"new: {', '.join(changed)}")
    print(f"  auth key  {fingerprint(auth)}  -> .env (NETWORK_AUTH_KEY, NETWORK_URL={url}), server/zdon_server.toml")
    print(f"  SSO secret {fingerprint(sso)}  -> server/zdon_server.toml [site], {FORGE_ENV.relative_to(REPO)}")
    if "auth" in changed:
        print("next: cmake . in build/ and rebuild, then restart the server and publish the new build.")
    if "sso" in changed:
        print(f"next: put {FORGE_ENV.relative_to(REPO)} into TaikoForge's environment and restart it.")


if __name__ == "__main__":
    main()
