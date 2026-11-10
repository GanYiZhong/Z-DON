# Registers the Z-DON server to start when Windows starts (Task Scheduler), whether or not anyone is logged in.
# Run once from an administrator PowerShell:
#     powershell -ExecutionPolicy Bypass -File server\install_autostart.ps1
# Remove it again with:  Unregister-ScheduledTask -TaskName "Z-DON server" -Confirm:$false

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = (Get-Command py -ErrorAction SilentlyContinue).Source
$argLine = "-3 `"$here\zdon_server.py`" serve"
if (-not $python) {
    $python = (Get-Command python -ErrorAction Stop).Source
    $argLine = "`"$here\zdon_server.py`" serve"
}

$action = New-ScheduledTaskAction -Execute $python -Argument $argLine -WorkingDirectory $here
$trigger = New-ScheduledTaskTrigger -AtStartup
# restart after a crash: every minute, up to 999 times; no time limit
$settings = New-ScheduledTaskSettingsSet -RestartInterval (New-TimeSpan -Minutes 1) -RestartCount 999 `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType S4U -RunLevel Limited

Register-ScheduledTask -TaskName "Z-DON server" -Action $action -Trigger $trigger -Settings $settings `
    -Principal $principal -Description "zdon.taiko.ac / Z-DON game server (server\zdon_server.py)" -Force | Out-Null
Start-ScheduledTask -TaskName "Z-DON server"
Write-Host "Registered and started the 'Z-DON server' task ($python $argLine)."
