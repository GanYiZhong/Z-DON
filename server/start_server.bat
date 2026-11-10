@echo off
rem Z-DON server: python 3.11+ on PATH (py launcher or python.exe)
cd /d "%~dp0"
where py >nul 2>nul && (py -3 zdon_server.py serve %* & exit /b %ERRORLEVEL%)
python zdon_server.py serve %*
