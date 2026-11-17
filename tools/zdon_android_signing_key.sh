#!/usr/bin/env bash
# Makes the fixed Android signing key and gives it to the release workflow (.github/workflows/zdon-release.yml),
# so every APK is signed the same way and a new one installs over the old one (which the in-game update needs).
#
#   bash tools/zdon_android_signing_key.sh [keystore file] [owner/repo]
#
# Asks for a password (typed twice, not shown), writes a PKCS#12 keystore (RSA 4096, alias "zdon", valid 30
# years) and sets the four GitHub secrets ANDROID_KEYSTORE_BASE64 / _PASSWORD / ANDROID_KEY_ALIAS /
# ANDROID_KEY_PASSWORD with `gh` (nothing is printed). Needs openssl and a logged-in gh.
#
# BACK UP THE KEYSTORE AND THE PASSWORD. Lost, no later APK can update the installed one: every player would
# have to uninstall (losing the app's data) and install again.
set -euo pipefail
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"  # Git Bash: keep "/CN=..." as it is

KEYSTORE="${1:-$HOME/zdon-android-release.p12}"
REPO="${2:-GanYiZhong/Z-DON}"
ALIAS="zdon"

command -v openssl >/dev/null || { echo "needs openssl"; exit 1; }
command -v gh >/dev/null || { echo "needs the GitHub CLI (gh)"; exit 1; }
if [ -e "$KEYSTORE" ]; then
    echo "$KEYSTORE already exists: reusing it (delete it first to make a new key)"
else
    read -r -s -p "New keystore password (8+ characters): " PW; echo
    read -r -s -p "Again: " PW2; echo
    [ "$PW" = "$PW2" ] || { echo "the passwords differ"; exit 1; }
    [ ${#PW} -ge 8 ] || { echo "too short"; exit 1; }
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    # relative names in $TMP and no path conversion: Git Bash would turn "/CN=..." into a Windows path
    (cd "$TMP" && MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" openssl req -x509 -newkey rsa:4096 -sha256 \
        -days 10950 -nodes -subj "/CN=Z-DON/O=Z-DON" -keyout key.pem -out cert.pem 2>/dev/null)
    # the password reaches openssl through its environment, not the command line
    ZDON_KS_PW="$PW" openssl pkcs12 -export -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -name "$ALIAS" \
        -out "$KEYSTORE" -passout env:ZDON_KS_PW
    echo "keystore: $KEYSTORE"
fi
if [ -z "${PW:-}" ]; then
    read -r -s -p "Keystore password: " PW; echo
fi
ZDON_KS_PW="$PW" openssl pkcs12 -in "$KEYSTORE" -passin env:ZDON_KS_PW -nokeys -noout 2>/dev/null \
    || { echo "wrong password for $KEYSTORE"; exit 1; }

base64 -w0 "$KEYSTORE" | gh secret set ANDROID_KEYSTORE_BASE64 -R "$REPO"
printf '%s' "$PW" | gh secret set ANDROID_KEYSTORE_PASSWORD -R "$REPO"
printf '%s' "$PW" | gh secret set ANDROID_KEY_PASSWORD -R "$REPO"
printf '%s' "$ALIAS" | gh secret set ANDROID_KEY_ALIAS -R "$REPO"
echo "set ANDROID_KEYSTORE_BASE64, ANDROID_KEYSTORE_PASSWORD, ANDROID_KEY_PASSWORD, ANDROID_KEY_ALIAS on $REPO"
echo "Back up $KEYSTORE and its password somewhere safe (not in the repository)."
