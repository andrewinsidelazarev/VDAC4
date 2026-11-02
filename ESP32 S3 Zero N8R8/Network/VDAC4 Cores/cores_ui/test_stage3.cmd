@echo off
setlocal
cd /d "%~dp0"
if not exist build-pc mkdir build-pc
set "TEMP=%CD%\build-pc"
set "TMP=%TEMP%"
set "PYTHONDONTWRITEBYTECODE=1"
"E:\zx\esp\pio-venv\Scripts\python.exe" -u -B tests\run_stage3.py %*
exit /b %errorlevel%
