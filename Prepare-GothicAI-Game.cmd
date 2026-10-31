@echo off
rem Gothic AI on Windows: a separate base-game folder for an installation with mods
rem (scripts\windows\game_profile.py), then game.dir points at it. Hard links to the original archives and
rem junctions to _work, system and Miles; no game file is copied or changed, the mods stay where they are.
rem   Prepare-GothicAI-Game.cmd "<Gothic II folder>"
setlocal
set "PYTHONUTF8=1"
set "UV_PYTHON=3.12"
set "uv=%~dp0tools\uv\uv.exe"
if not exist "%uv%" set "uv=uv"
"%uv%" run --locked --project "%~dp0launcher\core" python "%~dp0scripts\windows\game_profile.py" --set-config %*
exit /b %ERRORLEVEL%
