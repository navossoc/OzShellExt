@echo off
setlocal

rem Usage: uninstall.cmd [all|thumbnail|preview]
rem
rem Removing one handler kind leaves the other registered; the DLL is only
rem deleted when nothing is left.

set WHAT=%~1
if "%WHAT%"=="" set WHAT=all

if /i not "%WHAT%"=="all" if /i not "%WHAT%"=="thumbnail" if /i not "%WHAT%"=="preview" (
    echo Unknown option "%WHAT%". Use: all, thumbnail or preview.
    exit /b 1
)

net session >nul 2>&1
if errorlevel 1 (
    echo Run this script as administrator.
    exit /b 1
)

set DEST=%ProgramFiles%\navossoc\OzShellExt

if not exist "%DEST%\OzShellExt.dll" (
    echo Nothing to remove at %DEST%.
    exit /b 0
)

regsvr32 /s /u /n /i:"%WHAT%" "%DEST%\OzShellExt.dll"

taskkill /f /im dllhost.exe >nul 2>&1
taskkill /f /im prevhost.exe >nul 2>&1

if /i not "%WHAT%"=="all" (
    echo Unregistered the %WHAT% handler. The DLL stays in place for the other one.
    echo Run "uninstall.cmd all" to remove everything.
    exit /b 0
)

del /q "%DEST%\OzShellExt.dll"
del /q "%DEST%\LICENSE" 2>nul
rmdir "%DEST%" 2>nul
rmdir "%ProgramFiles%\navossoc" 2>nul

echo Removed. Windows falls back to whatever else handles these extensions.
