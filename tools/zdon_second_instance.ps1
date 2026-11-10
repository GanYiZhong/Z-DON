# Z-DON: a second copy of the game for testing online battles on one PC (two windows, two accounts).
#
#   powershell -ExecutionPolicy Bypass -File tools\zdon_second_instance.ps1
#
# Makes (or refreshes) build\bin_p2 next to build\bin:
#   - YataiDON.exe, the DLLs and shader\ copied (run again after a rebuild to refresh them)
#   - Skins\ and Songs\ as junctions to build\bin's (nothing big is copied; the same skins and songs)
#   - its own config.toml (from build\bin's, the first time): a second server account, the local server,
#     the audio on the default device (two games can't share an exclusive one), no update check
#   - its own scores.db (a copy, the first time), with auto play on for the second account: you play in one
#     window, this one plays itself (turn it off in the song select's options if you want to play both)
#   - start_p2.bat to launch it
# Your build\bin (config, scores, audio settings) is not changed.

param(
    [string]$Account = "Player2P",
    [string]$Server = "http://127.0.0.1:8730"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$main = Join-Path $repo "build\bin"
$p2 = Join-Path $repo "build\bin_p2"
$py = (Get-Command py -ErrorAction SilentlyContinue).Source
$pyArgs = @("-3")
if (-not $py) { $py = (Get-Command python -ErrorAction Stop).Source; $pyArgs = @() }
$utf8 = New-Object System.Text.UTF8Encoding($false)

if (-not (Test-Path (Join-Path $main "YataiDON.exe"))) { throw "build\bin has no YataiDON.exe" }
New-Item -ItemType Directory -Force $p2 | Out-Null

# the program
Copy-Item (Join-Path $main "YataiDON.exe") $p2 -Force
Copy-Item (Join-Path $main "*.dll") $p2 -Force
robocopy (Join-Path $main "shader") (Join-Path $p2 "shader") /MIR /NFL /NDL /NJH /NJS /NP | Out-Null

# the same skins and songs
foreach ($d in "Skins", "Songs") {
    $link = Join-Path $p2 $d
    if (-not (Test-Path $link)) {
        cmd /c mklink /J "$link" "$(Join-Path $main $d)" | Out-Null
        Write-Host "linked $d"
    }
}

# its own database (the first time)
$db = Join-Path $p2 "scores.db"
if (-not (Test-Path $db)) { Copy-Item (Join-Path $main "scores.db") $db }

# its own config and account (the first time)
$cfgPath = Join-Path $p2 "config.toml"
$fresh = -not (Test-Path $cfgPath)
if ($fresh) { Copy-Item (Join-Path $main "config.toml") $cfgPath }
$cfg = [IO.File]::ReadAllText($cfgPath, $utf8)
$code = ""
if ($cfg -match "(?m)^access_code_1 = '(\d{19,24})'" -and -not $fresh) { $code = $Matches[1] }
if (-not $code) {
    $keyFile = Join-Path $repo "server\zdon_server.toml"
    $reg = @'
import hashlib, hmac, secrets, sys, time, tomllib, urllib.parse, urllib.request
key = tomllib.load(open(sys.argv[1], "rb")).get("auth_key", "")
name, base = sys.argv[2], sys.argv[3].rstrip("/")
ts, nonce = str(int(time.time())), secrets.token_hex(16)
p = {"username": name, "idm": ""}
can = "POST\n/register_user\n" + "".join(f"{k}={p[k]}&" for k in sorted(p)) + f"\n{ts}\n{nonce}"
h = {"X-Timestamp": ts, "X-Nonce": nonce, "Content-Type": "application/x-www-form-urlencoded",
     "X-Signature": hmac.new(key.encode(), can.encode(), hashlib.sha256).hexdigest()}
req = urllib.request.Request(base + "/register_user", data=urllib.parse.urlencode({"username": name}).encode(),
                             headers=h, method="POST")
print(urllib.request.urlopen(req, timeout=10).read().decode().strip())
'@
    $tmp = Join-Path $env:TEMP "zdon_register.py"
    [IO.File]::WriteAllText($tmp, $reg, $utf8)
    $code = (& $py @pyArgs $tmp $keyFile $Account $Server).Trim()
    Remove-Item $tmp
    if ($code -notmatch '^\d{19,24}$') { throw "could not register an account: $code" }
    Write-Host "registered account '$Account' on $Server"
}
$net = "[network]`naccess_code_1 = '$code'`naccess_code_2 = '1'`nauto_login = true`nguest_mode = false`n" +
       "online_play = true`nsync_scores = false`nzdon_server_url = '$Server'`nzdon_update_check = false`n`n"
$cfg = [regex]::Replace($cfg, "(?ms)^\[network\].*?(?=^\[)", $net)
$cfg = [regex]::Replace($cfg, "(?m)^device_type = .*$", "device_type = 0")
$cfg = [regex]::Replace($cfg, "(?m)^skin_updater = .*$", "skin_updater = false")
[IO.File]::WriteAllText($cfgPath, $cfg, $utf8)

# auto play for the second account (its player row, created if the game hasn't yet)
$autoPy = @'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1]); code, name = sys.argv[2], sys.argv[3]
if not c.execute("SELECT 1 FROM players WHERE player_id=?", (code,)).fetchone():
    taken = c.execute("SELECT 1 FROM players WHERE username=?", (name,)).fetchone()
    c.execute("INSERT INTO players(player_id, username, title) VALUES (?,?,?)", (code, name + ("-" + code[-4:] if taken else ""), ""))
c.execute("UPDATE players SET modifier_auto=1 WHERE player_id=?", (code,))
c.commit()
'@
$tmp = Join-Path $env:TEMP "zdon_auto.py"
[IO.File]::WriteAllText($tmp, $autoPy, $utf8)
& $py @pyArgs $tmp $db $code $Account
Remove-Item $tmp

[IO.File]::WriteAllText((Join-Path $p2 "start_p2.bat"), "@echo off`r`ncd /d `"%~dp0`"`r`nstart `"`" YataiDON.exe`r`n", $utf8)
Write-Host "ready: $p2  (start it with start_p2.bat)"
