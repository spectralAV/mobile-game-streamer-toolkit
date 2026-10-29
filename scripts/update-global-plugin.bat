@echo off
setlocal
cd /d "%~dp0"

echo =======================================================
echo Installing obs-scrcpy-source to OBS Studio (Program Files)
echo =======================================================

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo Requesting Administrator privileges...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

taskkill /F /IM obs64.exe >nul 2>&1
taskkill /F /IM obs-airplay-tether-helper.exe >nul 2>&1
schtasks /end /tn "OBS-AirPlay-Tether-Helper" >nul 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-global-plugin.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-user-plugin.ps1"
if %errorLevel% equ 0 (
    echo.
    echo =======================================================
    echo [SUCCESS] Plugin successfully installed to Program Files!
    echo =======================================================
) else (
    echo.
    echo [ERROR] Installation failed. Make sure OBS Studio is closed.
)
echo.
pause
