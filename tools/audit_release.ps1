$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$vswhere='C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$devcmd=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$exe=Join-Path $root 'build/release/keepmd.exe'
$line='"{0}" -no_logo -arch=x64 -host_arch=x64 && dumpbin /dependents "{1}"' -f $devcmd,$exe
$output=& $env:ComSpec /d /s /c $line
if ($LASTEXITCODE -ne 0) { throw 'PE import inspection failed.' }
$imports=@($output | ForEach-Object { if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1] } })
if (-not $imports.Count) { throw 'No PE imports parsed.' }
if ($imports -match '(?i)(webview|electron|chrom|webkit|qt|node|vcruntime|msvcp\d)') { throw 'Unexpected runtime dependency detected.' }
$result=[ordered]@{exe_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower();static_imports=$imports;static_import_audit='No browser, Node, Qt, or separate MSVC runtime DLL imports.';optional_editor='Windows system Msftedit.dll loaded on demand; not bundled.'}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'bench/results/dependencies.json') -Encoding UTF8
$result | ConvertTo-Json -Depth 5
