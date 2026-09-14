@echo off
REM Launch BeatportDL: backend (--serve) + Qt GUI.
REM Requires MSYS2 + Qt6 + CMake + ninja (see install-build-tools.bat).
REM NOTE: keep this file ASCII-only. cmd.exe parses .bat in the OEM
REM codepage; UTF-8 Chinese comments get misaligned and eat bytes from
REM following commands, breaking set/start lines.
setlocal
set "PATH=C:\msys64\mingw64\bin;C:\msys64\usr\bin;C:\Program Files\CMake\bin;%PATH%"

set "BPDL_EXE=%~dp0bin\beatportdl.exe"
set "GUI_EXE=%~dp0gui\build\beatportdl_gui.exe"
set "ADDR=127.0.0.1:8765"

if not exist "%BPDL_EXE%" (
    echo [ERROR] backend not found: %BPDL_EXE%
    pause
    exit /b 1
)
if not exist "%GUI_EXE%" (
    echo [ERROR] GUI not found: %GUI_EXE%
    echo Build it first: cmake -S gui -B gui/build -G Ninja -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 ^&^& cmake --build gui/build
    pause
    exit /b 1
)

echo Checking backend at %ADDR% ...
curl -s --max-time 2 http://%ADDR%/health >nul 2>&1
if errorlevel 1 (
    echo Starting backend ...
    start "beatportdl-backend" /D "%~dp0bin" "%BPDL_EXE%" --serve --addr=%ADDR%
    set /a tries=0
) else (
    echo Backend already running.
    goto :launchgui
)

:waitloop
timeout /t 1 /nobreak >nul
curl -s --max-time 2 http://%ADDR%/health >nul 2>&1
if not errorlevel 1 goto :launchgui
set /a tries+=1
if %tries% lss 30 goto :waitloop
echo [ERROR] backend did not become ready within 30 seconds.
pause
exit /b 1

:launchgui
echo Launching GUI ...
start "" "%GUI_EXE%"
endlocal
