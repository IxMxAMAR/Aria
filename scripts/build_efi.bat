@echo off
setlocal

set EDK2_PATH=C:\edk2
set ARIA_PATH=%~dp0..

if not exist "%EDK2_PATH%\AriaPkg" (
    mklink /J "%EDK2_PATH%\AriaPkg" "%ARIA_PATH%"
)

cd /d "%EDK2_PATH%"
call edksetup.bat
build -a X64 -t VS2022 -b RELEASE -p AriaPkg/efi/AriaPkg.dsc

if %ERRORLEVEL% NEQ 0 (
    echo [!] Build failed
    exit /b 1
)

echo [+] Build successful
echo [+] Output: %EDK2_PATH%\Build\Aria\RELEASE_VS2022\X64\AriaEfi.efi
pause
