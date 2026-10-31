<#
.SYNOPSIS
Packs the prebuilt Windows release into one ZIP, with its build manifest and SHA256SUMS.

.DESCRIPTION
Runs after scripts\build-engine.ps1 and reads that build's outputs. The archive holds what a player runs,
copied by whitelist into the repository's own layout, so the launcher finds everything where it looks:
  Start-GothicAI.cmd, Setup-GothicAI.cmd, Diagnostics-GothicAI.cmd, Prepare-GothicAI-Game.cmd,
  Check-Microphone.cmd, START_HERE.ru.md, LICENSE
  scripts\windows\                       the entry points' PowerShell and Python helpers, Russian messages
  launcher\core, server, asr-service, tts-service, libs\chatgpt-auth
                                         sources, pyproject.toml, uv.lock, .python-version (no tests)
  OpenGothic\build\opengothic\           Gothic2Notr.exe, content-convert.exe and every DLL of that folder
  OpenGothic\ai_bridge\storygraph\build\ storygraph.exe
  tools\uv\                              the pinned official uv.exe and its licenses
  licenses\                              the licenses of everything compiled in or shipped
The Microsoft C++ runtime DLLs go beside both executables (app-local, from the Visual Studio redist
folder). Python and the locked wheels are not packed: Setup downloads them once through uv.

The archive is refused when something private or build-only slipped in, when a binary is not x64, or when
a DLL that a packed binary imports (dumpbin /dependents) is neither packed beside it nor part of Windows.

  scripts\windows\package.ps1 -UvZip <uv-x86_64-pc-windows-msvc.zip> -UvLicenses <folder> -OutDir <folder>
                              -Name <archive base name> [-Repository <url>] [-Commit <sha>] [-RunUrl <url>]
                              [-VulkanSdkVersion <v>] [-UvVersion <v>]
-Repository is the public source repository the archive names as its corresponding source (CI passes the
one it runs in).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$UvZip,
    [Parameter(Mandatory = $true)] [string]$UvLicenses,
    [Parameter(Mandatory = $true)] [string]$OutDir,
    [Parameter(Mandatory = $true)] [string]$Name,
    [string]$Repository = 'https://github.com/kaifaty/GothicAI',
    [string]$Commit = '',
    [string]$RunUrl = '',
    [string]$VulkanSdkVersion = '',
    [string]$UvVersion = ''
)

. (Join-Path $PSScriptRoot 'common.ps1')
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$PythonProjects = @('launcher\core', 'server', 'asr-service', 'tts-service', 'libs\chatgpt-auth')
$Game = 'OpenGothic\build\opengothic'
$Storygraph = 'OpenGothic\ai_bridge\storygraph\build'
$LicenseName = '^(licen[cs]e|copying|notice|unlicense)([.\-_].*)?$|\.license$'
# build-only files, game data, saves, settings, keys, sign-ins, venvs, tests and the internal port briefs
$Forbidden = '(?i)(\.(pdb|ilk|obj|lib|exp|pyc|pyo|sav|vdf|zen|log)$)' +
    '|(^|\\)(\.env[^\\]*|secrets\.toml|settings\.toml|chatgpt\.json|launcher\.json|ou\.bin|gothic\.dat' +
    '|\.venv|__pycache__|\.git|tests|(?-i:PORT_[^\\]*\.md))(\\|$)'

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$stage = Join-Path $OutDir $Name
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

function Copy-Into([string]$Source, [string]$Relative) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "missing: $Source" }
    $target = Join-Path $stage $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target
}

function Copy-FromRepo([string]$Relative) { Copy-Into (Join-Path $Root $Relative) $Relative }

