<#
.SYNOPSIS
Starts the AI services and the game (Start-GothicAI.cmd in the repository root).

.DESCRIPTION
`gothic-launcher setup --check` first (what is missing, in plain Russian), then `gothic-launcher up`: the
services on free localhost ports, the game with the newest save (or its menu). Closing the game stops
everything; so does Ctrl-C here, and closing this window ends every process the launcher started.

  Start-GothicAI.cmd          continue the newest save, or the menu when there is none
  Start-GothicAI.cmd -New     a new game at once
#>
[CmdletBinding()]
param([switch]$New)

. (Join-Path $PSScriptRoot 'common.ps1')

$uv = Find-Uv
if (-not $uv) {
    Say 'run_setup_first'
    exit 2
}
$code = Invoke-Launcher $uv @('setup', '--check')
if ($code -ne 0) { exit $code }

Say 'starting'
$mode = if ($New) { '--new' } else { '--continue' }
$code = Invoke-Launcher $uv @('up', $mode)
if ($code -eq 0 -or $code -eq 130) { exit 0 }  # the game closed, or Ctrl-C
Say 'game_failed' $code $LogRoot
exit $code
