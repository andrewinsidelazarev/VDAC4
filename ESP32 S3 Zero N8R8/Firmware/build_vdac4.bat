@echo off
rem Builds everything for the VDAC4 A3.1 board: ESP32-S3 firmware (ESP-IDF),
rem Wild Commander plugins for the SPI link, the MP3 player and esp_info.
rem Nothing is flashed. Options: -SkipFirmware, -SkipZ80, -Clean.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_vdac4.ps1" %*
exit /b %errorlevel%
