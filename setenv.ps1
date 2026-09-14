# setenv.ps1 — 为 beatportdl 的 CGO 构建/测试配置当前 PowerShell 会话的环境变量。
#
# 用法（在项目根目录下）：
#   . .\setenv.ps1            # dot-source 使变量在当前会话生效
#   go build ./cmd/beatportdl
#   go test ./test/...
#
# 本脚本只修改当前会话的 $env:* 变量，不写系统 PATH、不改 profile。
# 新开 PowerShell 会话需要重新 dot-source。

#requires -Version 5.1

# MSYS2 MinGW 与 usr 工具（pacman 等）路径。
$msysMingw = 'C:\msys64\mingw64\bin'
$msysUsr    = 'C:\msys64\usr\bin'

# Go 工具链默认安装路径（winget 装的 Go 通常在这里）。
$goRoot = 'C:\Program Files\Go\bin'

# 组装 PATH：优先 MSYS2 gcc，再 Go，最后保留既有 PATH。
$env:Path = "$msysMingw;$msysUsr;$goRoot;" + $env:Path

# CGO 构建 beatportdl 所需的编译器与链接参数。
# -DTAGLIB_STATIC 与 -Wno-deprecated 对齐原 Makefile 的 windows-amd64 目标。
$env:CGO_ENABLED = '1'
$env:CC          = 'gcc'
$env:CXX         = 'g++'
$env:CGO_CFLAGS   = '-IC:/msys64/mingw64/include -DTAGLIB_STATIC -Wno-deprecated'
$env:CGO_CXXFLAGS = '-IC:/msys64/mingw64/include -DTAGLIB_STATIC -Wno-deprecated'
$env:CGO_LDFLAGS  = '-LC:/msys64/mingw64/lib -lstdc++'

# -ltag -ltag_c -lz 由 internal/taglib/taglib.go 的 #cgo LDFLAGS 自动追加，
# 此处无需重复设置。

Write-Host 'CGO build environment configured for this session.' -ForegroundColor Green

# 自检：确认关键工具可解析，否则给出明确提示。
$checks = @(
    @{ Name = 'gcc';      Cmd = { gcc --version } },
    @{ Name = 'g++';      Cmd = { g++ --version } },
    @{ Name = 'go';       Cmd = { go version } },
    @{ Name = 'taglib.h'; Cmd = { Test-Path 'C:\msys64\mingw64\include\taglib\tag_c.h' } }
)
$ok = $true
foreach ($c in $checks) {
    try {
        & $c.Cmd | Select-Object -First 1 | ForEach-Object { Write-Host "  [$($c.Name)] $_" }
    } catch {
        Write-Host "  [$($c.Name)] NOT FOUND" -ForegroundColor Red
        $ok = $false
    }
}

if (-not $ok) {
    Write-Host ''
    Write-Host 'One or more prerequisites are missing. Expected:' -ForegroundColor Yellow
    Write-Host '  - MSYS2 at C:\msys64 (install: winget install MSYS2.MSYS2)'
    Write-Host '  - mingw-w64-x86_64-gcc and mingw-w64-x86_64-taglib via pacman'
    Write-Host '  - Go at C:\Program Files\Go (install: winget install GoLang.Go)'
    Write-Host 'After fixing, re-run: . .\setenv.ps1'
} else {
    Write-Host ''
    Write-Host 'Ready. Examples:' -ForegroundColor Cyan
    Write-Host '  go build -o bin\beatportdl.exe .\cmd\beatportdl'
    Write-Host '  .\bin\beatportdl.exe --serve'
    Write-Host '  go test ./test/...'
}
