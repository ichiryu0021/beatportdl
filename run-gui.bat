@echo off
REM Launch the BeatportDL desktop shell (WebView2 / Chromium kernel).
REM The shell starts bin\beatportdl.exe --serve by itself when needed.
setlocal
set "ROOT=%~dp0"
set "GUI=%ROOT%bin\bpdl-ui.exe"
if not exist "%GUI%" set "GUI=%ROOT%dist\beatportdl_gui.exe"
if not exist "%GUI%" (
    echo [ERROR] GUI not found. Build it with:
    echo     powershell -ExecutionPolicy Bypass -File .\build-desktop.ps1
    pause
    exit /b 1
)
start "" "%GUI%"
endlocal
