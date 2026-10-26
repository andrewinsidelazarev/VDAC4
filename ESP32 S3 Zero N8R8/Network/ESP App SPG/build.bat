@echo off
setlocal
chcp 65001 >nul
rem Запускатель приложения ESP: build.bat путь\к\ИМЯ.ELF -> build\ИМЯ.SPG (+ копия ИМЯ.ELF рядом).
rem По умолчанию — дема BOING (demo\boing\build\app\BOING.ELF рабочего дерева; demo\boing\build.bat).

set "HERE=%~dp0"
set "ELF=%~1"
if "%ELF%"=="" set "ELF=%HERE%..\..\..\..\demo\boing\build\app\BOING.ELF"
rem путь — полный: дальше build.bat меняет текущую папку
for %%I in ("%ELF%") do set "ELF=%%~fI"
if not exist "%ELF%" (
  echo Error: %ELF% not found ^(demo\boing\build.bat^)
  exit /b 1
)
for %%I in ("%HERE%..\shared\z80") do set "SHARED_Z80=%%~sI"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "E:\zx\z80\zuma\sjasmplus.exe" set "SJASM=E:\zx\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

cd /d "%HERE%"
py -3 prep.py "%ELF%"
if errorlevel 1 exit /b 1
rem ESP_APP_DIAG=1 — проверочная сборка: метки Z80 в FT_SEEN Tang (esp_app.asm, Trail)
set "DIAGOPT="
if defined ESP_APP_DIAG set "DIAGOPT=-DDIAG_TRAIL"
"%SJASM%" --nologo %DIAGOPT% --inc="%SHARED_Z80%" --sym="build/esp_app.sym" esp_app.asm
if errorlevel 1 exit /b 1

for %%I in ("%ELF%") do set "STEM=%%~nI" & set "ELFNAME=%%~nxI"
rem spgbld берёт пути блоков из ini от текущей папки (../build/...), утилиты — из папки ZiFi SPG
if not exist "%HERE%_spg" mkdir "%HERE%_spg"
set "PATH=%HERE%..\ZiFi SPG\_spg;%PATH%"
cd /d "%HERE%_spg"
"%HERE%..\ZiFi SPG\_spg\spgbld.exe" -b "%HERE%build\spgbld.ini" "%HERE%build\%STEM%.SPG"
if errorlevel 1 exit /b 1
copy /y "%ELF%" "%HERE%build\%ELFNAME%" >nul
echo Built: %HERE%build\%STEM%.SPG and %ELFNAME%
