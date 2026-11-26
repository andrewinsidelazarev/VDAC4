@echo off
setlocal
chcp 65001 >nul
rem Запускатель приложения ESP: build.bat путь\к\ИМЯ.ELF [путь\к\ЯДРО.BIN] [+путь\к\МОДУЛЬ.ELF ...]
rem   [ДАННЫЕ.EXT ...] -> build\ИМЯ.SPG (+ копии ИМЯ.ELF, ЯДРО.BIN и модулей рядом).
rem Ядро ПЛИС — необязательно, ESP 0.1.164+: второй аргумент с расширением .BIN, если такой
rem файл есть или в аргументе указана папка. Файлы данных — необязательно, ESP 0.1.165+: только
rem имена 8.3 (сами файлы лежат на карте Evo в папке приложения, в SPG их нет).
rem Дополнительные модули ELF (module_open_ex) — пути к ИМЯ.ELF, можно с «+» впереди: в SPG,
rem в ESP — после главного ELF; не взяла ESP — запуск без модуля.
rem По умолчанию — дема BOING (demo\boing\build\app\BOING.ELF рабочего дерева; demo\boing\build.bat).

set "HERE=%~dp0"
set "ELF=%~1"
if "%ELF%"=="" set "ELF=%HERE%..\..\..\..\demo\boing\build\app\BOING.ELF"
rem путь — полный: дальше build.bat меняет текущую папку
for %%I in ("%ELF%") do set "ELF=%%~fI"
set "CORE="
set "ARG2=%~2"
if /i "%~x2"==".BIN" (
  if exist "%~2" set "CORE=%~f2"
  if not "%ARG2%"=="%~nx2" set "CORE=%~f2"
)
if defined CORE if not exist "%CORE%" (
  echo Error: %CORE% not found
  exit /b 1
)
rem shift — отдельной строкой: внутри блока ( ) cmd теряет код выхода exit /b
if defined CORE shift /2
rem остальные аргументы — модули ELF (путь .ELF или «+путь») и имена файлов данных
set "DATA="
set "MODS="
:data_args
if "%~2"=="" goto data_done
set "ARG=%~2"
if "%ARG:~0,1%"=="+" goto mod_arg
if /i "%~x2"==".ELF" if exist "%~2" goto mod_arg
set "DATA=%DATA% %~2"
shift /2
goto data_args
:mod_arg
if "%ARG:~0,1%"=="+" set "ARG=%ARG:~1%"
rem путь — полный: дальше build.bat меняет текущую папку
for %%I in ("%ARG%") do set "MODS=%MODS% "%%~fI""
shift /2
goto data_args
:data_done
if not exist "%ELF%" (
  echo Error: %ELF% not found ^(demo\boing\build.bat^)
  exit /b 1
)
for %%I in ("%HERE%..\shared\z80") do set "SHARED_Z80=%%~sI"
for %%I in ("%HERE%..\shared\fat32\include") do set "SHARED_FAT32=%%~sI"

set "SJASM="
if defined SJASMPLUS if exist "%SJASMPLUS%" set "SJASM=%SJASMPLUS%"
if not defined SJASM if exist "E:\zx\z80\zuma\sjasmplus.exe" set "SJASM=E:\zx\z80\zuma\sjasmplus.exe"
if not defined SJASM if exist "C:\z80\zuma\sjasmplus.exe" set "SJASM=C:\z80\zuma\sjasmplus.exe"
if not defined SJASM (
  echo Error: sjasmplus.exe was not found.
  exit /b 1
)

cd /d "%HERE%"
if defined DATA (
  rem драйвер FAT32 для файлов данных: shared\fat32\build\fat32.bin, fat32-work.bin
  set "SJASMPLUS=%SJASM%"
  py -3 "%HERE%..\shared\fat32\build.py" >nul
  if errorlevel 1 (
    echo Error: FAT32 driver build failed ^(shared\fat32\build\fat32.log^)
    exit /b 1
  )
)
if "%CORE%"=="" (py -3 prep.py "%ELF%" %MODS% %DATA%) else (py -3 prep.py "%ELF%" "%CORE%" %MODS% %DATA%)
if errorlevel 1 exit /b 1
rem ESP_APP_DIAG=1 — проверочная сборка: метки Z80 в FT_SEEN Tang (esp_app.asm, Trail)
set "DIAGOPT="
if defined ESP_APP_DIAG set "DIAGOPT=-DDIAG_TRAIL"
"%SJASM%" --nologo %DIAGOPT% --inc="%SHARED_Z80%" --inc="%SHARED_FAT32%" --sym="build/esp_app.sym" esp_app.asm
if errorlevel 1 exit /b 1

for %%I in ("%ELF%") do set "STEM=%%~nI" & set "ELFNAME=%%~nxI"
rem spgbld берёт пути блоков из ini от текущей папки (../build/...), утилиты — из папки ZiFi SPG
if not exist "%HERE%_spg" mkdir "%HERE%_spg"
set "PATH=%HERE%..\ZiFi SPG\_spg;%PATH%"
cd /d "%HERE%_spg"
"%HERE%..\ZiFi SPG\_spg\spgbld.exe" -b "%HERE%build\spgbld.ini" "%HERE%build\%STEM%.SPG"
if errorlevel 1 exit /b 1
copy /y "%ELF%" "%HERE%build\%ELFNAME%" >nul
if not "%CORE%"=="" copy /y "%CORE%" "%HERE%build\" >nul
for %%Q in (%MODS%) do copy /y "%%~Q" "%HERE%build\" >nul
echo Built: %HERE%build\%STEM%.SPG and %ELFNAME%
if defined DATA echo Data files on the Evo card, next to %STEM%.SPG and %ELFNAME%:%DATA%
