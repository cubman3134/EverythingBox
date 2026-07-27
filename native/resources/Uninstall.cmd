@echo off
setlocal
REM Standalone uninstaller for EverythingBox (for when the app itself won't launch; the in-app
REM Settings > Uninstall does the same thing). Double-click it. It removes this whole portable
REM install folder plus the app's out-of-folder state (cache, a registry flag, crash dumps).
REM
REM THE PREVIOUS BRAND IS DELIBERATE HERE — this file is exempt from the suite's old-brand gate
REM (see native/tools/run-headless-probes.sh). Two reasons, and both are load-bearing:
REM   1. "%LOCALAPPDATA%\My Media Vault" is the CURRENT cache/AppData dir, not a stale one:
REM      QApplication::setApplicationName is still the legacy spaced form on purpose (see the
REM      WARNING at native/src/main.cpp:214 — changing it moves the mobile data directory), and
REM      QStandardPaths derives that path from it. Renaming this line to EverythingBox would make
REM      the uninstaller silently leave the real cache behind.
REM   2. This uninstaller exists precisely FOR the case where the app never launched — which is
REM      also the case where the brand migration never ran. Anything it only cleans up under the
REM      new name would be missed on exactly the installs that need it most.
REM The new-name paths are listed alongside so this keeps working once the app-name migration lands.

echo(
echo   ============================================================
echo    Uninstall EverythingBox
echo   ============================================================
echo(
echo   This permanently DELETES EverythingBox and ALL of its data:
echo     "%~dp0"
echo(
echo   That includes your settings, cloud sign-in, downloaded games/music,
echo   emulator saves and save states, installed emulators/cores, the cache,
echo   and crash logs. This cannot be undone.
echo(
echo   Copy anything you want to keep out of that folder first.
echo(
set /p "ANS=  Type  Y  then Enter to uninstall (anything else cancels): "
if /I not "%ANS%"=="Y" ( echo( & echo   Cancelled. & timeout /t 2 ^>NUL & exit /b )

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "RUNNER=%TEMP%\eb-uninstall.cmd"

REM Generate a self-contained runner in %TEMP% with the paths baked in (expanded now). A script can't delete the
REM folder it's running from, so this second script runs from %TEMP%, waits for the app to exit, removes
REM everything, then deletes itself. Mirrors AppUpdater's cmd /c <self-contained script> pattern.
> "%RUNNER%" echo @echo off
>>"%RUNNER%" echo :wait
>>"%RUNNER%" echo tasklist /FI "IMAGENAME eq EverythingBox.exe" 2^>NUL ^| find /I "EverythingBox.exe" ^>NUL ^&^& ( taskkill /IM EverythingBox.exe /F ^>NUL 2^>^&1 ^& timeout /t 1 /nobreak ^>NUL ^& goto wait )
>>"%RUNNER%" echo tasklist /FI "IMAGENAME eq MyMediaVault.exe" 2^>NUL ^| find /I "MyMediaVault.exe" ^>NUL ^&^& ( taskkill /IM MyMediaVault.exe /F ^>NUL 2^>^&1 ^& timeout /t 1 /nobreak ^>NUL ^& goto wait )
>>"%RUNNER%" echo rmdir /S /Q "%HERE%" 2^>NUL
>>"%RUNNER%" echo rmdir /S /Q "%LOCALAPPDATA%\My Media Vault" 2^>NUL
>>"%RUNNER%" echo rmdir /S /Q "%LOCALAPPDATA%\EverythingBox" 2^>NUL
>>"%RUNNER%" echo reg delete "HKCU\SOFTWARE\Xenia" /f ^>NUL 2^>^&1
>>"%RUNNER%" echo del /Q "%LOCALAPPDATA%\CrashDumps\EverythingBox.exe.*.dmp" ^>NUL 2^>^&1
>>"%RUNNER%" echo del /Q "%LOCALAPPDATA%\CrashDumps\MyMediaVault.exe.*.dmp" ^>NUL 2^>^&1
>>"%RUNNER%" echo ^(goto^) 2^>nul ^& del "%%~f0"

echo(
echo   Uninstalling...
start "" /min cmd /c "%RUNNER%"
exit /b
