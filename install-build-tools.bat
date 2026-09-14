@echo off
REM Install ninja/make (MSYS2) needed to build the GUI. Keep ASCII-only.
set PATH=C:\msys64\mingw64\bin;C:\msys64\usr\bin;%PATH%
echo Installing ninja + make...
pacman -S --noconfirm mingw-w64-x86_64-ninja mingw-w64-x86_64-make
echo.
if exist C:\msys64\mingw64\bin\ninja.exe (
    echo OK: ninja installed
) else (
    echo FAIL: ninja not found - check errors above
)
if exist C:\msys64\mingw64\bin\mingw32-make.exe (
    echo OK: mingw32-make installed
) else (
    echo WARN: mingw32-make not found (ninja alone is sufficient)
)
pause
