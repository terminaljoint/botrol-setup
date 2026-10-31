@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
  echo Python 3 was not found. Install Python 3.10 or newer from https://www.python.org/downloads/
  pause
  exit /b 1
)
if not exist ".venv\Scripts\python.exe" (
  echo Setting up BOTROL desktop agent...
  py -3 -m venv .venv
  if errorlevel 1 goto failed
  ".venv\Scripts\python.exe" -m pip install --upgrade pip
  if errorlevel 1 goto failed
  ".venv\Scripts\python.exe" -m pip install -r requirements.txt
  if errorlevel 1 goto failed
)
".venv\Scripts\python.exe" botrol_hotkey.py
if errorlevel 1 pause
exit /b 0
:failed
echo Setup failed. Check your internet connection and Python installation.
pause
exit /b 1
