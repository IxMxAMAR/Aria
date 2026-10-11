@echo off
setlocal enabledelayedexpansion

REM ===========================================================================
REM  Aria - EDK2 build script
REM
REM  Requirements:
REM    - EDK2 checkout              set ARIA_EDK2   (default C:\edk2)
REM    - Python 3 + edk2-basetools
REM    - NASM                       set NASM_PREFIX (default C:\nasm\)
REM    - Visual Studio with the C++ toolset (cl.exe, ml64.exe)
REM
REM  Usage:
REM    scripts\build_efi.bat
REM
REM  The workarounds in here are explained in docs/STATUS.md ("Build").
REM ===========================================================================

pushd "%~dp0.."
set "ARIA_ROOT=%CD%"
popd

if "%ARIA_EDK2%"==""   set "ARIA_EDK2=C:\edk2"
if "%NASM_PREFIX%"=="" set "NASM_PREFIX=C:\nasm\"
set "BUILD_TARGET=RELEASE"

if not exist "%ARIA_EDK2%\edksetup.bat" (
  echo [ERROR] No EDK2 checkout at "%ARIA_EDK2%".
  echo     Set ARIA_EDK2 to your edk2 directory.
  exit /b 1
)

REM --- 1. Expose this repository to the EDK2 workspace as the AriaPkg package -
if exist "%ARIA_EDK2%\AriaPkg" (
  echo [*] %ARIA_EDK2%\AriaPkg already exists - leaving it alone
) else (
  mklink /J "%ARIA_EDK2%\AriaPkg" "%ARIA_ROOT%" >nul
  if errorlevel 1 (
    echo [ERROR] Could not create the junction "%ARIA_EDK2%\AriaPkg" -^> "%ARIA_ROOT%"
    echo     Create it by hand, then re-run.
    exit /b 1
  )
  echo [+] Linked %ARIA_EDK2%\AriaPkg -^> "%ARIA_ROOT%"
)

REM --- 2. Python build tools -------------------------------------------------
where python >nul 2>&1
if errorlevel 1 (
  echo [ERROR] python not found on PATH.
  exit /b 1
)
python -m pip show edk2-basetools >nul 2>&1
if errorlevel 1 (
  echo [*] Installing edk2-basetools
  python -m pip install --quiet edk2-basetools
  if errorlevel 1 (
    echo [ERROR] pip install edk2-basetools failed
    exit /b 1
  )
)

REM --- 3. NASM ---------------------------------------------------------------
REM  MdePkg/BaseLib has a CpuId.nasm, so NASM is mandatory even though all of
REM  Aria's own assembly is MASM.
if not exist "%NASM_PREFIX%nasm.exe" (
  where nasm >nul 2>&1
  if errorlevel 1 (
    echo [ERROR] NASM not found. MdePkg/BaseLib needs it for CpuId.nasm.
    echo     Download it, then set NASM_PREFIX to the folder holding nasm.exe:
    echo       https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip
    exit /b 1
  )
)

REM --- 4. GenFw --------------------------------------------------------------
REM  GenFw is a compiled BaseTools binary; the edk2-basetools pip package ships
REM  only the Python tools. Compiling it from source does not work on MSVC
REM  14.4x+ (EDK2's BaseTools sources trip newer warnings, and
REM  MdePkg/IndustryStandard/PeImage.h redefines RUNTIME_FUNCTION against the
REM  current Windows SDK). Use the prebuilt binaries tianocore publishes.
set "GENFW=%ARIA_EDK2%\BaseTools\Bin\Win32\GenFw.exe"
if not exist "%GENFW%" (
  echo [*] GenFw.exe missing - fetching prebuilt BaseTools binaries
  set "ARIA_BT_MIRROR=%ARIA_EDK2%\..\edk2-BaseTools-win32"
  if not exist "!ARIA_BT_MIRROR!\.git" (
    git clone --depth 1 https://github.com/tianocore/edk2-BaseTools-win32.git "!ARIA_BT_MIRROR!"
    if errorlevel 1 (
      echo [ERROR] Could not clone the prebuilt BaseTools mirror
      exit /b 1
    )
  )
  if not exist "%ARIA_EDK2%\BaseTools\Bin\Win32" mkdir "%ARIA_EDK2%\BaseTools\Bin\Win32"
  REM  Native C tools only. Do NOT copy build.exe or GenFds.exe: those are the
  REM  old Python 2 implementations and would shadow the pip-provided tools.
  for %%t in (GenFw GenSec GenFfs GenFv GenCrc32 GenBootSector GenPage GenVtf Split EfiRom EfiLdrImage LzmaCompress TianoCompress VolInfo TargetTool PatchPcdValue) do (
    if exist "!ARIA_BT_MIRROR!\%%t.exe" copy /Y "!ARIA_BT_MIRROR!\%%t.exe" "%ARIA_EDK2%\BaseTools\Bin\Win32\%%t.exe" >nul
  )
  if not exist "%GENFW%" (
    echo [ERROR] GenFw.exe still missing after copying from the mirror
    exit /b 1
  )
  echo [+] Installed prebuilt BaseTools into %ARIA_EDK2%\BaseTools\Bin\Win32
)

