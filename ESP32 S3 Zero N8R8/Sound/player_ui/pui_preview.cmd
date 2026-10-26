@echo off
setlocal
set "PYTHONDONTWRITEBYTECODE=1"
"E:\zx\esp\pio-venv\Scripts\python.exe" "%~dp0pui_preview.py" %*
exit /b %errorlevel%
