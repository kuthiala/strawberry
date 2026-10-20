@echo off
rem install-windows.cmd - one-step build and install of Strawberry with iPod sync, from cmd or PowerShell.
rem Installs MSYS2 when it is missing (winget, or the official installer downloaded with curl), brings it up to date, then runs install-windows.sh in an MSYS2 UCRT64 shell.
rem Takes the same commands and options as install-windows.sh; with no command it does everything: packages, getopt-win, libgpod, build, bundle, install.
rem MSYS2 is updated from here because updating its core packages closes every MSYS2 process, including the one running the script.
setlocal EnableExtensions
cd /d "%~dp0"

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "MSYS2_BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "MSYS2_INSTALLER_URL=https://github.com/msys2/msys2-installer/releases/latest/download/msys2-x86_64-latest.exe"

if exist "%MSYS2_BASH%" goto have_msys2
where winget >nul 2>nul
if errorlevel 1 goto download_msys2
echo Installing MSYS2 with winget...
winget install --id MSYS2.MSYS2 --exact --location "%MSYS2_ROOT%" --accept-source-agreements --accept-package-agreements
if exist "%MSYS2_BASH%" goto have_msys2
echo winget did not install MSYS2 into %MSYS2_ROOT%, trying the official installer.

:download_msys2
echo Downloading the MSYS2 installer...
set "MSYS2_INSTALLER=%TEMP%\msys2-x86_64-latest.exe"
curl.exe -fL --retry 3 -o "%MSYS2_INSTALLER%" "%MSYS2_INSTALLER_URL%"
if errorlevel 1 (
  echo Could not download %MSYS2_INSTALLER_URL%
  exit /b 1
)
echo Installing MSYS2 into %MSYS2_ROOT%...
"%MSYS2_INSTALLER%" in --confirm-command --accept-messages --root "%MSYS2_ROOT%"
del "%MSYS2_INSTALLER%" >nul 2>nul
if not exist "%MSYS2_BASH%" (
  echo MSYS2 is not in %MSYS2_ROOT% after installing it. Install it from https://www.msys2.org/ and set MSYS2_ROOT to where it went.
  exit /b 1
)

:have_msys2
rem The first login shell of a new MSYS2 initialises its pacman keyring.
rem Updating twice: the first pass may only update the core packages.
echo Updating MSYS2...
"%MSYS2_BASH%" -lc "pacman -Syuu --noconfirm"
"%MSYS2_BASH%" -lc "pacman -Syuu --noconfirm"
if errorlevel 1 (
  echo Updating MSYS2 failed, see the pacman output above.
  exit /b 1
)

set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
"%MSYS2_BASH%" -lc "./install-windows.sh \"$@\"" install-windows.sh %*
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" echo install-windows.sh failed with exit code %RESULT%.
exit /b %RESULT%
