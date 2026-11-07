@echo off
rem Moto Racer Recomp PSP build for Windows: double-click, or drop the GOG installer onto this file.
rem Runs setup_psp.ps1, which builds in WSL (see there for details).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_psp.ps1" %*
pause
