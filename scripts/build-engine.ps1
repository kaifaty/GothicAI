<#
.SYNOPSIS
Builds this fork's game, with its AI bridge, and the story graph converter on Windows x64 (MSVC, Ninja).

.DESCRIPTION
The Windows counterpart of scripts/build-engine.sh, plus the converter the AI server and TTS run:
  OpenGothic\build\opengothic\Gothic2Notr.exe            the game; Tempest.dll, dxcompiler.dll, dxil.dll beside it
  OpenGothic\build\opengothic\content-convert.exe        the base and video converter the launcher runs first
  OpenGothic\ai_bridge\storygraph\build\storygraph.exe    the story graph converter (STORYGRAPH_TOOL)

Needs Visual Studio 2022 or its Build Tools with "Desktop development with C++" (MSVC x64, Windows SDK and
"C++ CMake tools", which bring CMake and Ninja), the Vulkan SDK (VULKAN_SDK, glslangValidator) and Git. The
first configure fetches IXWebSocket v12.0.1 (git, ai_bridge/CMakeLists.txt) and the DirectX shader compiler
(zip, SHA-256 pinned in lib/Tempest/Engine/CMakeLists.txt) from GitHub. Everything else is in the repository.
The game also needs vulkan-1.dll at run time, which graphics drivers and the Vulkan SDK install.

  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-engine.ps1
  ... -BuildType Release        (RelWithDebInfo by default; Debug is not supported by the engine's MSVC flags)
  ... -Jobs 4                   fewer parallel compilers on a PC with little memory
  ... -CheckOnly                only report missing prerequisites (exit 2 when something is missing)
#>
[CmdletBinding()]
param(
    [ValidateSet('RelWithDebInfo', 'Release')] [string]$BuildType = 'RelWithDebInfo',
    [int]$Jobs = 0,
    [switch]$CheckOnly
)

. (Join-Path $PSScriptRoot 'windows\common.ps1')

$missing = @(Get-BuildPrerequisites)
if ($missing.Count -gt 0) {
    foreach ($item in $missing) {
        Write-Host "missing: $($item.Name)"
        Write-Host "  winget $(Join-Arguments $item.Winget)"
        Write-Host "  or $($item.Page)"
    }
    exit 2
}
if ($CheckOnly) {
    Write-Host 'all build prerequisites found'
    exit 0
}

Import-VsEnvironment (Find-VisualStudio)
$vulkan = Find-VulkanSdk
$env:VULKAN_SDK = $vulkan  # Tempest reads its include and lib folders from here
$env:PATH = "$(Join-Path $vulkan 'Bin');$(Split-Path -Parent (Find-Git));$env:PATH"
foreach ($tool in 'cl.exe', 'cmake.exe', 'ninja.exe', 'glslangValidator.exe', 'git.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is not on PATH after loading Visual Studio, the Vulkan SDK and Git"
    }
}

function Invoke-Step([string[]]$Arguments) {
    Write-Host "> cmake $(Join-Arguments $Arguments)"
    $code = Invoke-Console (Get-Command cmake.exe).Source $Arguments
    if ($code -ne 0) { throw "cmake failed with exit code $code" }
}

$parallel = if ($Jobs -gt 0) { @('--parallel', "$Jobs") } else { @() }
# Ninja keeps compiling the other objects after an error (-k 0), so one failed build reports every
# broken file; the build still exits nonzero and Invoke-Step throws.
$keepGoing = @('--', '-k', '0')
$engine = Join-Path $Root 'OpenGothic'
$build = Join-Path $engine 'build'
Invoke-Step @('-S', $engine, '-B', $build, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$BuildType")
Invoke-Step (@('--build', $build, '--target', 'Gothic2Notr') + $parallel + $keepGoing)

$graph = Join-Path $engine 'ai_bridge\storygraph'
$graphBuild = Join-Path $graph 'build'
Invoke-Step @('-S', $graph, '-B', $graphBuild, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$BuildType")
Invoke-Step (@('--build', $graphBuild, '--target', 'storygraph') + $parallel + $keepGoing)

# The runtime output folder already holds the game, its converter (a dependency of the game target),
# Tempest.dll and the shader compiler DLLs Tempest copies.
$out = Join-Path $build 'opengothic'
$expected = @('Gothic2Notr.exe', 'content-convert.exe', 'Tempest.dll', 'dxcompiler.dll', 'dxil.dll' |
    ForEach-Object { Join-Path $out $_ })
$expected += Join-Path $graphBuild 'storygraph.exe'
$absent = @($expected | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($absent.Count -gt 0) { throw "the build finished without: $($absent -join ', ')" }

# This fork, not a stock OpenGothic: the AI bridge reads its server address from GOTHIC_AI_URL.
$game = Join-Path $out 'Gothic2Notr.exe'
if (-not [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($game)).Contains('GOTHIC_AI_URL')) {
    throw "$game has no AI bridge (GOTHIC_AI_URL not found)"
}
if (-not (Test-Path -LiteralPath (Join-Path $env:SystemRoot 'System32\vulkan-1.dll'))) {
    Write-Warning 'vulkan-1.dll is missing: update the graphics driver or install the Vulkan Runtime; the game cannot start without it'
}
Write-Host "built ($BuildType):"
$expected | ForEach-Object { Write-Host "  $_" }
