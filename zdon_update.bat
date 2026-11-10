@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0zdon_update.ps1" %*
exit /b %ERRORLEVEL%
