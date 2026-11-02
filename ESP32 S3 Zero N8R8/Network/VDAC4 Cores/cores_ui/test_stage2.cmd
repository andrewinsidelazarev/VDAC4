@echo off
setlocal
call "%~dp0test_stage3.cmd" %*
exit /b %errorlevel%
