@echo off
cd /d "%~dp0"

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

rem All Chinese messages come from Python on purpose:
rem cmd.exe cannot reliably parse a UTF-8 batch file.
rem --default-state turns on the incremental manifest so re-runs skip finished work.
python prepare_sd.py --interactive --default-state

echo.
pause
