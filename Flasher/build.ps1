# Сборка «Первой прошивки VDAC4» (VDAC4Flasher.exe) и проверок на ПК (FlasherTests.exe).
# Нужен только компилятор C# Roslyn (Visual Studio 2022 Build Tools); программа — .NET Framework 4.8,
# он есть в каждой Windows 10/11, ставить ничего не нужно.
# Messages in Latin: Windows PowerShell 5.1 reads BOM-less files as ANSI.
param(
    [string]$Out = 'E:\zx\cache\tmp\flasher\build',
    [switch]$Release              # собрать ещё и папку для пользователей (Out\VDAC4 Flasher)
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$csc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found: $csc" }
$fw = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319'
$refs = @('System.dll', 'System.Core.dll', 'System.Windows.Forms.dll', 'System.Drawing.dll', 'System.Management.dll') |
    ForEach-Object { "/reference:$fw\$_" }
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TMP = $Out; $env:TEMP = $Out

$lib = Get-ChildItem "$here\src\*.cs" | Where-Object { $_.Name -notin @('Program.cs', 'Wizard.cs') } | ForEach-Object { $_.FullName }
$ui = @("$here\src\Program.cs", "$here\src\Wizard.cs")
$tests = Get-ChildItem "$here\tests\*.cs" | ForEach-Object { $_.FullName }
$common = @('/nologo', '/noconfig', '/nostdlib+', "/reference:$fw\mscorlib.dll", '/langversion:7.3', '/optimize+',
            '/platform:anycpu', '/warn:4', '/utf8output', '/codepage:65001')

& $csc @common @refs /target:winexe "/win32manifest:$here\app.manifest" "/out:$Out\VDAC4Flasher.exe" @lib @ui
if ($LASTEXITCODE) { throw 'VDAC4Flasher build failed' }
& $csc @common @refs /target:exe "/out:$Out\FlasherTests.exe" @lib @tests
if ($LASTEXITCODE) { throw 'FlasherTests build failed' }
Get-Item "$Out\VDAC4Flasher.exe", "$Out\FlasherTests.exe" | Format-Table Name, Length

if ($Release) {
    $rel = Join-Path $Out 'VDAC4 Flasher'
    # the release folder is where users keep their Backup: never delete it with backups inside
    if ((Test-Path "$rel\Backup") -and (Get-ChildItem "$rel\Backup" -File)) { throw "$rel\Backup has backups - move them first or use another -Out" }
    if (Test-Path $rel) { Remove-Item -Recurse -Force $rel }
    New-Item -ItemType Directory -Force "$rel\firmware\esp", "$rel\firmware\bl616", "$rel\firmware\tang" | Out-Null
    Copy-Item "$Out\VDAC4Flasher.exe" $rel
    $v4 = Split-Path -Parent $here
    Copy-Item "$v4\ESP32 S3 Zero N8R8\Firmware\firmware\vdac4\firmware.factory.bin" "$rel\firmware\esp\"
    Copy-Item "$v4\Tang Nano 20k\BL616\firmware\vdac4_copro_bl616.bin" "$rel\firmware\bl616\"
    Copy-Item "$v4\Tang Nano 20k\Firmware\firmware\ts_fb_hdmi_top.fs" "$rel\firmware\tang\"
    Copy-Item "$v4\Tang Nano 20k\Firmware\firmware\ts_fb_slot1.fs" "$rel\firmware\tang\"
    Copy-Item "$v4\Tang Nano 20k\Firmware\firmware\ts_fb_video.bin" "$rel\firmware\tang\"
    & "$Out\FlasherTests.exe" --make-ini $v4 "$rel\firmware\firmware.ini"     # manifest for these images
    if ($LASTEXITCODE) { throw 'firmware.ini for the release failed' }
    Copy-Item "$here\README.txt" $rel
    Write-Host "Release folder: $rel"
}
