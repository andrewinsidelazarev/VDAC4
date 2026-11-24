# Сборка «Консоли VDAC4» (VDAC4Console.exe) и проверок на ПК (ConsoleTests.exe).
# Нужен только компилятор C# Roslyn (Visual Studio 2022 Build Tools); программа — .NET Framework 4.8,
# он есть в каждой Windows 10/11, ставить ничего не нужно.
#   build.ps1 [-Out папка] [-Test]   — -Test сразу гоняет проверки, -Snapshot делает картинки окна
# Messages in Latin: Windows PowerShell 5.1 reads BOM-less files as ANSI.
param(
    [string]$Out = 'E:\zx\cache\tmp\vdac4console\build',
    [switch]$Test,
    [switch]$Snapshot
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$csc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found: $csc" }
$fw = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319'
$refs = @('System.dll', 'System.Core.dll', 'System.Windows.Forms.dll', 'System.Drawing.dll') |
    ForEach-Object { "/reference:$fw\$_" }
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TMP = $Out; $env:TEMP = $Out

$lib = @(Get-ChildItem "$here\src\*.cs" | Where-Object { $_.Name -ne 'Program.cs' } | ForEach-Object { $_.FullName })
$tests = @(Get-ChildItem "$here\tests\*.cs" | ForEach-Object { $_.FullName })   # @(): one file must still be a list
$common = @('/nologo', '/noconfig', '/nostdlib+', "/reference:$fw\mscorlib.dll", '/langversion:7.3', '/optimize+',
            '/platform:anycpu', '/warn:4', '/utf8output', '/codepage:65001')

& $csc @common @refs /target:winexe "/win32manifest:$here\app.manifest" "/out:$Out\VDAC4Console.exe" @lib "$here\src\Program.cs"
if ($LASTEXITCODE) { throw 'VDAC4Console build failed' }
& $csc @common @refs /target:exe "/win32manifest:$here\app.manifest" "/out:$Out\ConsoleTests.exe" @lib @tests
if ($LASTEXITCODE) { throw 'ConsoleTests build failed' }
Get-Item "$Out\VDAC4Console.exe", "$Out\ConsoleTests.exe" | Format-Table Name, Length

if ($Test) {
    & "$Out\ConsoleTests.exe" (Join-Path $Out 'test')
    if ($LASTEXITCODE) { throw 'ConsoleTests failed' }
}
if ($Snapshot) {
    $p = Start-Process -FilePath "$Out\VDAC4Console.exe" -ArgumentList '--snapshot', (Join-Path $Out 'snapshot') -Wait -PassThru
    if ($p.ExitCode) { throw 'snapshot failed' }
}