REM --- 5. Visual Studio ------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found. Install Visual Studio with the C++ workload.
  exit /b 1
)

REM  Deliberately unconstrained by version: EDK2's own detection stops below
REM  VS 2022, but newer toolsets build Aria fine.
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
  echo [ERROR] No Visual Studio installation with the C++ toolset was found.
  exit /b 1
)
echo [*] Visual Studio: !VS_PATH!

call "!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (
  echo [ERROR] vcvarsall.bat failed
  exit /b 1
)

REM  EDK2's VS2022 toolchain expects VS2022_PREFIX to be the MSVC toolset root.
REM  Take the newest one rather than hardcoding a version.
set "MSVC_VER="
for /f "delims=" %%i in ('dir /b /ad /o-n "!VS_PATH!\VC\Tools\MSVC" 2^>nul') do (
  if not defined MSVC_VER set "MSVC_VER=%%i"
)
if not defined MSVC_VER (
  echo [ERROR] No MSVC toolset under "!VS_PATH!\VC\Tools\MSVC"
  exit /b 1
)
set "VS2022_PREFIX=!VS_PATH!\VC\Tools\MSVC\!MSVC_VER!\"
echo [*] MSVC toolset: !MSVC_VER!

REM --- 6. Workspace ----------------------------------------------------------
set "WORKSPACE=%ARIA_EDK2%"
set "EDK_TOOLS_PATH=%ARIA_EDK2%\BaseTools"
cd /d "%ARIA_EDK2%"

call edksetup.bat Reconfig >nul < nul
if errorlevel 1 (
  echo [ERROR] edksetup.bat Reconfig failed
  exit /b 1
)

REM --- 7. Relax /WX in the BaseTools C makefile ------------------------------
REM  Only matters if the BaseTools are rebuilt from source; harmless otherwise.
if exist "%EDK_TOOLS_PATH%\Source\C\Makefiles\ms.common" (
  powershell -NoProfile -Command "$f='%EDK_TOOLS_PATH%\Source\C\Makefiles\ms.common'; $c=Get-Content $f -Raw; if ($c -match '/W4 /WX') { Set-Content $f ($c -replace '/W4 /WX','/W4') -NoNewline; Write-Host '[+] Relaxed /WX in ms.common' }"
)

REM --- 8. Build --------------------------------------------------------------
echo.
echo ===== Building Aria (%BUILD_TARGET%, X64, VS2022) =====
call build -a X64 -t VS2022 -b %BUILD_TARGET% -p AriaPkg/efi/AriaPkg.dsc < nul
if errorlevel 1 (
  echo.
  echo [ERROR] Build failed
  exit /b 1
)

set "ARIA_EFI=%ARIA_EDK2%\Build\Aria\%BUILD_TARGET%_VS2022\X64\AriaPkg\efi\AriaEfi\OUTPUT\AriaEfi.efi"
echo.
if not exist "%ARIA_EFI%" (
  echo [ERROR] Build reported success but "%ARIA_EFI%" is missing
  exit /b 1
)
echo [+] Build successful
echo [+] %ARIA_EFI%
endlocal
