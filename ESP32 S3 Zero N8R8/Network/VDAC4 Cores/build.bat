@echo off
setlocal
rem VDAC4 Cores: build.bat -> build\vdac4\VDAC4COR.WMF
set "HERE=%~dp0"
chcp 65001 >nul
if not exist "%HERE%build\vdac4" mkdir "%HERE%build\vdac4"
set "SHARED_Z80=%HERE%..\shared\z80"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared sources were not found: %SHARED_Z80%
  exit /b 1
)
for %%I in ("%SHARED_Z80%") do set "SHARED_Z80=%%~sI"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "E:\zx\z80\zuma\sjasmplus.exe" set "SJASM=E:\zx\z80\zuma\sjasmplus.exe"
if not defined SJASM (
  where sjasmplus.exe >nul 2>nul
  if not errorlevel 1 set "SJASM=sjasmplus.exe"
)
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

cd /d "%HERE%src"
rem The plugin is built for the VDAC4 board only (SPI link, no DMA).
"%SJASM%" -DZIFI_SPI --inc="%SHARED_Z80%" --sym=..\build\vdac4\VDAC4COR.sym --lst=..\build\vdac4\VDAC4COR.lst main.asm
if errorlevel 1 exit /b 1

echo Built: %HERE%build\vdac4\VDAC4COR.WMF
