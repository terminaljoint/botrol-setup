@echo off
title BOTROL Desktop Agent
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
  echo Python was not found. Install Python 3 from https://www.python.org/downloads/
  pause
  exit /b 1
)
if not exist "config.json" (
  copy "config.example.json" "config.json" >nul
  echo Created config.json.
  echo Open config.json, add your Render BOTROL_DESKTOP_TOKEN, save it, then run this file again.
  notepad config.json
  pause
  exit /b 0
)
py -3 agent.py
if errorlevel 1 pause
