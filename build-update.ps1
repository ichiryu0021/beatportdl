# Build the two GitHub Release assets consumed by the auto-updater.
#
#   .\build-update.ps1 -Version 1.1.0
#
# Prerequisite: .\build-desktop.ps1 -Package has produced dist\ with the
# current beatportdl_gui.exe + beatportdl.exe + runtime DLLs.
#
# Outputs (in dist\, next to the portable folder):
#   bpdl-hotfix-v<version>.zip   minimal: app exe + WebView2Loader + manifest
#   bpdl-v<version>-full.zip     full runtime package (no config, no data/logs)
#
# Publish both files as assets of a GitHub Release tagged v<version> on the
# repository in internal/desktop/update.go (ichiryu0021/beatportdl by default).
# Do not publish credentials, data/ or logs.

param(
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

if ($Version -notmatch '^\d+(\.\d+){1,3}$') {
    throw "Version must look like 1.1.0 (got '$Version')"
}
$Dist = Join-Path $Root 'dist'
$Exe = Join-Path $Dist 'beatportdl_gui.exe'
$Backend = Join-Path $Dist 'beatportdl.exe'
$Loader = Join-Path $Dist 'WebView2Loader.dll'
foreach ($required in @($Exe, $Backend, $Loader)) {
    if (-not (Test-Path $required)) {
        throw "Missing $required; run .\build-desktop.ps1 -Package -Version $Version first."
    }
}
$VersionFile = Join-Path $Dist 'version.txt'
if (-not (Test-Path $VersionFile)) {
    throw "dist\version.txt is missing. Rebuild with: .\build-desktop.ps1 -Package -Version $Version"
}
$builtVersion = (Get-Content $VersionFile -Raw).Trim()
if ($builtVersion -ne $Version) {
    throw "dist was built as $builtVersion but this update is $Version. Rebuild with: .\build-desktop.ps1 -Package -Version $Version"
}

$RuntimeDlls = @(Get-ChildItem $Dist -Filter '*.dll' -File | Select-Object -ExpandProperty Name)
$StageRoot = Join-Path $env:TEMP ('bpdl-update-build-' + [guid]::NewGuid().ToString('N'))
$HotfixStage = Join-Path $StageRoot 'hotfix'
$FullStage = Join-Path $StageRoot 'full'

function Copy-RuntimeFiles([string]$Destination, [string[]]$Files) {
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($name in $Files) {
        $src = Join-Path $Dist $name
        if (Test-Path $src) { Copy-Item $src (Join-Path $Destination $name) -Force }
    }
}

function Write-Manifest([string]$Destination, [string[]]$Files) {
    $manifest = [ordered]@{
        app     = 'beatportdl'
        version = $Version
        builtAt = (Get-Date).ToUniversalTime().ToString('o')
        files   = $Files
    }
    $json = $manifest | ConvertTo-Json -Depth 4
    [IO.File]::WriteAllText((Join-Path $Destination 'update.json'), $json, (New-Object Text.UTF8Encoding($false)))
}

function New-Zip([string]$Source, [string]$ZipPath) {
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $Source, $ZipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
}

try {
    $HotfixFiles = @('beatportdl_gui.exe', 'WebView2Loader.dll')
    Copy-RuntimeFiles $HotfixStage $HotfixFiles
    Write-Manifest $HotfixStage $HotfixFiles

    $FullFiles = @('beatportdl_gui.exe', 'beatportdl.exe', 'WebView2Loader.dll') + $RuntimeDlls
    if (Test-Path (Join-Path $Dist '启动.bat')) { $FullFiles += '启动.bat' }
    $FullFiles = @($FullFiles | Select-Object -Unique)
    Copy-RuntimeFiles $FullStage $FullFiles
    Write-Manifest $FullStage $FullFiles

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $HotfixZip = Join-Path $Dist ("bpdl-hotfix-v$Version.zip")
    $FullZip = Join-Path $Dist ("bpdl-v$Version-full.zip")
    New-Zip $HotfixStage $HotfixZip
    New-Zip $FullStage $FullZip

    Write-Host ('Hotfix: {0} ({1:N1} MB)' -f $HotfixZip, ((Get-Item $HotfixZip).Length / 1MB))
    Write-Host ('Full:   {0} ({1:N1} MB)' -f $FullZip, ((Get-Item $FullZip).Length / 1MB))
    Write-Host ''
    Write-Host "Prerequisite check: dist must have been built with -Version $Version (same exe version)."
    Write-Host 'Next steps (only after explicit publish approval):'
    Write-Host "  1. Commit and push, then create GitHub Release tag v$Version."
    Write-Host '  2. Upload both zips above and set Chinese asset labels.'
    Write-Host '  3. The installed client checks /releases/latest on startup.'
}
finally {
    Remove-Item $StageRoot -Recurse -Force -ErrorAction SilentlyContinue
}
