<#
.SYNOPSIS
Writes what Codex needs to judge this PC and this install into one text file (Diagnostics-GothicAI.cmd).

.DESCRIPTION
Windows, CPU, memory, graphics adapters and drivers, Vulkan and DirectX, microphones and the microphone
permission, the build tools, the build outputs, the launcher's settings (keys shown only as <set>), what
`setup --check` still misses, and where the newest logs are. It never opens secrets.toml or chatgpt.json:
for them only whether they exist. Log contents stay in their folder.

  Diagnostics-GothicAI.cmd [-NoDxDiag]
#>
[CmdletBinding()]
param([switch]$NoDxDiag)

. (Join-Path $PSScriptRoot 'common.ps1')

Say 'diagnostics_running'
$report = New-Object System.Collections.Generic.List[string]
function Add-Section([string]$Title) { $report.Add(''); $report.Add("## $Title") }
function Add-Line([string]$Text) { $report.Add($Text) }
function Add-Object($Value) { if ($Value) { $report.Add(($Value | Format-List | Out-String).Trim()) } }

$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
Add-Line "Gothic AI diagnostics $stamp"
Add-Line "repository: $Root"

Add-Section 'Windows'
Add-Object (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture, MUILanguages)
Add-Line "PowerShell: $($PSVersionTable.PSVersion); problem: $(Get-WindowsProblem)"

Add-Section 'CPU and memory'
Add-Object (Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed)
$memory = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
Add-Line ('memory: {0:N1} GB' -f ($memory / 1GB))
Add-Object (Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($Root.Substring(0, 2))'" |
    Select-Object DeviceID, @{ n = 'FreeGB'; e = { [math]::Round($_.FreeSpace / 1GB, 1) } })

Add-Section 'Graphics'
Add-Object (Get-CimInstance Win32_VideoController |
    Select-Object Name, DriverVersion, DriverDate, VideoProcessor, CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate)
$loader = Join-Path $env:SystemRoot 'System32\vulkan-1.dll'
Add-Line "vulkan-1.dll: $(if (Test-Path -LiteralPath $loader) { (Get-Item -LiteralPath $loader).VersionInfo.FileVersion } else { 'missing' })"
$vulkan = Find-VulkanSdk
$vulkaninfo = if ($vulkan) { Join-Path $vulkan 'Bin\vulkaninfo.exe' } else { $null }
if (-not ($vulkaninfo -and (Test-Path -LiteralPath $vulkaninfo))) {
    $command = Get-Command vulkaninfo.exe -ErrorAction SilentlyContinue
    $vulkaninfo = if ($command) { $command.Source } else { $null }
}
if ($vulkaninfo) {
    Add-Line 'vulkaninfo --summary:'
    $summary = Get-NativeOutput $vulkaninfo @('--summary')
    Add-Line ((($summary -split "`r?`n") | Select-Object -First 80) -join "`n")
} else {
    Add-Line 'vulkaninfo: not found (it comes with the Vulkan SDK)'
}
if (-not $NoDxDiag) {
    $dx = Join-Path $env:TEMP "gothic-ai-dxdiag-$stamp.txt"
    $dxdiag = Start-Process -FilePath 'dxdiag.exe' -ArgumentList "/whql:off /t `"$dx`"" -PassThru -WindowStyle Hidden
    if ($dxdiag.WaitForExit(90000) -and (Test-Path -LiteralPath $dx)) {
        $wanted = 'Operating System|DirectX Version|Card name|Driver Version|Driver Model|Feature Levels|Dedicated Memory|Display Memory'
        Add-Line 'dxdiag:'
        Add-Line ((Get-Content -LiteralPath $dx | Where-Object { $_ -match $wanted } | ForEach-Object { $_.Trim() }) -join "`n")
        Remove-Item -LiteralPath $dx -ErrorAction SilentlyContinue
    } else {
        if (-not $dxdiag.HasExited) { $dxdiag.Kill() }
        Add-Line 'dxdiag: no answer within 90 s'
    }
}

