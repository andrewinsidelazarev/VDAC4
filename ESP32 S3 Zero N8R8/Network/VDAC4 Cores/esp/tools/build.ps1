# Сборка COREMENU.ELF (экран плагина VDAC4 Cores на HDMI Tang) и проверка совместимости с загрузчиком
# ELF прошивки ESP. Messages in Latin: Windows PowerShell 5.1 reads BOM-less files as ANSI.
param([string]$Vdac4='E:\zx\Tang Nano 20k VDAC\VDAC4')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
if (-not $env:IDF_PYTHON_ENV_PATH) { . 'E:\zx\esp\idf-env.ps1' }
$python=Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
# Стадия сборки — на E:, рядом с прочими: исходники не засоряются
$build='E:\zx\cache\tmp\coremenu-build'
& $python "$env:IDF_PATH\tools\idf.py" -B $build -D IDF_TARGET=esp32s3 -D "VDAC4_ROOT=$Vdac4" reconfigure
if($LASTEXITCODE) { throw 'ESP configure failed' }
& cmake --build $build --target elf
if($LASTEXITCODE) { throw 'ESP build failed' }
New-Item -ItemType Directory -Force dist | Out-Null
Copy-Item "$build/coremenu.app.elf" dist/COREMENU.ELF -Force
& $python "$Vdac4\ESP32 S3 Zero N8R8\Sound\tools\check_module_symbols.py" dist/COREMENU.ELF
if($LASTEXITCODE) { throw 'ELF loader compatibility check failed' }
Get-Item dist/COREMENU.ELF | Format-Table Name, Length
