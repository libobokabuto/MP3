@echo off
cd /d "%~dp0"

rem ===========================================================================
rem  MP3 player - ncm/mp3 to SD pipeline (one double-click)
rem
rem  ASCII ONLY. Every Chinese message is printed by Python on purpose:
rem  cmd.exe cannot reliably parse a UTF-8 batch file, Chinese lines get
rem  treated as commands and blow up. Keep this file ASCII.
rem ===========================================================================

where python >nul 2>nul
if errorlevel 1 (
    echo.
    echo  [ERROR] Python not found in PATH.
    echo  Install Python 3 first: https://www.python.org/downloads/
    echo  Remember to tick "Add python.exe to PATH" during install.
    echo.
    pause
    exit /b 1
)

rem --confirm prints the plan first, then asks before writing anything.
rem Do NOT put --yes here: a double-click must never write to the music/SD
rem directories without the user seeing the plan first.
python pipeline.py --confirm

echo.
pause
