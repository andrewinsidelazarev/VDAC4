@echo off
chcp 65001 >nul
"E:\zx\esp\pio-venv\Scripts\python.exe" -X utf8 "%~dp0run.py" serve
if errorlevel 1 pause
