param([string]$Version='0.7.0')
$ErrorActionPreference='Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([.-][A-Za-z0-9]+)*$') { throw 'Invalid package version.' }
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root 'build/release/keepmd.exe'
$promptExe=Join-Path $root 'build/release/KeepPrompt.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Build KeepMD first.' }
if (-not (Test-Path -LiteralPath $promptExe)) { throw 'Build KeepPrompt first.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem

# KeepMD and KeepPrompt are shipped as two separate portable packages: neither contains the other
# program, and each carries only its own documentation, examples, licences and verification results.
function Build-TextReplacements([string]$path, [hashtable]$rules, [string]$destination) {
    $text=Get-Content -LiteralPath $path -Raw -Encoding UTF8
    foreach ($rule in $rules.GetEnumerator()) { $text=$text.Replace($rule.Key, $rule.Value) }
    [IO.File]::WriteAllText($destination, $text, [Text.Encoding]::UTF8)
}

function Add-ZipEntries([string]$zipPath, [string]$folder, [string[]]$relativeFiles) {
    $stream=[IO.File]::Open($zipPath,[IO.FileMode]::Create)
    $archive=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
    try { foreach ($relative in $relativeFiles) { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $folder $relative),$relative,[IO.Compression.CompressionLevel]::Optimal) | Out-Null } }
    finally { $archive.Dispose();$stream.Dispose() }
}

function Write-Sums([string]$folder, [string[]]$relativeFiles) {
    $hashes=foreach ($relative in $relativeFiles) { '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $folder $relative) -Algorithm SHA256).Hash.ToLower(),$relative }
    Set-Content -LiteralPath (Join-Path $folder 'SHA256SUMS.txt') -Value $hashes -Encoding utf8
}

$licenseMap=[ordered]@{'third_party/md4c/LICENSE.md'='licenses/MD4C.txt';'third_party/tinta/LICENSE'='licenses/Tinta.txt';'third_party/microtex/LICENSE'='licenses/MicroTeX.txt';'third_party/microtex/res/fonts/licences/Knuth_License.txt'='licenses/MicroTeX-fonts-Knuth.txt';'third_party/microtex/res/fonts/licences/OFL.txt'='licenses/MicroTeX-fonts-OFL.txt';'third_party/microtex/res/fonts/licences/License_for_dsrom.txt'='licenses/MicroTeX-fonts-dsrom.txt';'third_party/microtex/LICENSE_BAKOMA'='licenses/MicroTeX-fonts-BaKoMA.txt';'third_party/tinyxml2/LICENSE.txt'='licenses/TinyXML-2.txt'}

# ---- KeepMD package: the reader/editor alone, without KeepPrompt. ----
$folder=Join-Path $root "dist/KeepMD-$Version-windows-x64"
New-Item -ItemType Directory -Path $folder,(Join-Path $folder 'licenses'),(Join-Path $folder 'examples'),(Join-Path $folder 'bench/results') -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $folder 'keepmd.exe')
Copy-Item -LiteralPath (Join-Path $root 'docs/USAGE.zh-CN.md') -Destination (Join-Path $folder '使用说明.md')
Copy-Item -LiteralPath (Join-Path $root 'docs/VALIDATION.zh-CN.md') -Destination (Join-Path $folder '验证报告.md')
Build-TextReplacements (Join-Path $folder '验证报告.md') @{'../bench/results/'='bench/results/'} (Join-Path $folder '验证报告.md')
Copy-Item -LiteralPath (Join-Path $root 'tests/fixtures/welcome.md') -Destination (Join-Path $folder 'examples/欢迎.md')
Build-TextReplacements (Join-Path $folder 'examples/欢迎.md') @{'../../README.md'='../使用说明.md'} (Join-Path $folder 'examples/欢迎.md')
Copy-Item -LiteralPath (Join-Path $root 'tests/fixtures/flow-details.md') -Destination (Join-Path $folder 'examples/流程图.md')
Copy-Item -LiteralPath (Join-Path $root 'tests/fixtures/math.md') -Destination (Join-Path $folder 'examples/公式.md')
foreach ($license in $licenseMap.GetEnumerator()) { Copy-Item -LiteralPath (Join-Path $root $license.Key) -Destination (Join-Path $folder $license.Value) }
Copy-Item -LiteralPath (Join-Path $root 'third_party/manifest.json') -Destination (Join-Path $folder 'DEPENDENCIES.json')
$notices=Get-Content -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Raw -Encoding UTF8
$notices=$notices.Replace('third_party/md4c/LICENSE.md','licenses/MD4C.txt').Replace('third_party/tinta/LICENSE','licenses/Tinta.txt').Replace('third_party/manifest.json','DEPENDENCIES.json')
foreach ($license in $licenseMap.GetEnumerator()) { $notices=$notices.Replace($license.Key,$license.Value) }
$notices | Set-Content -LiteralPath (Join-Path $folder '第三方说明.md') -Encoding UTF8
$keepmdResults=@('software-s100.json','software-l50.json','scroll.json','stress.json','editor-e2e.json','navigation-e2e.json','dpi-e2e.json','ime-e2e.json','environment.json','dependencies.json','idle.json','release-smoke.json','release-summary.json','ui-e2e.json','scrollbar-e2e.json','visual-editor-e2e.json','caption-e2e.json','icons-e2e.json','toolbar-toggle-e2e.json','visual-document-e2e.json','remote-images-e2e.json','brand-ui-e2e.json','fullscreen-e2e.json','math-e2e.json','html-images-e2e.json','syntax-e2e.json')
$relativeFiles=@('keepmd.exe','使用说明.md','验证报告.md','第三方说明.md','DEPENDENCIES.json','licenses/MD4C.txt','licenses/Tinta.txt','examples/欢迎.md','examples/流程图.md','examples/公式.md')
$relativeFiles+=$licenseMap.Values
foreach ($result in $keepmdResults) { Copy-Item -LiteralPath (Join-Path $root "bench/results/$result") -Destination (Join-Path $folder "bench/results/$result") }
$relativeFiles+=($keepmdResults | ForEach-Object { "bench/results/$_" })
Write-Sums $folder $relativeFiles
$relativeFiles+='SHA256SUMS.txt'
Add-ZipEntries (Join-Path $root "dist/KeepMD-$Version-windows-x64.zip") $folder $relativeFiles

