#!/bin/sh
# Z-DON updater for Linux and macOS: brings this install up to date from a Z-DON server
# (server/zdon_server.py publish-github), downloading only the files whose checksum changed. The Windows one is
# zdon_update.ps1; this does the same.
#
# Source: --url, else config.toml [network] zdon_update_url, else zdon_server_url + "/update/<linux|macos>",
# else zdon_update.url (the channel this install was published to).
# Server layout: <url>/version.txt, <url>/checksums.sha256 ("<sha256>  <path>" lines), <url>/files/<path>
#
#   sh zdon_update.sh                    update now (the game must be closed)
#   sh zdon_update.sh --check-only       exit 10 when an update is available, 0 when up to date
#   sh zdon_update.sh --wait-pid <pid> --restart      (what the game runs)
#
# Never touches config.toml, scores.db, cache/, Songs/, Records/ or anything the server didn't publish.
# Files a previous update installed and the new release no longer lists are removed. Needs curl.

URL=""
WAIT_PID=""
CHECK_ONLY=0
RESTART=0
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --url) URL="$2"; shift 2 ;;
        --wait-pid) WAIT_PID="$2"; shift 2 ;;
        --check-only) CHECK_ONLY=1; shift ;;
        --restart) RESTART=1; shift ;;
        --force) FORCE=1; shift ;;
        *) echo "[zdon-update] unknown option: $1"; exit 2 ;;
    esac
done

INSTALL_DIR=$(cd "$(dirname "$0")" && pwd)
VERSION_FILE="$INSTALL_DIR/.zdon_version"
MANIFEST_FILE="$INSTALL_DIR/.zdon_manifest.sha256"
STAGE_DIR="$INSTALL_DIR/.zdon_update_tmp"
WORK=$(mktemp -d 2>/dev/null || mktemp -d -t zdonupd)
trap 'rm -rf "$WORK"' EXIT

log() { echo "[zdon-update] $*"; }
fail() { echo "[zdon-update] Error: $*" >&2; exit "${2:-1}"; }

case "$(uname -s)" in
    Darwin) CHANNEL=macos ;;
    *) CHANNEL=linux ;;
esac

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | awk '{print tolower($1)}'; }
elif command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | awk '{print tolower($1)}'; }
else
    fail "needs sha256sum or shasum"
fi
command -v curl >/dev/null 2>&1 || fail "needs curl"

