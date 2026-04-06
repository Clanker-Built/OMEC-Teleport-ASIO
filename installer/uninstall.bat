@echo off
:: ============================================================================
:: uninstall.bat -- OMEC Teleport ASIO Driver Uninstaller
:: Must be run as Administrator.
:: ============================================================================
setlocal

set SCRIPT_DIR=%~dp0
set DLL_PATH=%SCRIPT_DIR%..\x64\Release\OmecTeleportASIO.dll
if not exist "%DLL_PATH%" set DLL_PATH=%SCRIPT_DIR%..\x64\Debug\OmecTeleportASIO.dll

echo ==========================================================
echo  Orange OMEC Teleport ASIO Driver Uninstaller
echo ==========================================================
echo.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Must be run as Administrator.
    pause & exit /b 1
)

if exist "%DLL_PATH%" (
    echo Unregistering: %DLL_PATH%
    regsvr32 /u /s "%DLL_PATH%"
    echo Done.
) else (
    echo DLL not found, cleaning registry manually...
    reg delete "HKLM\SOFTWARE\ASIO\OmecTeleport ASIO" /f >nul 2>&1
    echo Done.
)

echo.
echo Driver unregistered. You can now delete the driver files.
pause
endlocal