# ---- KeepPrompt package: the prompt tool alone, without KeepMD. ----
$promptFolder=Join-Path $root "dist/KeepPrompt-$Version-windows-x64"
New-Item -ItemType Directory -Path $promptFolder,(Join-Path $promptFolder 'licenses'),(Join-Path $promptFolder 'examples'),(Join-Path $promptFolder 'bench/results') -Force | Out-Null
Copy-Item -LiteralPath $promptExe -Destination (Join-Path $promptFolder 'KeepPrompt.exe')
Copy-Item -LiteralPath (Join-Path $root 'docs/PROMPT-FLOW.zh-CN.md') -Destination (Join-Path $promptFolder 'PromptFlow集成说明.md')
Build-TextReplacements (Join-Path $promptFolder 'PromptFlow集成说明.md') @{'VALIDATION.zh-CN.md'='验证报告.md'} (Join-Path $promptFolder 'PromptFlow集成说明.md')
Copy-Item -LiteralPath (Join-Path $root 'tests/fixtures/prompt.md') -Destination (Join-Path $promptFolder 'examples/提示词.md')
[IO.File]::WriteAllText((Join-Path $promptFolder '输入提示词.cmd'), "@echo off`r`nstart `"`" `"%~dp0KeepPrompt.exe`" --show`r`n", [Text.Encoding]::ASCII)
Copy-Item -LiteralPath (Join-Path $root 'third_party/md4c/LICENSE.md') -Destination (Join-Path $promptFolder 'licenses/MD4C.txt')
Copy-Item -LiteralPath (Join-Path $root 'third_party/tinta/LICENSE') -Destination (Join-Path $promptFolder 'licenses/Tinta.txt')
Copy-Item -LiteralPath (Join-Path $root 'third_party/manifest.json') -Destination (Join-Path $promptFolder 'DEPENDENCIES.json')
$notices | Set-Content -LiteralPath (Join-Path $promptFolder '第三方说明.md') -Encoding UTF8
$promptResults=@('prompt-e2e.json','prompt-process-e2e.json')
$promptFiles=@('KeepPrompt.exe','PromptFlow集成说明.md','第三方说明.md','DEPENDENCIES.json','licenses/MD4C.txt','licenses/Tinta.txt','examples/提示词.md','输入提示词.cmd')
foreach ($result in $promptResults) { Copy-Item -LiteralPath (Join-Path $root "bench/results/$result") -Destination (Join-Path $promptFolder "bench/results/$result") }
$promptFiles+=($promptResults | ForEach-Object { "bench/results/$_" })
Write-Sums $promptFolder $promptFiles
$promptFiles+='SHA256SUMS.txt'
Add-ZipEntries (Join-Path $root "dist/KeepPrompt-$Version-windows-x64.zip") $promptFolder $promptFiles

Get-Item -LiteralPath (Join-Path $root "dist/KeepMD-$Version-windows-x64.zip"),(Join-Path $root "dist/KeepPrompt-$Version-windows-x64.zip") | Select-Object FullName,Length
Get-FileHash -LiteralPath (Join-Path $root "dist/KeepMD-$Version-windows-x64.zip"),(Join-Path $root "dist/KeepPrompt-$Version-windows-x64.zip") -Algorithm SHA256
