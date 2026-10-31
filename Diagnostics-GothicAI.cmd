@echo off
rem Gothic AI on Windows: one text file about this PC and this install, for Codex (scripts\windows\diagnostics.ps1).
rem The execution policy is bypassed for this one PowerShell process only; nothing is changed system-wide.
setlocal
set "PYTHONUTF8=1"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\diagnostics.ps1" %*
set "code=%ERRORLEVEL%"
pause
exit /b %code%
