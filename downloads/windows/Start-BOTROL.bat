@echo off
title BOTROL Voice Agent
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0BOTROL-Voice-Agent.ps1"
if errorlevel 1 (
  echo.
  echo BOTROL stopped with an error.
  pause
)
