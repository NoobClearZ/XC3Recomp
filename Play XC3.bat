@echo off
rem XC3Recomp: the launcher (graphics, resolution, controls, language), then the game.
cd /d "%~dp0"
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
start "" "xenoblade3_launcher.exe"
