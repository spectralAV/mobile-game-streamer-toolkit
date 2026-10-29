@echo off
setlocal
echo =======================================================
echo Removing stale Program Files obs-scrcpy-source.dll
echo =======================================================

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo Requesting Administrator privileges...
    powershell -Command "Start-Process cmd.exe -ArgumentList '/c `\"\"%~f0`\"\"' -Verb RunAs"
    exit /b
)

del /f /q "C:\Program Files\obs-studio\obs-plugins\64bit\obs-scrcpy-source.dll" 2>nul
if not exist "C:\Program Files\obs-studio\obs-plugins\64bit\obs-scrcpy-source.dll" (
    echo.
    echo [SUCCESS] Stale global DLL removed successfully!
    echo OBS Studio will now load the updated plugin from:
    echo %%APPDATA%%\obs-studio\plugins\obs-scrcpy-source\bin\64bit\obs-scrcpy-source.dll
    echo.
) else (
    echo.
    echo [ERROR] Could not remove global DLL. Please make sure OBS is closed.
    echo.
)
pause
