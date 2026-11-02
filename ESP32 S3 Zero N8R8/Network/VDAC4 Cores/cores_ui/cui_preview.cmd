@echo off
setlocal
set "TEMP=%~dp0build-pc"
set "TMP=%~dp0build-pc"
set "PYTHONDONTWRITEBYTECODE=1"
if not exist "%TEMP%" mkdir "%TEMP%"
if "%~1"=="--no-build" goto render
"E:\zx\esp\pio-venv\Scripts\python.exe" -u -B "%~dp0tests\run_stage3.py" --only fixtures
if errorlevel 1 exit /b %errorlevel%
:render
"E:\zx\esp\pio-venv\Scripts\python.exe" -B "%~dp0cui_preview.py" %*
exit /b %errorlevel%
