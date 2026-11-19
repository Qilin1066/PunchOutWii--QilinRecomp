@echo off
rem Update Punch-Out!!: double-click. Gets the latest code from GitHub,
rem rebuilds what changed, and starts the game.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build-punchout.ps1" -Pull -Run
if errorlevel 1 (
    echo.
    echo The update stopped. The message above says where; copy it for Claude.
    pause
)
