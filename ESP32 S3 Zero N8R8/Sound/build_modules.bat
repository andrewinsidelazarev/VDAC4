@echo off
rem Builds the ESP modules (apps and decoders for /WC/ESP/ on the Evo card) into modules\out.
rem Usage: build_modules.bat [mp3 player tracker ...]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_modules.ps1" %*
exit /b %errorlevel%
