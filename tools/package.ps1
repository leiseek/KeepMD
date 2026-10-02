param([string]$Version='0.4.3')
$ErrorActionPreference='Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([.-][A-Za-z0-9]+)*$') { throw 'Invalid package version.' }
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root 'build/release/keepmd.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build KeepMD first.' }
$folder=Join-Path $root "dist/KeepMD-$Version-windows-x64"
New-Item -ItemType Directory -Path $folder,(Join-Path $folder 'licenses'),(Join-Path $folder 'examples'),(Join-Path $folder 'bench/results') -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $folder 'keepmd.exe')
Copy-Item -LiteralPath (Join-Path $root 'docs/USAGE.zh-CN.md') -Destination (Join-Path $folder '使用说明.md')
Copy-Item -LiteralPath (Join-Path $root 'docs/VALIDATION.zh-CN.md') -Destination (Join-Path $folder '验证报告.md')
Copy-Item -LiteralPath (Join-Path $root 'docs/PROMPT-FLOW.zh-CN.md') -Destination (Join-Path $folder 'PromptFlow集成说明.md')
$(Get-Content -LiteralPath (Join-Path $folder 'PromptFlow集成说明.md') -Raw -Encoding UTF8).Replace('VALIDATION.zh-CN.md','验证报告.md') | Set-Content -LiteralPath (Join-Path $folder 'PromptFlow集成说明.md') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $root 'tests/fixtures/prompt.md') -Destination (Join-Path $folder 'examples/提示词.md')
[IO.File]::WriteAllText((Join-Path $folder '输入提示词.cmd'), "@echo off`r`nstart `"`" `"%~dp0keepmd.exe`" --prompt`r`n", [Text.Encoding]::ASCII)
$(Get-Content -LiteralPath (Join-Path $folder '验证报告.md') -Raw -Encoding UTF8).Replace('../bench/results/','bench/results/') | Set-Content -LiteralPath (Join-Path $folder '验证报告.md') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $root 'third_party/md4c/LICENSE.md') -Destination (Join-Path $folder 'licenses/MD4C.txt')
Copy-Item -LiteralPath (Join-Path $root 'third_party/tinta/LICENSE') -Destination (Join-Path $folder 'licenses/Tinta.txt')
Copy-Item -LiteralPath (Join-Path $root 'tests/fixtures/welcome.md') -Destination (Join-Path $folder 'examples/欢迎.md')
$(Get-Content -LiteralPath (Join-Path $folder 'examples/欢迎.md') -Raw -Encoding UTF8).Replace('../../README.md','../使用说明.md') | Set-Content -LiteralPath (Join-Path $folder 'examples/欢迎.md') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $root 'tests/fixtures/flow-details.md') -Destination (Join-Path $folder 'examples/流程图.md')
Copy-Item -LiteralPath (Join-Path $root 'third_party/manifest.json') -Destination (Join-Path $folder 'DEPENDENCIES.json')
$(Get-Content -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Raw -Encoding UTF8).Replace('third_party/md4c/LICENSE.md','licenses/MD4C.txt').Replace('third_party/tinta/LICENSE','licenses/Tinta.txt').Replace('third_party/manifest.json','DEPENDENCIES.json') | Set-Content -LiteralPath (Join-Path $folder '第三方说明.md') -Encoding UTF8
$relativeFiles=@('keepmd.exe','使用说明.md','验证报告.md','第三方说明.md','DEPENDENCIES.json','licenses/MD4C.txt','licenses/Tinta.txt','examples/欢迎.md','examples/流程图.md')
$relativeFiles+=@('PromptFlow集成说明.md','examples/提示词.md','输入提示词.cmd')
foreach ($result in @('software-s100.json','software-l50.json','scroll.json','stress.json','editor-e2e.json','navigation-e2e.json','dpi-e2e.json','ime-e2e.json','environment.json','dependencies.json','idle.json','release-smoke.json','release-summary.json','prompt-e2e.json','ui-e2e.json','scrollbar-e2e.json','visual-editor-e2e.json','caption-e2e.json','icons-e2e.json','toolbar-toggle-e2e.json')) {
    Copy-Item -LiteralPath (Join-Path $root "bench/results/$result") -Destination (Join-Path $folder "bench/results/$result")
    $relativeFiles += "bench/results/$result"
}
$hashes=foreach ($relative in $relativeFiles) { '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $folder $relative) -Algorithm SHA256).Hash.ToLower(),$relative }
Set-Content -LiteralPath (Join-Path $folder 'SHA256SUMS.txt') -Value $hashes -Encoding utf8
$relativeFiles+='SHA256SUMS.txt'
$zip=Join-Path $root "dist/KeepMD-$Version-windows-x64.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream=[IO.File]::Open($zip,[IO.FileMode]::Create)
$archive=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
try { foreach ($relative in $relativeFiles) { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $folder $relative),$relative,[IO.Compression.CompressionLevel]::Optimal) | Out-Null } } finally { $archive.Dispose();$stream.Dispose() }
Get-Item -LiteralPath $zip,(Join-Path $folder 'keepmd.exe') | Select-Object FullName,Length
Get-FileHash -LiteralPath $zip -Algorithm SHA256
