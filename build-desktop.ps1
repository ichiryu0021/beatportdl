# Build the WebView2 desktop shell.
#
#   .\build-desktop.ps1                        # build bin\bpdl-ui.exe
#   .\build-desktop.ps1 -Package               # clean runtime into dist\, keep user files
#   .\build-desktop.ps1 -Package -Version 1.1.0  # release build (version injected)
#
# Packaging replaces only program files. beatportdl-config.yml, credentials,
# data/ and logs/ in dist\ are preserved. If dist\ is currently running the
# script refuses to package instead of leaving a half-copied folder.
#
# The shell is pure Go (CGO_ENABLED=0). The Go downloader backend is a separate
# binary (bin\beatportdl.exe) built with the MSYS2/TagLib toolchain described in
# WORKLOG.md; this script only copies it into dist.

param(
    [switch]$Package,
    [string]$Version = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

if ($Version -and $Version -notmatch '^\d+(\.\d+){1,3}$') {
    throw "Version must look like 1.1.0 (got '$Version')"
}

$env:CGO_ENABLED = '0'
$env:GOFLAGS = '-mod=vendor'

if (-not (Test-Path 'bin')) { New-Item -ItemType Directory 'bin' | Out-Null }

$ldflags = '-H windowsgui -s -w'
$effectiveVersion = $Version
if (-not $effectiveVersion) {
    $assets = Get-Content 'internal\desktop\assets.go' -Raw
    if ($assets -match 'var Version = "([^"]+)"') { $effectiveVersion = $Matches[1] }
}
if ($Version) {
    $ldflags += " -X unspok3n/beatportdl/internal/desktop.Version=$Version"
}

Write-Host ('Building bin\bpdl-ui.exe' + $(if ($Version) { " (version $Version)" } else { '' }) + ' ...')
& go build -trimpath -ldflags $ldflags -o 'bin\bpdl-ui.exe' '.\cmd\bpdl-ui'
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

Copy-Item 'third_party\webview2\x64\WebView2Loader.dll' 'bin\WebView2Loader.dll' -Force
Write-Host ('Built: ' + (Resolve-Path 'bin\bpdl-ui.exe'))

if (-not $Package) { exit 0 }

$Dist = Join-Path $Root 'dist'
if (-not (Test-Path $Dist)) { New-Item -ItemType Directory $Dist | Out-Null }

# Never replace files of a running copy: Windows locks the exe/DLLs and a
# half-finished package is worse than refusing to build.
$distPrefix = [IO.Path]::GetFullPath($Dist).TrimEnd('\') + '\'
$runningFromDist = @(Get-Process -Name 'beatportdl_gui', 'beatportdl' -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ([IO.Path]::GetFullPath($_.Path)).StartsWith($distPrefix, [StringComparison]::OrdinalIgnoreCase)
    })
if ($runningFromDist.Count -gt 0) {
    $pids = ($runningFromDist | ForEach-Object { $_.Id }) -join ', '
    throw "dist is currently running (PID $pids). Close it before packaging. Config/credentials/data/logs will be preserved."
}

# Clean only the program runtime. User files (config, credentials, data/, logs/)
# are deliberately left untouched so a rebuild never destroys a working setup.
$RuntimeFiles = @(
    'beatportdl_gui.exe', 'beatportdl.exe', 'WebView2Loader.dll',
    'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
    'libtag-2.dll', 'libtag_c-2.dll', 'zlib1.dll',
    '启动.bat', 'version.txt'
)
foreach ($name in $RuntimeFiles) {
    $target = Join-Path $Dist $name
    if (Test-Path $target) { Remove-Item $target -Force -ErrorAction SilentlyContinue }
}
Get-ChildItem $Dist -Filter 'bpdl-*.zip' -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

Copy-Item 'bin\bpdl-ui.exe' (Join-Path $Dist 'beatportdl_gui.exe')
Copy-Item 'third_party\webview2\x64\WebView2Loader.dll' $Dist

$Backend = Join-Path $Root 'bin\beatportdl.exe'
if (-not (Test-Path $Backend)) {
    throw 'bin\beatportdl.exe is missing. Build the backend with the CGO/TagLib toolchain first.'
}
Copy-Item $Backend $Dist

foreach ($dll in @(
        'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
        'libtag-2.dll', 'libtag_c-2.dll', 'zlib1.dll')) {
    $src = Join-Path $Root ('bin\' + $dll)
    if (-not (Test-Path $src)) { throw ('missing runtime DLL: ' + $src) }
    Copy-Item $src $Dist
}

$launcher = "@echo off`r`nstart `"`" `"%~dp0beatportdl_gui.exe`""
Set-Content -Path (Join-Path $Dist '启动.bat') -Value $launcher -Encoding ASCII
Set-Content -Path (Join-Path $Dist 'version.txt') -Value $effectiveVersion -Encoding ASCII

# Seed local config/credentials only when dist has none. Existing files are
# never overwritten; the GitHub release builder excludes both explicitly.
foreach ($cfg in @('beatportdl-config.yml', 'beatportdl-credentials.json')) {
    $target = Join-Path $Dist $cfg
    if (Test-Path $target) { continue }
    $seed = Join-Path $Root ('test-deploy\' + $cfg)
    if (-not (Test-Path $seed)) { $seed = Join-Path $Root ('bin\' + $cfg) }
    if (Test-Path $seed) { Copy-Item $seed $target }
}

$size = [math]::Round(((Get-ChildItem $Dist -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host ('Packaged clean runtime: ' + $Dist + ' (' + $size + ' MB)')
Write-Host 'Preserved user files (if present): beatportdl-config.yml, beatportdl-credentials.json, data/, logs/.'
