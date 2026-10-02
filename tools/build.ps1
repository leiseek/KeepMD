param([switch]$Test,[switch]$Clean)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC C++ Build Tools are required.' }
$devcmd = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
Push-Location $root
try {
    $buildCommand = '"{0}" -no_logo -arch=x64 -host_arch=x64 && set "VSLANG=1033" && "{1}" --preset release && "{1}" --build --preset release' -f $devcmd, $cmake
    if ($Clean) { $buildCommand += ' --clean-first' }
    if ($Test) { $buildCommand += ' && "{0}" --preset release' -f $ctest }
    & $env:ComSpec /d /s /c $buildCommand
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
} finally { Pop-Location }
