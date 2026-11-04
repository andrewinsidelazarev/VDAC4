# MD.ELF build (ESP layer of the VDAC4 Mega Drive core) and the ELF loader compatibility check.
# Messages in Latin: Windows PowerShell 5.1 reads BOM-less files as ANSI.
param([string]$Vdac4='E:\zx\Tang Nano 20k VDAC\VDAC4')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
if (-not $env:IDF_PYTHON_ENV_PATH) { . 'E:\zx\esp\idf-env.ps1' }
$python=Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
& $python "$env:IDF_PATH\tools\idf.py" -B build -D IDF_TARGET=esp32s3 -D "VDAC4_ROOT=$Vdac4" reconfigure
if($LASTEXITCODE) { throw 'ESP configure failed' }
& cmake --build build --target elf
if($LASTEXITCODE) { throw 'ESP build failed' }
New-Item -ItemType Directory -Force dist | Out-Null
Copy-Item build/md_vdac4.app.elf dist/MD.ELF -Force
# Card for the VDAC4 Cores screen (COREMENU.ELF): card/card.bin (make_card.py from card.json and PNG,
# needs Pillow) as section .vdac4.card without ALLOC; the ELF loader skips it. No card yet - the screen
# shows the generic 'FPGA core' card
if (Test-Path card/card.bin) {
  & $python -W ignore "$Vdac4/ESP32 S3 Zero N8R8/Network/VDAC4 Cores/cores_ui/tools/add_card.py" dist/MD.ELF card/card.bin
  if($LASTEXITCODE) { throw 'card insert failed' }
}
& $python "$Vdac4\ESP32 S3 Zero N8R8\Sound\tools\check_module_symbols.py" dist/MD.ELF
if($LASTEXITCODE) { throw 'ELF loader compatibility check failed' }
& $python (Join-Path $root '..\tests\check_elf_pointers.py') dist/MD.ELF
if($LASTEXITCODE) { throw 'ELF pointer check failed' }
Get-Item dist/MD.ELF | Format-Table Name, Length
