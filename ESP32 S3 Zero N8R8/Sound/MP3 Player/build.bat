@echo off
setlocal
chcp 65001 >nul
rem MP3 player and DAC test for the VDAC4 board. Always built for the SPI link.
set "HERE=%~dp0"
if not exist "%HERE%build" mkdir "%HERE%build"
set "SHARED_Z80=%HERE%..\..\Network\shared\z80"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared Z80 sources were not found: %SHARED_Z80%
  exit /b 1
)
cd /d "%HERE%src"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "E:\zx\z80\zuma\sjasmplus.exe" set "SJASM=E:\zx\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "..\..\..\ZiFi\sjasmplus.exe" set "SJASM=..\..\..\ZiFi\sjasmplus.exe"
if not defined SJASM (
  where sjasmplus.exe >nul 2>nul
  if not errorlevel 1 set "SJASM=sjasmplus.exe"
)
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

"%SJASM%" -DZIFI_SPI -DZIFI_SPI_DMA --inc="%SHARED_Z80%" --sym=..\build\VDAC4MP3.sym --lst=..\build\VDAC4MP3.lst main.asm
if errorlevel 1 exit /b 1

echo Built: %HERE%build\VDAC4MP3.WMF