function Get-Relative([string]$Path, [string]$Base) { return $Path.Substring($Base.TrimEnd('\').Length + 1) }

# the entry points and the player's guide
foreach ($file in 'Start-GothicAI.cmd', 'Setup-GothicAI.cmd', 'Diagnostics-GothicAI.cmd',
                  'Prepare-GothicAI-Game.cmd', 'Check-Microphone.cmd', 'START_HERE.ru.md',
                  'LICENSE', 'scripts\windows\common.ps1', 'scripts\windows\setup.ps1',
                  'scripts\windows\start.ps1', 'scripts\windows\diagnostics.ps1',
                  'scripts\windows\microphone-check.ps1', 'scripts\windows\game_profile.py',
                  'scripts\windows\messages.ru.json') {
    Copy-FromRepo $file
}

# the AI services and the launcher: what `uv sync --locked` and `uv run` need, without tests or caches
foreach ($project in $PythonProjects) {
    foreach ($file in 'pyproject.toml', 'uv.lock', '.python-version', 'README.md') {
        if (Test-Path -LiteralPath (Join-Path $Root "$project\$file")) { Copy-FromRepo "$project\$file" }
    }
    $src = Join-Path $Root "$project\src"
    Get-ChildItem -LiteralPath $src -Recurse -File -Force |
        Where-Object { (Get-Relative $_.FullName $Root) -notmatch $Forbidden } |
        ForEach-Object { Copy-FromRepo (Get-Relative $_.FullName $Root) }
}

# the game, its base and video converter, and every DLL of their output folder (Tempest, the DirectX
# shader compiler...)
$gameDir = Join-Path $Root $Game
Copy-FromRepo "$Game\Gothic2Notr.exe"
Copy-FromRepo "$Game\content-convert.exe"
Get-ChildItem -LiteralPath $gameDir -Filter '*.dll' -File | ForEach-Object { Copy-FromRepo "$Game\$($_.Name)" }
Copy-FromRepo "$Storygraph\storygraph.exe"
Get-ChildItem -LiteralPath (Join-Path $Root $Storygraph) -Filter '*.dll' -File |
    ForEach-Object { Copy-FromRepo "$Storygraph\$($_.Name)" }

# dumpbin and the C++ runtime come with the Visual Studio environment
if (-not (Get-Command dumpbin.exe -ErrorAction SilentlyContinue) -or -not $env:VCToolsRedistDir) {
    $vcvars = Find-VisualStudio
    if (-not $vcvars) { throw 'Visual Studio with the x64 C++ tools is needed for dumpbin and the C++ runtime' }
    Import-VsEnvironment $vcvars
}
$crt = @(Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
    Sort-Object Name -Descending) | Select-Object -First 1
if (-not $crt) { throw "no Microsoft.VC*.CRT under $env:VCToolsRedistDir\x64" }
$runtime = @(Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' -File)
foreach ($dll in $runtime) {
    Copy-Into $dll.FullName "$Game\$($dll.Name)"
    Copy-Into $dll.FullName "$Storygraph\$($dll.Name)"
}

# uv, pinned and checked by the caller
$uvTarget = Join-Path $stage 'tools\uv\uv.exe'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $uvTarget) | Out-Null
$archive = [System.IO.Compression.ZipFile]::OpenRead($UvZip)
try {
    $entry = @($archive.Entries | Where-Object { $_.Name -eq 'uv.exe' }) | Select-Object -First 1
    if (-not $entry) { throw "no uv.exe in $UvZip" }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $uvTarget, $true)
} finally {
    $archive.Dispose()
}
foreach ($license in 'LICENSE-MIT', 'LICENSE-APACHE') {
    Copy-Into (Join-Path $UvLicenses $license) "tools\uv\$license"
}
foreach ($dll in $runtime) { Copy-Into $dll.FullName "tools\uv\$($dll.Name)" }  # whichever runtime uv links

# licenses: the engine, its vendored libraries, what CMake fetched, the C++ runtime
$licenses = New-Object System.Collections.Generic.List[string]
function Add-License([string]$Source, [string]$Relative) {
    Copy-Into $Source "licenses\$Relative"
    $licenses.Add($Relative)
}
Add-License (Join-Path $Root 'OpenGothic\LICENSE') 'OpenGothic\LICENSE'
Add-License (Join-Path $Root 'OpenGothic\lib\VENDORED.md') 'OpenGothic\lib\VENDORED.md'
$lib = Join-Path $Root 'OpenGothic\lib'
Get-ChildItem -LiteralPath $lib -Recurse -File | Where-Object { $_.Name -match $LicenseName } |
    ForEach-Object { Add-License $_.FullName "OpenGothic\lib\$(Get-Relative $_.FullName $lib)" }
foreach ($fetched in 'ixwebsocket', 'directxshadercompiler') {  # linked into the game, or shipped beside it
    $dep = Join-Path $Root "OpenGothic\build\_deps\$fetched-src"
    $found = @(Get-ChildItem -LiteralPath $dep -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $LicenseName })
    if ($found.Count -eq 0) { throw "no license file in $dep" }
    $found | ForEach-Object { Add-License $_.FullName "$fetched\$(Get-Relative $_.FullName $dep)" }
}
$vcNote = Join-Path $OutDir 'Microsoft-CPP-Runtime.txt'
[System.IO.File]::WriteAllLines($vcNote, @(
    "$(($runtime | ForEach-Object { $_.Name }) -join ', ')",
    "are the Microsoft Visual C++ Redistributable files of Visual Studio 2022 ($($crt.Name), $env:VCToolsRedistDir),",
    'deployed app-locally beside the executables and redistributed under the Microsoft Software License',
    'Terms for Visual Studio (Distributable Code). The official installer of the same runtime:',
    'https://aka.ms/vs/17/release/vc_redist.x64.exe'
))
Add-License $vcNote 'Microsoft-CPP-Runtime.txt'
Remove-Item -LiteralPath $vcNote
$source = Join-Path $OutDir 'SOURCE.txt'
[System.IO.File]::WriteAllLines($source, @(
    "Built from $Repository at commit $Commit.",
    'The complete corresponding source of every binary here, including Tempest.dll with OpenAL Soft',
    '(LGPL, OpenGothic\lib\Tempest\Engine\thirdparty\openal-soft\COPYING), is that commit of the repository;',
    'scripts\build-engine.ps1 rebuilds it.'
))
Add-License $source 'SOURCE.txt'
Remove-Item -LiteralPath $source

# refuse anything private or build-only
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
$bad = @($files | Where-Object { (Get-Relative $_.FullName $stage) -match $Forbidden })
if ($bad.Count -gt 0) { throw "not for the archive: $(($bad | ForEach-Object { Get-Relative $_.FullName $stage }) -join ', ')" }

# x64 binaries whose every imported DLL is packed beside them or part of Windows
function Test-X64([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { return $false }  # MZ
        $stream.Position = 0x3C
        $stream.Position = $reader.ReadInt32()
        if ($reader.ReadUInt32() -ne 0x00004550) { return $false }  # PE\0\0
        return ($reader.ReadUInt16() -eq 0x8664)  # IMAGE_FILE_MACHINE_AMD64
    } finally {
        $stream.Dispose()
    }
}
$system32 = Join-Path $env:SystemRoot 'System32'
$binaries = @()
foreach ($binary in @($files | Where-Object { $_.Extension -in '.exe', '.dll' })) {
    $relative = Get-Relative $binary.FullName $stage
    if (-not (Test-X64 $binary.FullName)) { throw "not an x64 binary: $relative" }
    $imports = @()
    $listing = Get-NativeOutput 'dumpbin.exe' @('/nologo', '/dependents', $binary.FullName)
    foreach ($dll in @($listing -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[\w.\-]+\.dll$' })) {
        $from = if (Test-Path -LiteralPath (Join-Path $binary.DirectoryName $dll)) { 'packed' }
            elseif ($dll -match '^(msvcp|vcruntime|concrt|vccorlib)\d') { $null }  # app-local only
            elseif ($dll -match '^(api|ext)-ms-win-') { 'windows' }
            elseif ($dll -ieq 'vulkan-1.dll') { 'graphics driver' }
            elseif (Test-Path -LiteralPath (Join-Path $system32 $dll)) { 'windows' }
            else { $null }
        if (-not $from) { throw "$relative imports $dll, which is neither packed beside it nor part of Windows" }
        $imports += [ordered]@{ dll = $dll; from = $from }
    }
    $binaries += [ordered]@{
        path = $relative.Replace('\', '/')
        bytes = $binary.Length
        sha256 = (Get-FileHash -LiteralPath $binary.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        imports = $imports
    }
}
foreach ($needed in 'OpenGothic/build/opengothic/Gothic2Notr.exe', 'OpenGothic/build/opengothic/content-convert.exe',
                    'OpenGothic/ai_bridge/storygraph/build/storygraph.exe') {
    if (-not ($binaries | Where-Object { $_.path -eq $needed })) { throw "$needed is not in the archive" }
}

# the archive, its manifest and their checksums
$zip = Join-Path $OutDir "$Name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
$check = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @($check.Entries | ForEach-Object { $_.FullName })
} finally {
    $check.Dispose()
}
foreach ($needed in 'Start-GothicAI.cmd', 'Setup-GothicAI.cmd', 'Prepare-GothicAI-Game.cmd', 'Check-Microphone.cmd',
                    'scripts/windows/game_profile.py', 'scripts/windows/microphone-check.ps1', 'tools/uv/uv.exe',
                    'OpenGothic/build/opengothic/Gothic2Notr.exe', 'OpenGothic/build/opengothic/content-convert.exe',
                    'OpenGothic/ai_bridge/storygraph/build/storygraph.exe', 'server/uv.lock') {
    if ($entries -notcontains "$Name/$needed") { throw "the archive lacks $Name/$needed" }
}
$manifest = [ordered]@{
    name = $Name
    repository = $Repository
    commit = $Commit
    run = $RunUrl
    built_utc = (Get-Date).ToUniversalTime().ToString('o')
    msvc_tools = $env:VCToolsVersion
    msvc_runtime = $crt.Name
    vulkan_sdk = $VulkanSdkVersion
    uv = $UvVersion
    files = $entries.Count
    binaries = $binaries
    licenses = @($licenses | ForEach-Object { $_.Replace('\', '/') })
    verified = 'built and unit-tested on windows-2022; gameplay, graphics and microphone not yet tested on a real PC'
}
$manifestFile = Join-Path $OutDir "$Name.manifest.json"
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($manifestFile, ($manifest | ConvertTo-Json -Depth 6), $utf8)
$sums = foreach ($file in $zip, $manifestFile) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path -Leaf $file)
}
[System.IO.File]::WriteAllLines((Join-Path $OutDir 'SHA256SUMS'), [string[]]$sums, $utf8)
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host "packed $($entries.Count) files:"
Get-ChildItem -LiteralPath $OutDir -File | ForEach-Object { Write-Host ('  {0} ({1:N0} bytes)' -f $_.Name, $_.Length) }
