@echo off
setlocal

REM ===========================================================================
REM  Aria - deploy AriaEfi.efi to the EFI System Partition
REM
REM  Run from an elevated prompt (mountvol needs it).
REM ===========================================================================

if "%ARIA_EDK2%"=="" set "ARIA_EDK2=C:\edk2"
set "ARIA_EFI=%ARIA_EDK2%\Build\Aria\RELEASE_VS2022\X64\AriaPkg\efi\AriaEfi\OUTPUT\AriaEfi.efi"

if not exist "%ARIA_EFI%" (
  echo [!] "%ARIA_EFI%" not found.
  echo     Run scripts\build_efi.bat first.
  exit /b 1
)

echo [*] Mounting the EFI System Partition at S:
mountvol S: /S
if errorlevel 1 (
  echo [!] mountvol failed. Run this from an elevated prompt.
  exit /b 1
)

if not exist "S:\EFI\aria" mkdir "S:\EFI\aria"

copy /Y "%ARIA_EFI%" "S:\EFI\aria\ariaefi.efi" >nul
if errorlevel 1 (
  echo [!] Copy to the ESP failed.
  exit /b 1
)

echo [+] Deployed to S:\EFI\aria\ariaefi.efi
echo.
echo     To run it, either:
echo       1. Boot the UEFI Shell and run:
echo            fs0:\EFI\aria\ariaefi.efi
echo          (scripts\startup_aria.nsh does this automatically - copy it to
echo           the root of the same partition as startup.nsh)
echo       2. Add a firmware boot entry for \EFI\aria\ariaefi.efi.
echo.
echo     Read docs\STATUS.md first: the current build can hard-hang the machine.
pause
endlocal
