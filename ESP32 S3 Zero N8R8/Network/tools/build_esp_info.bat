@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "E:\zx\z80\zuma\sjasmplus.exe" set "SJASM=E:\zx\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "..\..\ZiFi\sjasmplus.exe" set "SJASM=..\..\ZiFi\sjasmplus.exe"
if not defined SJASM (
  where sjasmplus.exe >nul 2>nul
  if not errorlevel 1 set "SJASM=sjasmplus.exe"
)
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

set "SHARED_Z80=%~dp0..\shared\z80"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared Z80 sources were not found: %SHARED_Z80%
  exit /b 1
)

rem esp_info_vdac4.sna - VDAC4 board (SPI); the ZiFi adapter (UART) esp_info.sna was dropped.
"%SJASM%" -DZIFI_SPI --inc="%SHARED_Z80%" --sym=esp_info_vdac4.sym esp_info.asm
if errorlevel 1 exit /b 1

echo Built: %~dp0esp_info_vdac4.sna

rem update_vdac4.sna - opens the firmware update port of the VDAC4 board (SPI).
"%SJASM%" -DZIFI_SPI --inc="%SHARED_Z80%" --sym=update_vdac4.sym update_vdac4.asm
if errorlevel 1 exit /b 1
echo Built: %~dp0update_vdac4.sna

rem dma_probe_vdac4.sna - checks the DMA exchange with the ESP of the VDAC4 board.
"%SJASM%" -DZIFI_SPI --inc="%SHARED_Z80%" --sym=dma_probe_vdac4.sym dma_probe_vdac4.asm
if errorlevel 1 exit /b 1
echo Built: %~dp0dma_probe_vdac4.sna

rem audio_test_vdac4.sna - DAC tones and line test of the VDAC4 board.
"%SJASM%" -DZIFI_SPI --inc="%SHARED_Z80%" --sym=audio_test_vdac4.sym audio_test_vdac4.asm
if errorlevel 1 exit /b 1
echo Built: %~dp0audio_test_vdac4.sna
