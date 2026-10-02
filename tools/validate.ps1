param([switch]$Performance,[switch]$SkipIme)
$ErrorActionPreference='Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    & "$PSScriptRoot/build.ps1" -Test
    if ($LASTEXITCODE -ne 0) { throw 'Core validation failed' }
    $env:PYTHONIOENCODING='utf-8'
    foreach ($script in @('generate_corpus.py','test_gui.py','test_navigation.py','test_editor.py','test_dpi.py','test_stress.py','test_ui.py','test_scrollbars.py','test_visual_editor.py')) {
        Write-Output "Validating $script"
        & python (Join-Path $PSScriptRoot $script)
        if ($LASTEXITCODE -ne 0) { throw "$script failed" }
    }
    if (-not $SkipIme) {
        & python "$PSScriptRoot/test_ime.py"
        if ($LASTEXITCODE -ne 0) { throw 'Native IME validation failed' }
        & python "$PSScriptRoot/test_prompt.py"
        if ($LASTEXITCODE -ne 0) { throw 'Prompt integration validation failed' }
    }
    if ($Performance) {
        & python "$PSScriptRoot/benchmark.py" --runs 30
        if ($LASTEXITCODE -ne 0) { throw 'Startup benchmark failed' }
        & python "$PSScriptRoot/benchmark_scroll.py"
        if ($LASTEXITCODE -ne 0) { throw 'Scroll benchmark failed' }
    }
} finally { Pop-Location }
