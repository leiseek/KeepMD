$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$devcmd = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
Push-Location $root
try {
    if (-not (Test-Path '.cache/tinta/CMakeLists.txt')) { throw 'Clone the pinned reference to .cache/tinta first.' }
    $head = git -C .cache/tinta rev-parse HEAD
    if ($head -ne 'db70698e49a1f700c7ad98add1bebe713ac796bd') { throw 'Reference commit does not match the audited source.' }
    $line = '"{0}" -no_logo -arch=x64 -host_arch=x64 && "{1}" -S .cache/tinta -B .cache/tinta-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF && "{1}" --build .cache/tinta-build' -f $devcmd,$cmake
    & $env:ComSpec /d /s /c $line
    if ($LASTEXITCODE -ne 0) { throw "Reference build failed: $LASTEXITCODE" }
} finally { Pop-Location }
