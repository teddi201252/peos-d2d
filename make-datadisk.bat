@echo off
rem Makes the Diablo II data disk for d2d on PeOS (tools\make_datadisk.py).
rem Double-click it, or drop your Diablo II folder on it.
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 tools\make_datadisk.py %*
    goto done
)
where python >nul 2>nul
if %errorlevel%==0 (
    python tools\make_datadisk.py %*
    goto done
)
echo Python 3 is not installed. Get it from https://www.python.org/downloads/
echo and tick "Add python.exe to PATH" in the installer.
:done
echo.
pause
