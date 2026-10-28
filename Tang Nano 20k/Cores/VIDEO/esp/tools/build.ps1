# Сборка VIDEO.ELF (приложение ESP ядра видео VDAC4) и проверка совместимости с загрузчиком ELF
# прошивки ESP. Messages in Latin: Windows PowerShell 5.1 reads BOM-less files as ANSI.
# Build directory is outside the source tree: CMake drops repeated words from the space-split
# path of build/toolchain/cflags ("Tang Nano 20k VDAC/VDAC4/Tang Nano 20k" loses the 2nd "Nano").
param([string]$Vdac4='E:\zx\Tang Nano 20k VDAC\VDAC4', [string]$Build='E:\zx\cache\tmp\video_esp_build')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
if (-not $env:IDF_PYTHON_ENV_PATH) { . 'E:\zx\esp\idf-env.ps1' }
$python=Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
& $python "$env:IDF_PATH\tools\idf.py" -B $Build -D IDF_TARGET=esp32s3 -D "VDAC4_ROOT=$Vdac4" reconfigure
if($LASTEXITCODE) { throw 'ESP configure failed' }
& cmake --build $Build --target elf
if($LASTEXITCODE) { throw 'ESP build failed' }
New-Item -ItemType Directory -Force dist | Out-Null
Copy-Item $Build/video_vdac4.app.elf dist/VIDEO.ELF -Force
& $python "$Vdac4\ESP32 S3 Zero N8R8\Sound\tools\check_module_symbols.py" dist/VIDEO.ELF
if($LASTEXITCODE) { throw 'ELF loader compatibility check failed' }
Get-Item dist/VIDEO.ELF | Format-Table Name, Length
