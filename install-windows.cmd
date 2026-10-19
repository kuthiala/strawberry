@echo off
rem install-windows.cmd - runs install-windows.sh in an MSYS2 UCRT64 shell, from cmd or PowerShell.
rem Installs MSYS2 with winget when it is missing. Takes the same commands and options as install-windows.sh.
rem --refresh-deps also updates MSYS2 itself first; that runs from here because updating MSYS2's core closes every MSYS2 process.
setlocal EnableExtensions
cd /d "%~dp0"

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "MSYS2_BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "UPDATE_MSYS2=0"

if not exist "%MSYS2_BASH%" (
  where winget >nul 2>nul
  if errorlevel 1 (
    echo MSYS2 was not found in %MSYS2_ROOT% and winget is not available.
    echo Install MSYS2 from https://www.msys2.org/ ^(or set MSYS2_ROOT^) and run this again.
    exit /b 1
  )
  echo Installing MSYS2 with winget...
  winget install --id MSYS2.MSYS2 --exact --accept-source-agreements --accept-package-agreements
  if errorlevel 1 exit /b 1
  set "UPDATE_MSYS2=1"
)
if not exist "%MSYS2_BASH%" (
  echo MSYS2 is not in %MSYS2_ROOT% after installing it, set MSYS2_ROOT to where it went.
  exit /b 1
)

for %%A in (%*) do if /i "%%~A"=="--refresh-deps" set "UPDATE_MSYS2=1"

if "%UPDATE_MSYS2%"=="1" (
  echo Updating MSYS2, twice: the first pass may only update the core packages.
  "%MSYS2_BASH%" -lc "pacman -Syuu --noconfirm"
  "%MSYS2_BASH%" -lc "pacman -Syuu --noconfirm"
)

set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
"%MSYS2_BASH%" -lc "./install-windows.sh \"$@\"" install-windows.sh %*
exit /b %ERRORLEVEL%
