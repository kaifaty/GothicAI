<#
.SYNOPSIS
First run of Gothic AI on Windows; safe to run again (Setup-GothicAI.cmd in the repository root).

.DESCRIPTION
Checks the PC, offers to install what is missing through winget (each installer asks for itself), builds
this fork's game and story graph converter when they are not built yet, prepares the AI services' Python
environments exactly as locked, then `gothic-launcher setup`: the game folder (found in Steam, else a folder
picker), the remote-only profile, the Inworld key (hidden input) and the ChatGPT sign-in in the browser.
Existing settings, keys, saves and builds stay; only what is missing is done. Nothing runs as administrator
except the installers Windows asks about.

  Setup-GothicAI.cmd [-GameDir <folder>] [-ChangeKey] [-Rebuild] [-SkipBuild]
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [switch]$ChangeKey,
    [switch]$Rebuild,
    [switch]$SkipBuild
)

. (Join-Path $PSScriptRoot 'common.ps1')

function Install-WithWinget([object[]]$Items) {
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) {
        Say 'no_winget'
        return $false
    }
    foreach ($item in $Items) {
        Write-Host "> winget $(Join-Arguments $item.Winget)"
        $code = Invoke-Console $winget.Source $item.Winget
        if ($code -ne 0) { Write-Host "winget: exit code $code" }
    }
    return $true
}

function Select-GameFolder {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = Get-Message 'pick_game'
        $dialog.ShowNewFolderButton = $false
        $steam = Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common'
        if (Test-Path -LiteralPath $steam) { $dialog.SelectedPath = $steam }
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
        return $null
    } catch {
        $typed = Read-Host (Get-Message 'pick_game_typed')  # no dialog (e.g. a remote console): type it
        if ($typed) { return $typed.Trim().Trim('"') }
        return $null
    }
}

Say 'setup_title'
$problem = Get-WindowsProblem
if ($problem) {
    Say 'windows_unsupported' $problem
    exit 2
}
if (Test-Path -LiteralPath (Join-Path $Root 'OpenGothic\CMakeLists.txt')) { Show-PathWarnings }  # a build minds the path

# uv brings Python and the services' libraries
$uv = Find-Uv
if (-not $uv) {
    Say 'uv_missing'
    if ((Get-Command winget.exe -ErrorAction SilentlyContinue) -and (Confirm-Step 'uv_install_ask')) {
        Install-WithWinget @([pscustomobject]@{ Winget = @('install', '--id', 'astral-sh.uv', '-e') }) | Out-Null
        $uv = Find-Uv
    }
    if (-not $uv) {
        Say 'uv_manual'
        exit 2
    }
}

# the game, its base and video converter, and the story graph converter, built here from this fork with
# its AI bridge
$game = Join-Path $Root 'OpenGothic\build\opengothic\Gothic2Notr.exe'
$converter = Join-Path $Root 'OpenGothic\build\opengothic\content-convert.exe'
$storygraph = Join-Path $Root 'OpenGothic\ai_bridge\storygraph\build\storygraph.exe'
$built = (Test-Path -LiteralPath $game) -and (Test-Path -LiteralPath $converter) -and
    (Test-Path -LiteralPath $storygraph)
$sources = Test-Path -LiteralPath (Join-Path $Root 'OpenGothic\CMakeLists.txt')
if (-not $built -and -not $sources) {
    Say 'package_incomplete'  # the release archive is prebuilt: without its game there is nothing to build
    exit 2
}
if (-not $SkipBuild -and $sources -and ($Rebuild -or -not $built)) {
    $missing = @(Get-BuildPrerequisites)
    if ($missing.Count -gt 0) {
        Say 'build_tools_missing'
        foreach ($item in $missing) {
            Write-Host "  - $($item.Name)"
            Write-Host "    winget $(Join-Arguments $item.Winget)"
            Write-Host "    $($item.Page)"
        }
        if (-not (Confirm-Step 'build_tools_install_ask')) {
            Say 'build_tools_manual'
            exit 2
        }
        if (-not (Install-WithWinget $missing)) { exit 2 }
        if (@(Get-BuildPrerequisites).Count -gt 0) {
            Say 'build_tools_restart'
            exit 2
        }
    }
    Say 'building'
    $script = Join-Path $Root 'scripts\build-engine.ps1'
    $code = Invoke-Console 'powershell.exe' @('-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script)
    if ($code -ne 0) {
        Say 'build_failed'
        exit 1
    }
}

# the AI services' environments, exactly as locked
Say 'syncing'
foreach ($project in 'launcher\core', 'server', 'asr-service', 'tts-service') {
    $code = Invoke-Console $uv @('sync', '--locked', '--project', (Join-Path $Root $project))
    if ($code -ne 0) {
        Say 'sync_failed' $project
        exit 1
    }
}

# the game folder, the remote-only profile, the Inworld key and the ChatGPT sign-in
$options = @()
if ($ChangeKey) { $options += '--change-key' }
$folder = @()
if ($GameDir) { $folder = @('--game-dir', $GameDir) }
$code = Invoke-Launcher $uv (@('setup') + $folder + $options)
if ($code -eq 3) {
    $picked = Select-GameFolder
    if (-not $picked) {
        Say 'no_game_folder'
        exit 2
    }
    $code = Invoke-Launcher $uv (@('setup', '--game-dir', $picked) + $options)
}
if ($code -eq 0) { Say 'setup_done' } else { Say 'setup_incomplete' }
exit $code
