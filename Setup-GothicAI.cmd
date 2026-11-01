@echo off
rem Gothic AI on Windows: first run and repair (scripts\windows\setup.ps1). Safe to run again.
rem The execution policy is bypassed for this one PowerShell process only; nothing is changed system-wide.
setlocal
set "PYTHONUTF8=1"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\setup.ps1" %*
set "code=%ERRORLEVEL%"
pause
exit /b %code%