Add-Section 'Audio'
Add-Object (Get-CimInstance Win32_SoundDevice | Select-Object Name, Status)
$consent = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone'
foreach ($key in @($consent, (Join-Path $consent 'NonPackaged'))) {
    $value = (Get-ItemProperty -LiteralPath $key -Name Value -ErrorAction SilentlyContinue)
    Add-Line "microphone permission $(Split-Path -Leaf $key): $(if ($value) { $value.Value } else { 'not set' })"
}

Add-Section 'Tools'
$uv = Find-Uv
Add-Line "uv: $(if ($uv) { "$uv, $(Get-NativeOutput $uv @('--version'))" } else { 'missing' })"
$git = Find-Git
Add-Line "git: $(if ($git) { Get-NativeOutput $git @('--version') } else { 'missing' })"
Add-Line "Visual Studio (vcvars64): $(if ($vcvars = Find-VisualStudio) { $vcvars } else { 'missing' })"
Add-Line "Vulkan SDK: $(if ($vulkan) { $vulkan } else { 'missing' })"
Add-Line "winget: $(if (Get-Command winget.exe -ErrorAction SilentlyContinue) { 'present' } else { 'missing' })"

Add-Section 'Build outputs'
$outputs = @('OpenGothic\build\opengothic\Gothic2Notr.exe', 'OpenGothic\build\opengothic\content-convert.exe',
             'OpenGothic\build\opengothic\Tempest.dll', 'OpenGothic\build\opengothic\dxcompiler.dll',
             'OpenGothic\build\opengothic\dxil.dll', 'OpenGothic\ai_bridge\storygraph\build\storygraph.exe')
foreach ($relative in $outputs) {
    $path = Join-Path $Root $relative
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path
        Add-Line ('{0}: {1:N0} bytes, {2:yyyy-MM-dd HH:mm}' -f $relative, $item.Length, $item.LastWriteTime)
    } else {
        Add-Line "${relative}: missing"
    }
}

Add-Section 'Gothic AI'
$config = Join-Path $env:USERPROFILE '.config\gothic-ai'
foreach ($name in 'settings.toml', 'secrets.toml', 'chatgpt.json') {
    Add-Line "$name exists: $(Test-Path -LiteralPath (Join-Path $config $name))"
}
if ($uv) {
    $project = Join-Path $Root 'launcher\core'
    Add-Line 'config show (keys only as <set>):'
    Add-Line (Get-NativeOutput $uv @('run', '--locked', '--project', $project, 'gothic-launcher', 'config', 'show'))
    Add-Line 'setup --check:'
    Add-Line (Get-NativeOutput $uv @('run', '--locked', '--project', $project, 'gothic-launcher', 'setup', '--check'))
}
$sessions = @(Get-ChildItem -LiteralPath $LogRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name)
Add-Line "logs: $LogRoot ($($sessions.Count) sessions)"
if ($sessions.Count -gt 0) {
    $latest = $sessions[-1]
    Add-Line "newest session: $($latest.FullName)"
    Get-ChildItem -LiteralPath $latest.FullName -File | ForEach-Object { Add-Line ('  {0}: {1:N0} bytes' -f $_.Name, $_.Length) }
}
$gameHome = Join-Path $env:USERPROFILE '.local\share\gothic-ai\game'
$saves = @(Get-ChildItem -LiteralPath $gameHome -Filter 'save_slot_*.sav' -ErrorAction SilentlyContinue)
Add-Line "game home: $gameHome ($($saves.Count) saves; log.txt exists: $(Test-Path -LiteralPath (Join-Path $gameHome 'log.txt')))"
$base = if ($env:OPENGOTHIC_BASE_CACHE) { $env:OPENGOTHIC_BASE_CACHE } else { Join-Path $env:LOCALAPPDATA 'gothic-ai\base' }
Add-Line "imported base cache: $base (exists: $(Test-Path -LiteralPath $base))"

$folder = Join-Path $env:LOCALAPPDATA 'GothicAI'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
$file = Join-Path $folder "diagnostics-$stamp.txt"
[System.IO.File]::WriteAllLines($file, $report, (New-Object System.Text.UTF8Encoding($false)))
Say 'diagnostics_done' $file
