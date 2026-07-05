@echo off
setlocal
REM Standalone uninstaller for My Media Vault (for when the app itself won't launch; the in-app
REM Settings > Uninstall does the same thing). Double-click it. It removes this whole portable
REM install folder plus the app's out-of-folder state (cache, a registry flag, crash dumps).

echo(
echo   ============================================================
echo    Uninstall My Media Vault
echo   ============================================================
echo(
echo   This permanently DELETES My Media Vault and ALL of its data:
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
set "RUNNER=%TEMP%\mmv-uninstall.cmd"

REM Generate a self-contained runner in %TEMP% with the paths baked in (expanded now). A script can't delete the
REM folder it's running from, so this second script runs from %TEMP%, waits for the app to exit, removes
REM everything, then deletes itself. Mirrors AppUpdater's cmd /c <self-contained script> pattern.
> "%RUNNER%" echo @echo off
>>"%RUNNER%" echo :wait
>>"%RUNNER%" echo tasklist /FI "IMAGENAME eq MyMediaVault.exe" 2^>NUL ^| find /I "MyMediaVault.exe" ^>NUL ^&^& ( taskkill /IM MyMediaVault.exe /F ^>NUL 2^>^&1 ^& timeout /t 1 /nobreak ^>NUL ^& goto wait )
>>"%RUNNER%" echo rmdir /S /Q "%HERE%" 2^>NUL
>>"%RUNNER%" echo rmdir /S /Q "%LOCALAPPDATA%\My Media Vault" 2^>NUL
>>"%RUNNER%" echo reg delete "HKCU\SOFTWARE\Xenia" /f ^>NUL 2^>^&1
>>"%RUNNER%" echo del /Q "%LOCALAPPDATA%\CrashDumps\MyMediaVault.exe.*.dmp" ^>NUL 2^>^&1
>>"%RUNNER%" echo ^(goto^) 2^>nul ^& del "%%~f0"

echo(
echo   Uninstalling...
start "" /min cmd /c "%RUNNER%"
exit /b
