@echo off
setlocal
rem %~dp0 is taken before any cd: after "cd /d" cmd may resolve it
rem against the new current directory when the script was called by a relative path.
set "HERE=%~dp0"
chcp 65001 >nul
rem Build order: tools/gen_assets.py (fonts, icons, palette, strings), then
rem sjasmplus assembles build/vdac4/VDAC4WTH.WMF with INCBIN of the data pages.
rem Shared Z80 modules (proto, config, SPI link) come from ../shared/z80, the
rem weather saver core shared with the VDAC2 version from ../shared/weather.
if not exist "%HERE%build" mkdir "%HERE%build"
set "SHARED_Z80=%HERE%..\shared\z80"
set "SHARED_WEATHER=%HERE%..\shared\weather"
if not exist "%SHARED_Z80%\proto.asm" (
  echo Error: shared Z80 sources were not found: %SHARED_Z80%
  exit /b 1
)
if not exist "%SHARED_WEATHER%\saver.asm" (
  echo Error: shared weather sources were not found: %SHARED_WEATHER%
  exit /b 1
)

rem gen_assets.py needs Pillow. "python" may be the Microsoft Store stub on
rem Windows, so the py launcher is preferred; PYTHON overrides both.
set "PY=python"
where py >nul 2>nul
if not errorlevel 1 set "PY=py -3"
if defined PYTHON set "PY="%PYTHON%""
%PY% "%HERE%tools\gen_assets.py"
if errorlevel 1 exit /b 1

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

rem The saver is built for the VDAC4 board only (SPI link), under its own name:
rem the ZiFi adapter (UART) build was dropped. The "vdac4" argument is still accepted.
set "VARIANT="
set "OUT=..\build"
set "NAME=WEATHER"
(
  set "VARIANT=-DZIFI_SPI"
  set "OUT=..\build\vdac4"
  set "NAME=VDAC4WTH"
  if not exist "%HERE%build\vdac4" mkdir "%HERE%build\vdac4"
)

"%SJASM%" %VARIANT% --inc="%SHARED_Z80%" --inc="%SHARED_WEATHER%" --sym=%OUT%\%NAME%.sym --lst=%OUT%\%NAME%.lst main.asm
if errorlevel 1 exit /b 1

echo Built: %OUT%\%NAME%.WMF