# [network] <key> = "<value>" from config.toml
config_value() {
    [ -f "$INSTALL_DIR/config.toml" ] || return 0
    awk -v key="$1" '
        /^[ \t]*\[/ { s = $0; gsub(/[ \t\[\]]/, "", s); section = s; next }
        section == "network" {
            line = $0; sub(/^[ \t]+/, "", line)
            if (index(line, key) == 1) {
                rest = substr(line, length(key) + 1); sub(/^[ \t]*=[ \t]*/, "", rest)
                if (rest ~ /^["\047]/) { q = substr(rest, 1, 1); rest = substr(rest, 2); sub(q ".*$", "", rest); print rest; exit }
            }
        }' "$INSTALL_DIR/config.toml"
}

# a manifest path the updater may write: relative, no "..", nothing the player owns
safe_path() {
    case "$1" in
        ""|/*|*\\*|*:*) return 1 ;;
    esac
    case "/$1/" in
        */../*|*/./*) return 1 ;;
    esac
    lower=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')
    case "$lower" in
        config.toml|scores.db) return 1 ;;
        cache/*|songs/*|records/*|logs/*|.zdon_update_tmp/*) return 1 ;;
    esac
    return 0
}

[ -z "$URL" ] && URL=$(config_value zdon_update_url)
if [ -z "$URL" ]; then
    server=$(config_value zdon_server_url)
    [ -n "$server" ] && URL="${server%/}/update/$CHANNEL"
fi
if [ -z "$URL" ] && [ -f "$INSTALL_DIR/zdon_update.url" ]; then
    URL=$(tr -d ' \r\n' < "$INSTALL_DIR/zdon_update.url")
fi
[ -z "$URL" ] && fail "no update source: set zdon_update_url or zdon_server_url under [network] in config.toml, or pass --url" 2
URL=${URL%/}
log "Source: $URL"

curl -fsSL --max-time 30 "$URL/version.txt" -o "$WORK/version.txt" || fail "cannot reach $URL/version.txt"
curl -fsSL --max-time 60 "$URL/checksums.sha256" -o "$WORK/manifest" || fail "cannot reach $URL/checksums.sha256"
REMOTE_VERSION=$(tr -d '\r\n' < "$WORK/version.txt")
[ -n "$(tail -c 1 "$WORK/manifest")" ] && fail "the server's checksums.sha256 is incomplete"
# "<hash> <path>" (a leading "*" on the path dropped), lower-case hashes
tr -d '\r' < "$WORK/manifest" | awk 'NF >= 2 && $1 ~ /^[0-9a-fA-F]{64}$/ {
    h = tolower($1); p = $0; sub(/^[0-9a-fA-F]+[ \t]+\*?/, "", p); print h " " p }' > "$WORK/want"
LOCAL_VERSION=""
[ -f "$VERSION_FILE" ] && LOCAL_VERSION=$(tr -d '\r\n' < "$VERSION_FILE")
log "Installed: ${LOCAL_VERSION:-(unknown)}  |  Server: $REMOTE_VERSION"

# --- what changed ---
: > "$WORK/changed"
while IFS=' ' read -r hash rel; do
    safe_path "$rel" || { log "skipping unsafe path in manifest: $rel"; continue; }
    if [ "$FORCE" = 1 ] || [ ! -f "$INSTALL_DIR/$rel" ] || [ "$(sha256 "$INSTALL_DIR/$rel")" != "$hash" ]; then
        echo "$hash $rel" >> "$WORK/changed"
    fi
done < "$WORK/want"
: > "$WORK/removed"
if [ -f "$MANIFEST_FILE" ]; then
    tr -d '\r' < "$MANIFEST_FILE" | awk 'NF >= 2 { p = $0; sub(/^[0-9a-fA-F]+[ \t]+\*?/, "", p); print p }' |
        while IFS= read -r rel; do
            awk -v r="$rel" 'substr($0, index($0, " ") + 1) == r { f = 1 } END { exit !f }' "$WORK/want" && continue
            safe_path "$rel" && [ -f "$INSTALL_DIR/$rel" ] && echo "$rel" >> "$WORK/removed"
        done
fi
N_CHANGED=$(wc -l < "$WORK/changed" | tr -d ' ')
N_REMOVED=$(wc -l < "$WORK/removed" | tr -d ' ')

restart_game() {
    [ "$RESTART" = 1 ] || return 0
    log "Starting Z-DON..."
    (cd "$INSTALL_DIR" && nohup ./YataiDON >/dev/null 2>&1 &)
}
wait_for_game() {
    [ -n "$WAIT_PID" ] || return 0
    n=0
    while kill -0 "$WAIT_PID" 2>/dev/null; do
        [ $n -eq 0 ] && log "Waiting for the game (PID $WAIT_PID) to close..."
        n=$((n + 1)); sleep 1
        [ $n -gt 300 ] && fail "the game (PID $WAIT_PID) did not close"
    done
}

if [ "$N_CHANGED" = 0 ] && [ "$N_REMOVED" = 0 ]; then
    printf '%s\n' "$REMOTE_VERSION" > "$VERSION_FILE"
    cp "$WORK/manifest" "$MANIFEST_FILE"
    log "Already up to date."
    [ "$CHECK_ONLY" = 1 ] && exit 0
    wait_for_game
    restart_game
    exit 0
fi
log "$N_CHANGED file(s) to download, $N_REMOVED to remove."
[ "$CHECK_ONLY" = 1 ] && exit 10

# --- download into a staging folder beside the install, verifying each file ---
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR" || fail "cannot create $STAGE_DIR"
i=0
while IFS=' ' read -r hash rel; do
    i=$((i + 1))
    log "[$i/$N_CHANGED] $rel"
    mkdir -p "$STAGE_DIR/$(dirname "$rel")"
    ok=0
    for try in 1 2 3; do
        if curl -fsSL --max-time 600 "$URL/files/$rel" -o "$STAGE_DIR/$rel" &&
            [ "$(sha256 "$STAGE_DIR/$rel")" = "$hash" ]; then
            ok=1; break
        fi
        log "  failed or checksum mismatch, retrying"
    done
    [ $ok = 1 ] || fail "could not download $rel"
done < "$WORK/changed"

# --- the game must be closed before its files are replaced ---
wait_for_game

# --- swap the files in (a rename over the old file: a running copy keeps its own) ---
while IFS=' ' read -r hash rel; do
    mkdir -p "$INSTALL_DIR/$(dirname "$rel")"
    mv -f "$STAGE_DIR/$rel" "$INSTALL_DIR/$rel" || fail "could not replace $rel"
done < "$WORK/changed"
while IFS= read -r rel; do
    rm -f "$INSTALL_DIR/$rel" && log "removed $rel"
done < "$WORK/removed"
chmod +x "$INSTALL_DIR/YataiDON" "$INSTALL_DIR/zdon_update.sh" 2>/dev/null
rm -rf "$STAGE_DIR"
cp "$WORK/manifest" "$MANIFEST_FILE"
printf '%s\n' "$REMOTE_VERSION" > "$VERSION_FILE"
log "Updated to $REMOTE_VERSION."
restart_game
exit 0
