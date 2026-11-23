@echo off
rem Gothic AI on Windows: the AI services and the game (scripts\windows\start.ps1).
rem The execution policy is bypassed for this one PowerShell process only; nothing is changed system-wide.
setlocal
set "PYTHONUTF8=1"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\start.ps1" %*
set "code=%ERRORLEVEL%"
if not "%code%"=="0" pause
exit /b %code%
