@echo off
rem Z-DON: render a chart to an MP4 without playing it (src/libs/zdon_record.h).
rem   record_video.bat <song.tja> [difficulty] [auto^|nohit] [out.mp4]
rem difficulty: 0 easy, 1 normal, 2 hard, 3 oni, 4 ura (default 3)
rem mode: auto = auto play (default), nohit = no hits at all
rem out: default Records\<song>_<difficulty>_<mode>.mp4 next to YataiDON.exe
rem Drag a .tja onto this file for oni on auto. Needs ffmpeg on PATH (or ZDON_FFMPEG).
setlocal
if "%~1"=="" (
    echo usage: %~nx0 ^<song.tja^> [difficulty 0-4] [auto^|nohit] [out.mp4]
    pause
    exit /b 1
)
set "SONG=%~f1"
set "DIFF=%~2"
if "%DIFF%"=="" set "DIFF=3"
set "MODE=%~3"
if "%MODE%"=="" set "MODE=auto"
set "EXE=%~dp0..\build\bin\YataiDON.exe"
if exist "%~dp0YataiDON.exe" set "EXE=%~dp0YataiDON.exe"
for %%E in ("%EXE%") do set "RECORDS=%%~dpERecords"
set "OUT=%~4"
if "%OUT%"=="" set "OUT=%RECORDS%\%~n1_%DIFF%_%MODE%.mp4"

set "AUTO="
if /i "%MODE%"=="auto" set "AUTO=--auto"

echo recording "%SONG%" difficulty %DIFF% (%MODE%) to "%OUT%" ...
"%EXE%" "%SONG%" %DIFF% %AUTO% --record "%OUT%"
if errorlevel 1 (
    echo failed: see record.log next to YataiDON.exe
    pause
    exit /b 1
)
echo done: "%OUT%"
