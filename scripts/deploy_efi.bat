@echo off
echo [Aria] Deploying EFI binary to ESP partition...

mountvol S: /S

if not exist "S:\EFI\aria" mkdir "S:\EFI\aria"

set EFI_BIN=C:\edk2\Build\Aria\RELEASE_VS2022\X64\AriaEfi.efi
copy /Y "%EFI_BIN%" "S:\EFI\aria\ariaefi.efi"

echo [+] Deployed. Add boot entry manually with bcdedit if first time.
pause
