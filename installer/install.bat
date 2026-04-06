@echo off
:: ============================================================================
:: install.bat -- OMEC Teleport ASIO Driver Installer
:: Must be run as Administrator.
::
:: Registers the ASIO COM DLL. The device stays on Windows' inbox USB audio
:: driver -- no custom INF or driver signing needed.
:: ============================================================================
setlocal

set SCRIPT_DIR=%~dp0
set DLL_PATH=%SCRIPT_DIR%..\x64\Release\OmecTeleportASIO.dll
if not exist "%DLL_PATH%" set DLL_PATH=%SCRIPT_DIR%..\x64\Debug\OmecTeleportASIO.dll

echo ==========================================================
echo  Orange OMEC Teleport ASIO Driver Installer  v1.0
echo ==========================================================
echo.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: This installer must be run as Administrator.
    echo Right-click and select "Run as administrator".
    pause & exit /b 1
)

if not exist "%DLL_PATH%" (
    echo ERROR: DLL not found. Build the project first.
    echo Looked for: %DLL_PATH%
    pause & exit /b 1
)
echo Using: %DLL_PATH%
echo.

echo Registering ASIO COM server...
regsvr32 /s "%DLL_PATH%"
if %errorlevel% neq 0 (
    echo ERROR: regsvr32 failed ^(code %errorlevel%^).
    pause & exit /b 1
)
echo Registration successful.
echo.
echo IMPORTANT: Before opening your DAW, ensure both the OMEC Teleport
echo capture and playback devices are set to the SAME sample rate in:
echo   Windows Settings ^> Sound ^> More sound settings ^> Advanced tab
echo.
echo Verify registration:
echo   reg query "HKLM\SOFTWARE\ASIO\OmecTeleport ASIO"
echo.
pause
endlocal
