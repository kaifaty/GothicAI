@echo off
rem Gothic AI on Windows: can the game hear the microphone (scripts\windows\microphone-check.ps1). Read-only:
rem records nothing and changes no setting.
rem The execution policy is bypassed for this one PowerShell process only; nothing is changed system-wide.
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\microphone-check.ps1" %*
set "code=%ERRORLEVEL%"
pause
exit /b %code%
