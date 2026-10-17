"""LaTeX formulas in the real reader: typeset in light, dark and zoomed layouts, while find and copy still see
the LaTeX source. Drives the window with messages only, so it does not depend on desktop focus."""
import ctypes as C
import hashlib
import json
import subprocess
from test_gui import ROOT, OUT, u, windows, children, name, wait, activate, screenshot, clipboard

FIXTURE = ROOT / 'tests/fixtures/math.md'
# tests/fixtures/math.md: fifteen formulas MicroTeX typesets (one with an unknown command it marks in red,
# four drawing symbols from Windows fonts)
# and one, "}{", it cannot recover from, which stays as source. "$5 和 $6" is prose.
TYPESET, SOURCE = 15, 1
exe = ROOT / 'build/release/keepmd.exe'
report = OUT / 'math-e2e.json'
config = OUT / 'math-e2e.ini'
config.write_text('[Reader]\nVisual=0\nDark=0\n', encoding='utf-8')
process = subprocess.Popen([str(exe), str(FIXTURE), '--config', str(config), '--report', str(report)], cwd=ROOT)
checks = []


def counts():
    u.SendMessageW(hwnd, 0x8000 + 100, 0, 0)  # Rewrites the diagnostic report.
    data = json.loads(report.read_text())
    return data['formulas_typeset'], data['formulas_source']


try:
    hwnd = wait(lambda: (windows(process.pid) or [None])[0])
    status = wait(lambda: next(h for h in children(hwnd) if name(h, True) == 'msctls_statusbar32'))
    wait(lambda: name(status).startswith('阅读'))
    activate(hwnd)
    wait(lambda: counts() == (TYPESET, SOURCE))
    screenshot(hwnd, 'math-light.png')
    checks.append('15 formulas typeset and one unrecoverable formula kept as source in the real reader')

    u.SendMessageW(hwnd, 0x111, 102, 0)  # Find
    edit = next(h for h in children(hwnd) if name(h, True) == 'Edit')
    query = C.create_unicode_buffer('mc^2')
    u.SendMessageW(edit, 0x000C, 0, C.cast(query, C.c_void_p).value)
    wait(lambda: '1 / 1' in name(status))
    screenshot(hwnd, 'math-find.png')
    u.SendMessageW(hwnd, 0x111, 102, 0)
    checks.append('find matches LaTeX source inside a typeset formula')

    u.SendMessageW(hwnd, 0x111, 111, 0)  # Select all
    u.SendMessageW(hwnd, 0x111, 110, 0)  # Copy
    copied = clipboard()
    for source in ('E = mc^2', '\\int_0^1 x^2 \\, dx = \\frac{1}{3}', '\\nosuchcommand', '}{',
                   '价格 $5 和 $6 不是公式。'):
        assert source in copied, (source, copied)
    checks.append('copy returns the exact LaTeX source of typeset formulas, and prose dollar signs')

    # Theme and zoom drop the cached layouts; counts return once the window has painted again.
    u.SendMessageW(hwnd, 0x111, 106, 0)  # Theme
    wait(lambda: counts() == (TYPESET, SOURCE))
    screenshot(hwnd, 'math-dark.png')
    u.SendMessageW(hwnd, 0x111, 107, 0)  # Zoom in
    u.SendMessageW(hwnd, 0x111, 107, 0)
    u.SendMessageW(hwnd, 0x111, 108, 0)  # Zoom out once: the whole fixture still fits the window.
    wait(lambda: counts() == (TYPESET, SOURCE))
    screenshot(hwnd, 'math-zoomed.png')
    checks.append('formulas are typeset again after a theme change and zoom')

    u.PostMessageW(hwnd, 0x10, 0, 0)
    process.wait(timeout=10)
    assert process.returncode == 0
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)

result = {'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'fixture': FIXTURE.name,
          'formulas_typeset': TYPESET, 'formulas_source': SOURCE, 'checks': checks}
(ROOT / 'bench/results/math-e2e.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n',
                                                encoding='utf-8')
print(json.dumps(result, ensure_ascii=False, indent=2))
