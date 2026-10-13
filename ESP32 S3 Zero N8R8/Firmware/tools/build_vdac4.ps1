# Сборка всего, что нужно плате VDAC4 A3.1: прошивка ESP32-S3 (ESP-IDF 6.0 с
# Arduino-ядром как компонентом), плагины Wild Commander для обмена по SPI,
# проигрыватель MP3 и esp_info. Плату скрипт не прошивает и не трогает вовсе.
# Прошивка — в этой папке (Firmware), плагины и утилиты Z80 — в ..\Network,
# проигрыватель — в ..\Sound.
#
#   .\tools\build_vdac4.ps1                 # всё
#   .\tools\build_vdac4.ps1 -SkipFirmware   # только программы Z80
#   .\tools\build_vdac4.ps1 -SkipZ80        # только прошивка
#   .\tools\build_vdac4.ps1 -Clean          # прошивка с нуля (после правки sdkconfig.defaults)
#
# Окружение ESP-IDF берётся готовым, если скрипт запущен из «ESP-IDF PowerShell»
# (заданы IDF_PATH и IDF_PYTHON_ENV_PATH). Иначе выполняется скрипт окружения из
# переменной ZIFI_IDF_ENV либо E:\zx\esp\idf-env.ps1.
#
# Сообщения скрипта — латиницей: Windows PowerShell 5.1 читает файл без BOM в
# кодировке ANSI, и русский текст в строках превратился бы в мусор.

[CmdletBinding()]
param(
    [switch]$SkipFirmware,
    [switch]$SkipZ80,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$parts = Split-Path -Parent $root
$network = Join-Path $parts 'Network'
$sound = Join-Path $parts 'Sound'

function Use-IdfEnvironment {
    if ($env:IDF_PATH -and $env:IDF_PYTHON_ENV_PATH -and
        (Test-Path (Join-Path $env:IDF_PATH 'tools\idf.py'))) {
        return
    }
    $scripts = @($env:ZIFI_IDF_ENV, 'E:\zx\esp\idf-env.ps1') |
        Where-Object { $_ -and (Test-Path $_) }
    if (-not $scripts) {
        throw 'ESP-IDF was not found: run from an ESP-IDF PowerShell prompt or set ZIFI_IDF_ENV to an environment script'
    }
    . @($scripts)[0]
}

function Invoke-Checked {
    param([string]$What, [scriptblock]$Command)
    Write-Host "== $What" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$What failed with exit code $LASTEXITCODE"
    }
}

if (-not $SkipFirmware) {
    Use-IdfEnvironment
    # idf.py отказывается работать, если видит окружение MSYS/Git Bash.
    Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
    $python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
    $idf = Join-Path $env:IDF_PATH 'tools\idf.py'

    if ($Clean) {
        Invoke-Checked 'clean' { & $python $idf -C $root fullclean }
        Remove-Item (Join-Path $root 'sdkconfig') -ErrorAction SilentlyContinue
    }
    # reconfigure: список src/*.cpp CMake составляет при настройке, и новый файл
    # без неё в сборку не попадёт.
    Invoke-Checked 'ESP32-S3 firmware' { & $python $idf -C $root reconfigure build }
    Invoke-Checked 'firmware images' {
        & $python (Join-Path $root 'tools\package_vdac4.py')
    }
}

if (-not $SkipZ80) {
    foreach ($plugin in 'NTP Time Sync', 'FTP Server', 'SMB Server', 'WC Update', 'Online Update') {
        Invoke-Checked "plugin $plugin" { & (Join-Path $network "$plugin\build.bat") vdac4 }
    }
    Invoke-Checked 'plugin MP3 Player' { & (Join-Path $sound 'MP3 Player\build.bat') }
    Invoke-Checked 'saver Weather screensaver' {
        & (Join-Path $network 'Weather screensaver\build.bat') vdac4
    }
    # The VDAC2 saver draws its pictures with the Windows Segoe UI fonts; with
    # other font versions they may not fit into the plugin pages. That must not
    # stop the rest of the board build.
    try {
        Invoke-Checked 'saver Weather screensaver VDAC2' {
            & (Join-Path $network 'Weather screensaver VDAC2\build.bat') vdac4
        }
    } catch {
        Write-Warning "VDAC2 weather saver was not built: $_"
    }
    Invoke-Checked 'esp_info' { & (Join-Path $network 'tools\build_esp_info.bat') }
}

Write-Host ''
Write-Host 'Done. Files for the VDAC4 board:' -ForegroundColor Green
# Пути — от папки ESP32 S3 Zero N8R8 (Firmware, Network, Sound).
$outputs = @(
    'Firmware\firmware\vdac4\firmware.factory.bin',
    'Firmware\firmware\vdac4\firmware.bin',
    'Network\NTP Time Sync\build\vdac4\VDAC4NTP.WMF',
    'Network\FTP Server\build\vdac4\VDAC4FTP.WMF',
    'Network\SMB Server\build\vdac4\VDAC4SMB.WMF',
    'Network\WC Update\build\vdac4\VDAC4WCU.WMF',
    'Network\Online Update\build\vdac4\VDAC4UPD.WMF',
    'Sound\MP3 Player\build\VDAC4MP3.WMF',
    'Network\Weather screensaver\build\vdac4\VDAC4WTH.WMF',
    'Network\Weather screensaver VDAC2\build\vdac4\VDAC4WT2.WMF',
    'Network\tools\esp_info_vdac4.sna',
    'Network\tools\update_vdac4.sna',
    'Network\tools\dma_probe_vdac4.sna',
    'Network\tools\audio_test_vdac4.sna'
)
foreach ($name in $outputs) {
    $path = Join-Path $parts $name
    if (Test-Path $path) {
        $item = Get-Item $path
        '{0,9}  {1:yyyy-MM-dd HH:mm}  {2}' -f $item.Length, $item.LastWriteTime, $name
    } else {
        "  missing                    $name"
    }
}
