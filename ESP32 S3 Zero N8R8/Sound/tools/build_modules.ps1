# Сборка модулей ESP (приложения и декодеры для /WC/ESP/ на карте Evo).
#
#   .\tools\build_modules.ps1            # все модули из modules\
#   .\tools\build_modules.ps1 mp3 player # выбранные
#
# Каждый модуль — отдельный проект ESP-IDF с project_elf (Firmware/components/elf_loader);
# готовые файлы ложатся в modules\out\<ИМЯ>.ELF. После сборки
# tools\check_module_symbols.py сверяет внешние символы модуля с тем, что даёт
# ядро (модуль с чужим символом на плате не свяжется), ищет секции, которых
# загрузчик не кладёт, и указатели вне загружаемых секций (их загрузчик делает 0).
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Modules
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$all = @{ 'mp3' = 'MP3.ELF'; 'aac' = 'AAC.ELF'; 'ogg' = 'OGG.ELF'; 'flac' = 'FLAC.ELF'; 'player' = 'PLAYER.ELF'; 'tracker' = 'TRACKER.ELF'; 'midi' = 'MIDI.ELF'; 'opl' = 'OPL.ELF'; 'saa' = 'SAA.ELF'; 'tsfm' = 'TSFM.ELF' }
if (-not $Modules) { $Modules = @($all.Keys | Where-Object { Test-Path (Join-Path $root "modules\$_") }) }

if (-not ($env:IDF_PATH -and $env:IDF_PYTHON_ENV_PATH)) {
    $scripts = @($env:ZIFI_IDF_ENV, 'E:\zx\esp\idf-env.ps1') | Where-Object { $_ -and (Test-Path $_) }
    if (-not $scripts) { throw 'ESP-IDF was not found: set ZIFI_IDF_ENV to an environment script' }
    . @($scripts)[0]
}
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idf = Join-Path $env:IDF_PATH 'tools\idf.py'
$out = Join-Path $root 'modules\out'
New-Item -ItemType Directory -Force $out | Out-Null

foreach ($module in $Modules) {
    $dir = Join-Path $root "modules\$module"
    $file = $all[$module]
    if (-not $file) { throw "unknown module $module" }
    Write-Host "== module $module" -ForegroundColor Cyan
    if (-not (Test-Path (Join-Path $dir 'sdkconfig'))) {
        & $python $idf -C $dir set-target esp32s3
        if ($LASTEXITCODE -ne 0) { throw "set-target $module failed" }
    }
    & $python $idf -C $dir build
    if ($LASTEXITCODE -ne 0) { throw "build $module failed" }
    $project = if ($module -eq 'tracker') { 'tracker_module' } else { $module }
    $elf = Join-Path $dir "build\$project.app.elf"
    Copy-Item $elf (Join-Path $out $file) -Force
    & $python (Join-Path $root 'tools\check_module_symbols.py') (Join-Path $out $file)
    if ($LASTEXITCODE -ne 0) { throw "module $module failed the loader check (symbols, sections, pointers)" }
    Write-Host ("{0}: {1} bytes" -f $file, (Get-Item (Join-Path $out $file)).Length)
}
