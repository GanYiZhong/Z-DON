# Z-DON updater: brings this install up to date from a Z-DON server (server/zdon_server.py publish),
# downloading only the files whose checksum changed.
#
# Source: -Url, else config.toml [network] zdon_update_url, else zdon_server_url + "/update/windows",
# else zdon_update.url (the channel this install was published to).
# Server layout: <url>/version.txt, <url>/checksums.sha256 ("<sha256>  <path>" lines), <url>/files/<path>
#
#   zdon_update.bat                      update now (the game must be closed)
#   zdon_update.bat -CheckOnly           exit 10 when an update is available, 0 when up to date
#   zdon_update.bat -WaitPid <pid> -Restart -PauseOnError     (what the game runs)
#
# Never touches config.toml, scores.db, cache\, Songs\, Records\ or anything the server didn't publish.
# Files a previous update installed and the new release no longer lists are removed.

param(
    [string]$Url = "",
    [string]$WaitPid = "",
    [switch]$CheckOnly,
    [switch]$Restart,
    [switch]$PauseOnError,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest is many times slower with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$InstallDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$VersionFile  = Join-Path $InstallDir ".zdon_version"
$ManifestFile = Join-Path $InstallDir ".zdon_manifest.sha256"
$HashCache    = Join-Path $InstallDir ".zdon_update_hashes.tsv"
$StageDir     = Join-Path $InstallDir ".zdon_update_tmp"
$Utf8         = New-Object System.Text.UTF8Encoding($false)

$ProtectedNames = @("config.toml", "scores.db")
$ProtectedDirs  = @("cache", "songs", "records", "logs", ".zdon_update_tmp")

function Log { param($msg) Write-Host "[zdon-update] $msg" }
function Fail {
    param($msg, [int]$code = 1)
    Write-Host "[zdon-update] Error: $msg" -ForegroundColor Red
    if ($PauseOnError) { Write-Host "Press Enter to close."; [void][Console]::ReadLine() }
    exit $code
}

function Read-ConfigValue {
    param([string]$Key)
    $cfg = Join-Path $InstallDir "config.toml"
    if (-not (Test-Path -LiteralPath $cfg)) { return "" }
    $section = ""
    foreach ($line in [IO.File]::ReadAllLines($cfg, $Utf8)) {
        $t = $line.Trim()
        if ($t -match '^\[(.+)\]$') { $section = $Matches[1].Trim(); continue }
        if ($section -eq "network" -and $t -match ('^' + [regex]::Escape($Key) + '\s*=\s*["''](.*)["'']\s*$')) {
            return $Matches[1].Trim()
        }
    }
    return ""
}

function Join-Url {
    param([string]$Base, [string]$Rel)
    $segments = $Rel.Split('/') | ForEach-Object { [Uri]::EscapeDataString($_) }
    return $Base.TrimEnd('/') + "/" + ($segments -join '/')
}

function Get-Text {
    param([string]$U)
    $tmp = [IO.Path]::GetTempFileName()
    try {
        Invoke-WebRequest -Uri $U -OutFile $tmp -UseBasicParsing -TimeoutSec 30
        return [IO.File]::ReadAllText($tmp, $Utf8)
    } finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
}

function Test-SafePath {
    param([string]$Rel)
    if ($Rel -eq "" -or $Rel.StartsWith("/") -or $Rel.Contains(":") -or $Rel.Contains("\")) { return $false }
    foreach ($part in $Rel.Split('/')) { if ($part -eq ".." -or $part -eq ".") { return $false } }
    if ($ProtectedNames -contains $Rel.ToLower()) { return $false }
    if ($ProtectedDirs -contains $Rel.Split('/')[0].ToLower()) { return $false }
    return $true
}

function Read-Manifest {
    param([string]$Text)
    $map = [ordered]@{}
    foreach ($line in $Text -split "`n") {
        $l = $line.TrimEnd("`r")
        if ($l -eq "") { continue }
        $m = [regex]::Match($l, '^([0-9a-fA-F]{64})\s+\*?(.+)$')
        if (-not $m.Success) { throw "bad manifest line: $l" }
        $map[$m.Groups[2].Value] = $m.Groups[1].Value.ToLower()
    }
    return $map
}

# --- hashes of local files, cached by size + last write so unchanged files aren't re-read ---
$cache = @{}
if (Test-Path -LiteralPath $HashCache) {
    foreach ($line in [IO.File]::ReadAllLines($HashCache, $Utf8)) {
        $p = $line.Split("`t")
        if ($p.Count -eq 4) { $cache[$p[3]] = $p }
    }
}
function Get-LocalHash {
    param([string]$Rel)
    $full = Join-Path $InstallDir ($Rel.Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return "" }
    $fi = Get-Item -LiteralPath $full
    $size = [string]$fi.Length; $stamp = [string]$fi.LastWriteTimeUtc.Ticks
    $c = $cache[$Rel]
    if ($c -and $c[1] -eq $size -and $c[2] -eq $stamp) { return $c[0] }
    $h = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLower()
    $cache[$Rel] = @($h, $size, $stamp, $Rel)
    return $h
}
function Save-HashCache {
    $lines = foreach ($k in $cache.Keys) { $cache[$k] -join "`t" }
    [IO.File]::WriteAllLines($HashCache, [string[]]@($lines), $Utf8)
}

try {
    # --- where to update from ---
    if ($Url -eq "") { $Url = Read-ConfigValue "zdon_update_url" }
    if ($Url -eq "") {
        $server = Read-ConfigValue "zdon_server_url"
        if ($server -ne "") { $Url = $server.TrimEnd('/') + "/update/windows" }
    }
    if ($Url -eq "") {
        # written by the server's publish step: the channel this install came from
        $urlFile = Join-Path $InstallDir "zdon_update.url"
        if (Test-Path -LiteralPath $urlFile) { $Url = [IO.File]::ReadAllText($urlFile, $Utf8).Trim() }
    }
    if ($Url -eq "") {
        Fail "no update source: set zdon_update_url or zdon_server_url under [network] in config.toml, or pass -Url" 2
    }
    $Url = $Url.TrimEnd('/')
    Log "Source: $Url"

    $remoteVersion = (Get-Text "$Url/version.txt").Trim()
    $manifestText  = Get-Text "$Url/checksums.sha256"
    if (-not $manifestText.EndsWith("`n")) { Fail "the server's checksums.sha256 is incomplete" }
    $manifest = Read-Manifest $manifestText
    $localVersion = if (Test-Path -LiteralPath $VersionFile) { [IO.File]::ReadAllText($VersionFile, $Utf8).Trim() } else { "" }
    Log "Installed: $(if ($localVersion) { $localVersion } else { '(unknown)' })  |  Server: $remoteVersion"

    # --- what changed ---
    $changed = New-Object System.Collections.Generic.List[string]
    foreach ($rel in $manifest.Keys) {
        if (-not (Test-SafePath $rel)) { Log "skipping unsafe path in manifest: $rel"; continue }
        if ($Force -or (Get-LocalHash $rel) -ne $manifest[$rel]) { $changed.Add($rel) }
    }
    $removed = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $ManifestFile) {
        $old = Read-Manifest ([IO.File]::ReadAllText($ManifestFile, $Utf8))
        foreach ($rel in $old.Keys) {
            if (-not $manifest.Contains($rel) -and (Test-SafePath $rel) -and
                (Test-Path -LiteralPath (Join-Path $InstallDir ($rel.Replace('/', '\'))))) { $removed.Add($rel) }
        }
    }
    Save-HashCache

    if ($changed.Count -eq 0 -and $removed.Count -eq 0) {
        [IO.File]::WriteAllText($VersionFile, $remoteVersion + "`n", $Utf8)
        [IO.File]::WriteAllText($ManifestFile, $manifestText, $Utf8)
        Log "Already up to date."
        if ($CheckOnly) { exit 0 }
        if ($Restart -and $WaitPid -ne "") {
            $proc = Get-Process -Id ([int]$WaitPid) -ErrorAction SilentlyContinue
            if ($proc) { $proc.WaitForExit() }
            Start-Process -FilePath (Join-Path $InstallDir "YataiDON.exe") -WorkingDirectory $InstallDir
        }
        exit 0
    }
    Log "$($changed.Count) file(s) to download, $($removed.Count) to remove."
    if ($CheckOnly) { exit 10 }

    # --- download into a staging folder on the same drive, verifying each file ---
    if (Test-Path -LiteralPath $StageDir) { Remove-Item -LiteralPath $StageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $StageDir | Out-Null
    $i = 0
    foreach ($rel in $changed) {
        $i++
        $dest = Join-Path $StageDir ($rel.Replace('/', '\'))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
        Log "[$i/$($changed.Count)] $rel"
        $ok = $false
        for ($try = 1; $try -le 3 -and -not $ok; $try++) {
            try {
                Invoke-WebRequest -Uri (Join-Url "$Url/files" $rel) -OutFile $dest -UseBasicParsing -TimeoutSec 600
                $ok = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLower() -eq $manifest[$rel]
                if (-not $ok) { Log "  checksum mismatch, retrying" }
            } catch { Log "  $($_.Exception.Message), retrying" }
        }
        if (-not $ok) { Fail "could not download $rel" }
    }

    # --- the game must be closed before its files are replaced ---
    if ($WaitPid -ne "") {
        $proc = Get-Process -Id ([int]$WaitPid) -ErrorAction SilentlyContinue
        if ($proc) { Log "Waiting for the game (PID $WaitPid) to close..."; $proc.WaitForExit() }
    }
    $exe = Join-Path $InstallDir "YataiDON.exe"
    for ($wait = 0; $wait -lt 60; $wait++) {
        $running = Get-Process -Name "YataiDON" -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
        if (-not $running) { break }
        if ($wait -eq 0) { Log "YataiDON.exe is running from this folder; waiting for it to close..." }
        Start-Sleep -Seconds 1
    }
    if ($running) { Fail "close YataiDON first" }

    # --- swap the files in ---
    foreach ($rel in $changed) {
        $src = Join-Path $StageDir ($rel.Replace('/', '\'))
        $dst = Join-Path $InstallDir ($rel.Replace('/', '\'))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        $moved = $false
        for ($try = 0; $try -lt 10 -and -not $moved; $try++) {
            try { Move-Item -LiteralPath $src -Destination $dst -Force; $moved = $true }
            catch { Start-Sleep -Milliseconds 500 }
        }
        if (-not $moved) { Fail "could not replace $rel (is it open in another program?)" }
    }
    foreach ($rel in $removed) {
        Remove-Item -LiteralPath (Join-Path $InstallDir ($rel.Replace('/', '\'))) -Force -ErrorAction SilentlyContinue
        Log "removed $rel"
    }
    Remove-Item -LiteralPath $StageDir -Recurse -Force -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText($ManifestFile, $manifestText, $Utf8)
    [IO.File]::WriteAllText($VersionFile, $remoteVersion + "`n", $Utf8)
    Log "Updated to $remoteVersion."

    if ($Restart) {
        Log "Starting Z-DON..."
        Start-Process -FilePath $exe -WorkingDirectory $InstallDir
    }
    exit 0
} catch {
    Fail $_.Exception.Message
}
