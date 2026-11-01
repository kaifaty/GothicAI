# Shared by the Windows entry points (setup.ps1, start.ps1, diagnostics.ps1, ..\build-engine.ps1):
# the repository root, the player's messages, uv, the build prerequisites and native calls.
#
# Windows PowerShell 5.1 compatible. The scripts stay ASCII: 5.1 reads a .ps1 without BOM in the ANSI code
# page, so the Russian text lives in messages.ru.json, read as UTF-8 explicitly.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$script:Messages = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'messages.ru.json') |
    ConvertFrom-Json
$script:LogRoot = Join-Path $env:LOCALAPPDATA 'GothicAI\Logs'
# Python children read and write UTF-8 instead of the ANSI code page: Russian paths and lines stay intact
$env:PYTHONUTF8 = '1'
# one interpreter for the launcher and the services (theirs is pinned by .python-version): uv downloads it
# once, on the first setup
$env:UV_PYTHON = '3.12'

function Get-Message([string]$Key, [object[]]$Values) {
    $entry = $script:Messages.PSObject.Properties[$Key]
    $text = if ($entry) { [string]$entry.Value } else { $Key }
    if ($null -ne $Values -and $Values.Count -gt 0) { return $text -f $Values }  # @(0) alone is falsy
    return $text
}

function Say {
    param(
        [Parameter(Mandatory = $true)] [string]$Key,
        [Parameter(ValueFromRemainingArguments = $true)] [object[]]$Values
    )
    Write-Host (Get-Message $Key $Values)
}

function Confirm-Step([string]$Key) {
    $answer = Read-Host (Get-Message $Key)
    return ($answer.Trim() -match (Get-Message 'yes_answers'))
}

function Format-Argument([string]$Value) {
    # the quoting CommandLineToArgvW and the C runtime undo: quotes around spaces, backslashes before a
    # quote doubled
    if ($Value -ne '' -and $Value -notmatch '[\s"]') { return $Value }
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

function Join-Arguments([string[]]$Arguments) {
    if ($null -eq $Arguments -or $Arguments.Count -eq 0) { return '' }
    return (@($Arguments | ForEach-Object { Format-Argument $_ }) -join ' ')
}

function Invoke-Console([string]$File, [string[]]$Arguments) {
    # The child writes to this console itself (prompts, hidden input, progress) and only it is waited
    # for: a browser it opens for a sign-in does not hold this script.
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    $info.Arguments = Join-Arguments $Arguments
    $info.UseShellExecute = $false
    $process = [System.Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    return $process.ExitCode
}

function Get-NativeOutput([string]$File, [string[]]$Arguments) {
    # stdout and stderr as text (UTF-8), without the exceptions Windows PowerShell raises for stderr lines
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $File
    $info.Arguments = Join-Arguments $Arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    try {
        $process = [System.Diagnostics.Process]::Start($info)
    } catch {
        return "(cannot run $File`: $($_.Exception.Message))"
    }
    $stderr = $process.StandardError.ReadToEndAsync()
    $stdout = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    return ($stdout + $stderr.Result).TrimEnd()
}

function Find-Uv {
    # the release archive's own pinned uv first (package.ps1), then an installed one
    $candidates = @(Join-Path $script:Root 'tools\uv\uv.exe')
    $command = Get-Command uv.exe -ErrorAction SilentlyContinue
    if ($command) { $candidates += $command.Source }
    $candidates += @(
        (Join-Path $env:USERPROFILE '.local\bin\uv.exe'),
        (Join-Path $env:USERPROFILE '.cargo\bin\uv.exe'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\uv.exe')
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            # the launcher finds it on PATH too when it starts the services with `uv run`
            $directory = Split-Path -Parent $candidate
            if (($env:PATH -split ';') -notcontains $directory) { $env:PATH = "$directory;$env:PATH" }
            return $candidate
        }
    }
    return $null
}

function Invoke-Launcher([string]$Uv, [string[]]$Arguments) {
    $project = Join-Path $script:Root 'launcher\core'
    return Invoke-Console $Uv (@('run', '--locked', '--project', $project, 'gothic-launcher') + $Arguments)
}

function Get-WindowsProblem {
    # $null on 64-bit Windows 10 or 11 on x64, else what is here instead
    $arch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    $version = [Environment]::OSVersion.Version
    if ($arch -eq 'AMD64' -and $version.Major -eq 10 -and $version.Build -ge 10240) { return $null }
    return "Windows $version, $arch"
}

function Show-PathWarnings {
    if ($script:Root -match '[^\x20-\x7E]') { Say 'path_non_ascii' $script:Root }
    if ($script:Root.Length -gt 60) { Say 'path_long' $script:Root.Length }
}

function Find-VisualStudio {
    # vcvars64.bat of the newest Visual Studio 2022 (or its Build Tools) with the x64 C++ compiler
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $found = Get-NativeOutput $vswhere @(
        '-latest', '-products', '*', '-version', '[17.0,)',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
    $path = @($found -split "`r?`n" | Where-Object { $_ -and (Test-Path -LiteralPath $_) }) | Select-Object -First 1
    if (-not $path) { return $null }
    $vcvars = Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $vcvars) { return $vcvars }
    return $null
}

function Find-VulkanSdk {
    $named = @($env:VULKAN_SDK,
        [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine'),
        [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'User'))
    $installed = @(Get-ChildItem -LiteralPath 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue |
        Sort-Object { $_.Name -as [version] } -Descending | ForEach-Object { $_.FullName })
    foreach ($candidate in $named + $installed) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'Bin\glslangValidator.exe'))) {
            return $candidate
        }
    }
    return $null
}

function Find-Git {
    $command = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @((Join-Path $env:ProgramFiles 'Git\cmd\git.exe'),
                             (Join-Path $env:LOCALAPPDATA 'Programs\Git\cmd\git.exe'))) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Get-BuildPrerequisites {
    # what the engine build still needs, each with its official winget package and page
    $missing = @()
    if (-not (Find-VisualStudio)) {
        $missing += [pscustomobject]@{
            Name = 'Visual Studio 2022 Build Tools: Desktop development with C++ (MSVC x64, Windows SDK, CMake, Ninja)'
            Winget = @('install', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '-e', '--override',
                       '--wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
            Page = 'https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022'
        }
    }
    if (-not (Find-VulkanSdk)) {
        $missing += [pscustomobject]@{
            Name = 'Vulkan SDK (LunarG)'
            Winget = @('install', '--id', 'KhronosGroup.VulkanSDK', '-e', '--interactive')
            Page = 'https://vulkan.lunarg.com/sdk/home#windows'
        }
    }
    if (-not (Find-Git)) {
        $missing += [pscustomobject]@{
            Name = 'Git for Windows'
            Winget = @('install', '--id', 'Git.Git', '-e', '--interactive')
            Page = 'https://git-scm.com/download/win'
        }
    }
    return $missing
}

function Import-VsEnvironment([string]$VcVars) {
    # the compiler's environment (PATH, INCLUDE, LIB...) as vcvars64.bat sets it, into this process
    $lines = & cmd.exe /d /s /c "`"$VcVars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed: $VcVars" }
    foreach ($line in $lines) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { Set-Item -LiteralPath "env:$($line.Substring(0, $i))" -Value $line.Substring($i + 1) }
    }
}
