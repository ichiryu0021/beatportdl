# Build the WebView2 desktop shell.
#
#   .\build-desktop.ps1                        # build bin\bpdl-ui.exe
#   .\build-desktop.ps1 -Package               # build + portable dist\ folder
#   .\build-desktop.ps1 -Package -Version 1.1.0  # release build (version injected)
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
if (Test-Path $Dist) { Remove-Item $Dist -Recurse -Force }
New-Item -ItemType Directory $Dist | Out-Null

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

$size = [math]::Round(((Get-ChildItem $Dist -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host ('Packaged clean portable folder: ' + $Dist + ' (' + $size + ' MB)')
Write-Host 'Clean package: no beatportdl-config.yml, credentials, data/, or logs/.'
