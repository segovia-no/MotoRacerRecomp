@echo off
rem Moto Racer Recomp setup for Windows: double-click, or drop the GOG installer onto this file.
rem Runs setup.ps1 (see there for details).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
pause
