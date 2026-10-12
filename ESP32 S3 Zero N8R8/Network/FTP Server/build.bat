@echo off
setlocal
rem %~dp0 is taken before any cd: after "cd /d" cmd may resolve it
rem against the new current directory when the script was called by a relative path.
set "HERE=%~dp0"
chcp 65001
rem Put all generated files under build.
if not exist "%HERE%build" mkdir "%HERE%build"
set "SHARED_Z80=%HERE%..\shared\z80"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared sources were not found: %SHARED_Z80%
  exit /b 1
)
cd /d "%HERE%src"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "E:\zx\z80\zuma\sjasmplus.exe" set "SJASM=E:\zx\z80\zuma\sjasmplus.exe"
rem This path is resolved from FTP Server\src.
if not defined SJASM if exist "..\..\..\ZiFi\sjasmplus.exe" set "SJASM=..\..\..\ZiFi\sjasmplus.exe"
if not defined SJASM (
  where sjasmplus.exe
  if not errorlevel 1 set "SJASM=sjasmplus.exe"
)
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

rem The plugin is built for the VDAC4 board only (SPI link): the ZiFi adapter
rem (UART) build was dropped. The "vdac4" argument is still accepted.
set "VARIANT="
set "OUT=..\build"
set "NAME=ZIFIFTP"
(
  rem ZIFI_SPI_DMA: blocks from the ESP come through the TS-Config DMA.
  set "VARIANT=-DZIFI_SPI -DZIFI_SPI_DMA"
  set "OUT=..\build\vdac4"
  rem The board build keeps its own file name (VDAC4*.WMF).
  set "NAME=VDAC4FTP"
  if not exist "%HERE%build\vdac4" mkdir "%HERE%build\vdac4"
)

"%SJASM%" %VARIANT% --inc="%SHARED_Z80%" --sym=%OUT%\%NAME%.sym --lst=%OUT%\%NAME%.lst main.asm
if errorlevel 1 exit /b 1

echo Built: %OUT%\%NAME%.WMF
echo Firmware and update.sna: ..\firmware\
