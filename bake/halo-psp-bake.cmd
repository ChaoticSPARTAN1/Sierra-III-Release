@echo off
rem The bake's command on Windows (halo-psp-bake on Linux and macOS). The
rem bundle names it "Bake Halo.cmd" for the player. Double-clicked (no
rem arguments), it runs the guided bake (guided.py); with arguments, the bake
rem itself:
rem   "Bake Halo.cmd" <Halo disc image or maps folder> <output folder> [options]
rem
rem It finds a Python 3.9 to 3.14 (the bake's environment's own once made, a
rem Python it downloaded before, then the newest 3.14 ... 3.9 the py launcher
rem knows, then python or python3), and hands over to launcher.py, which sets
rem up the bake's own environment on the first run. With none, it offers to
rem download a portable Python from its official release
rem (python-build-standalone) with PowerShell, checked against the SHA-256
rem pinned below, into .python in this folder.
rem
rem HALO_PSP_BAKE_YES=1 answers its question yes (the tests).
rem HALO_PSP_BAKE_NO_SYSTEM_PYTHON=1 looks past the system's Python.
setlocal
set "HERE=%~dp0"
set "GUIDED="
if "%~1"=="" set "GUIDED=1"
set "CHECK=import sys; sys.exit(0 if sys.version_info[:2] in [(3, 9), (3, 10), (3, 11), (3, 12), (3, 13), (3, 14)] else 1)"

rem The portable Python: python-build-standalone's release, pinned. Its
rem SHA256SUMS lists this file; a new pin takes the new release's line.
set "PBS_VERSION=3.12.15"
set "PBS_PAGE=https://github.com/astral-sh/python-build-standalone/releases/tag/20261003"
set "PBS_FILE=cpython-3.12.15+20261003-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
set "PBS_URL=https://github.com/astral-sh/python-build-standalone/releases/download/20261003/cpython-3.12.15%%2B20261003-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
set "PBS_SHA=6fba7f2ae506facf41d457ea8293c7497910a675c69a4e954875169410a50402"

set "PY="
if exist "%HERE%.venv\Scripts\python.exe" call :try "%HERE%.venv\Scripts\python.exe"
if not defined PY if exist "%HERE%.python\python.exe" call :try "%HERE%.python\python.exe"
if "%HALO_PSP_BAKE_NO_SYSTEM_PYTHON%"=="1" goto found
for %%v in (3.14 3.13 3.12 3.11 3.10 3.9) do if not defined PY call :trypy %%v
if not defined PY call :try python
if not defined PY call :try python3
:found
if not defined PY goto nopython

:run
rem the guided bake waits for Enter itself
%PY% "%HERE%launcher.py" %*
exit /b %errorlevel%

:nopython
echo The bake needs Python 3.9 to 3.14, and this computer has none it can use.
echo.
echo It can download a portable Python %PBS_VERSION% for the bake alone (about 22 MB),
echo from its official release, python-build-standalone:
echo     %PBS_PAGE%
echo It goes into this folder (.python), is checked against the SHA-256 the bake carries,
echo and nothing else on this computer changes.
if "%HALO_PSP_BAKE_YES%"=="1" goto download
set "ANSWER="
set /p "ANSWER=Download it? [Y/n] "
if not defined ANSWER goto download
if /i "%ANSWER%"=="y" goto download
if /i "%ANSWER%"=="yes" goto download
echo Not downloaded.
goto hint

:download
set "DL=%HERE%.python-download"
if exist "%DL%" rmdir /s /q "%DL%"
mkdir "%DL%\unpacked"
echo Downloading %PBS_FILE% ...
rem .NET alone, no PowerShell modules: a PSModulePath left by PowerShell 7 (or
rem anything else) can keep Windows PowerShell from loading them
set "PSModulePath="
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; $file = $env:DL + '\python.tar.gz'; try { [Net.WebClient]::new().DownloadFile($env:PBS_URL, $file) } catch { [Console]::WriteLine('The download failed: ' + $_.Exception.Message); exit 1 }; $h = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([IO.File]::ReadAllBytes($file))).Replace('-', '').ToLower(); if ($h -ne $env:PBS_SHA) { [Console]::WriteLine('The download is not the file the bake expects (its SHA-256 is ' + $h + ', not ' + $env:PBS_SHA + '), so it was not used.'); exit 2 }"
if errorlevel 1 goto failed
rem Windows' own tar (Windows 10 1803 on): another tar on PATH may read C: as a host name
if not exist "%SystemRoot%\System32\tar.exe" (
  echo This Windows has no tar.exe to unpack it with: it needs Windows 10 version 1803 or later.
  goto failed
)
"%SystemRoot%\System32\tar.exe" -xzf "%DL%\python.tar.gz" -C "%DL%\unpacked"
if errorlevel 1 goto failed
if exist "%HERE%.python" rmdir /s /q "%HERE%.python"
move "%DL%\unpacked\python" "%HERE%.python" >nul
if errorlevel 1 goto failed
rmdir /s /q "%DL%"
call :try "%HERE%.python\python.exe"
if not defined PY (
  echo The downloaded Python does not start on this system.
  goto hint
)
echo Python %PBS_VERSION% is ready, in %HERE%.python
echo.
goto run

:failed
if exist "%DL%" rmdir /s /q "%DL%"
goto hint

:hint
echo.
echo Or install Python 3.12 from https://www.python.org/downloads/windows/ (tick "Add python.exe to PATH"),
echo then start this again.
if defined GUIDED if not "%HALO_PSP_BAKE_YES%"=="1" pause
exit /b 1

:try
"%~1" -c "%CHECK%" >nul 2>&1 && set PY="%~1"
exit /b 0

:trypy
py -%1 -c "%CHECK%" >nul 2>&1 && set PY=py -%1
exit /b 0
