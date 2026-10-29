"""Code block syntax colours and the copy chip in the real reader: token colours are asserted from
rendered pixels in light and dark themes, the chip is located from its frame in the screenshot and
clicked with posted messages, and the clipboard must hold the exact fenced source. The clipboard is
cleared first so a stale system value can never pass for a copy."""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import subprocess
import time
from PIL import Image
from test_gui import ROOT, OUT, u, k, windows, children, name, wait, activate, screenshot, clipboard

FIXTURE = ROOT / 'tests/fixtures/syntax.md'
CPP_SOURCE = '// compute the answer\nint total = 42;\nconst char* name = "keepmd";\nreturn printf("%d\\n", total);\n'
PYTHON_SOURCE = 'def greet(name):\n    # greet the reader\n    return f"Hello, {name}!"\n'
JSON_SOURCE = '{\n  "name": "KeepMD",\n  "version": 6,\n  "native": true\n}\n'
PLAIN_SOURCE = 'plain block without a known language\n'
ANY_SOURCE = {CPP_SOURCE, PYTHON_SOURCE, JSON_SOURCE, PLAIN_SOURCE}
exe = ROOT / 'build/release/keepmd.exe'
report = ROOT / 'bench/results/syntax-e2e.json'
report.parent.mkdir(parents=True, exist_ok=True)
config = OUT / 'syntax-e2e.ini'
config.write_text('[Reader]\nVisual=0\nDark=0\n', encoding='utf-8')
process = subprocess.Popen([str(exe), str(FIXTURE), '--config', str(config)], cwd=ROOT)
checks = []

# Reader token colours mirrored from view.cpp; antialiasing moves pixels, glyph cores do not.
LIGHT_TOKENS = {'keyword': (175, 34, 45), 'builtin': (150, 62, 8), 'string': (14, 52, 118),
                'comment': (100, 109, 122), 'number': (5, 76, 168), 'function': (121, 72, 212)}
DARK_TOKENS = {'keyword': (255, 116, 106), 'builtin': (255, 166, 87), 'string': (165, 214, 255),
               'comment': (139, 148, 158), 'number': (121, 192, 255), 'function': (210, 168, 255)}
LIGHT_CODE_BG, DARK_CODE_BG = (237, 240, 244), (26, 28, 32)


def counts(image, palette, tolerance=16):
    pixels = image.convert('RGB').load()
    width, height = image.size
    found = {key: 0 for key in palette}
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            r, g, b = pixels[x, y]
            for key, (tr, tg, tb) in palette.items():
                if abs(r - tr) <= tolerance and abs(g - tg) <= tolerance and abs(b - tb) <= tolerance:
                    found[key] += 1
    return found


def near(a, b, tolerance=10):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


def find_chip(image, border, code_bg, scale):
    """The copy chip is the only short rounded frame of the border colour sitting on the code
    surface. Scanned bottom-up, below the toolbar, so toolbar buttons can never match."""
    pixels = image.convert('RGB').load()
    width, height = image.size
    low, high = int(38 * scale), int(66 * scale)
    for y in range(height - 2, int(110 * scale), -1):
        run = 0
        for x in range(width):
            if near(pixels[x, y], border):
                run += 1
                continue
            if low <= run <= high:
                left, right = x - run, x - 1
                top = y
                for bottom in range(top + int(14 * scale), min(top + int(28 * scale), height - 1)):
                    if all(near(pixels[cx, bottom], border) for cx in (left + 2, (left + right) // 2, right - 2)):
                        probe = min(bottom + int(8 * scale), height - 1)
                        if near(pixels[(left + right) // 2, probe], code_bg):
                            return (left + right) // 2, (top + bottom) // 2
            run = 0
    return None


def click_reader(reader, x, y):
    """Post a real press/release pair to the reader in physical client pixels."""
    lparam = (int(y) << 16) | (int(x) & 0xFFFF)
    u.PostMessageW(reader, 0x201, 0, lparam)  # WM_LBUTTONDOWN
    time.sleep(.05)
    u.PostMessageW(reader, 0x202, 0, lparam)  # WM_LBUTTONUP


def clear_clipboard():
    if u.OpenClipboard(None):
        u.EmptyClipboard()
        u.CloseClipboard()


def image_to_reader(hwnd, reader, x, y):
    frame = W.RECT()
    dwm = C.WinDLL('dwmapi')
    dwm.DwmGetWindowAttribute.argtypes = [W.HWND, W.DWORD, C.c_void_p, W.DWORD]
    if dwm.DwmGetWindowAttribute(hwnd, 9, C.byref(frame), C.sizeof(frame)) != 0:
        u.GetWindowRect(hwnd, C.byref(frame))
    point = W.POINT(0, 0)
    u.ClientToScreen(reader, C.byref(point))
    return x + frame.left - point.x, y + frame.top - point.y


try:
    hwnd = wait(lambda: (windows(process.pid) or [None])[0])
    wait(lambda: '阅读' in name(hwnd))
    activate(hwnd)
    reader = next(h for h in children(hwnd) if name(h, True) == 'KeepMD.Reader')
    scale = u.GetDpiForWindow(hwnd) / 96.0

    screenshot(hwnd, 'syntax-light.png')
    light = Image.open(OUT / 'syntax-light.png')
    found = counts(light, LIGHT_TOKENS)
    missing = [key for key, n in found.items() if n < 6]
    checks.append(f'light theme shows all token colours (min 6 px each): {found}')
    assert not missing, f'light theme missing token colours: {missing} in {found}'

    chip = find_chip(light, (224, 227, 232), LIGHT_CODE_BG, scale)
    assert chip, 'copy chip not found in the light screenshot'
    rx, ry = image_to_reader(hwnd, reader, *chip)
    clear_clipboard()
    click_reader(reader, rx, ry)
    screenshot(hwnd, 'syntax-copied.png')  # Within the 1.5 s feedback window.
    copied_image = Image.open(OUT / 'syntax-copied.png').convert('RGB')
    box = (chip[0] - int(34 * scale), chip[1] - int(14 * scale), chip[0] + int(34 * scale),
           chip[1] + int(14 * scale))
    before = light.crop(box).tobytes()
    after = copied_image.crop(box).tobytes()
    changed = sum(1 for a, b in zip(before, after) if a != b)
    assert changed > 20, f'the chip label did not switch to its copied feedback ({changed} px)'
    checks.append(f'the chip shows its copied feedback for a while after the click ({changed} px changed)')
    copied = wait(lambda: clipboard() or None, timeout=5)
    assert copied in ANY_SOURCE, repr(copied)
    checks.append('clicking a code block chip copies that block\'s whole fenced source exactly')

    u.SendMessageW(hwnd, 0x111, 106, 0)  # Theme
    time.sleep(.4)
    screenshot(hwnd, 'syntax-dark.png')
    dark = Image.open(OUT / 'syntax-dark.png')
    found = counts(dark, DARK_TOKENS)
    missing = [key for key, n in found.items() if n < 6]
    checks.append(f'dark theme shows all token colours (min 6 px each): {found}')
    assert not missing, f'dark theme missing token colours: {missing} in {found}'
    chip = find_chip(dark, (53, 56, 62), DARK_CODE_BG, scale)
    assert chip, 'copy chip not found in the dark screenshot'
    checks.append('the copy chip renders in the dark theme too')

    u.PostMessageW(hwnd, 0x10, 0, 0)
    process.wait(timeout=10)
    assert process.returncode == 0
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)

result = {'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'fixture': FIXTURE.name,
          'checks': checks}
report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, ensure_ascii=False, indent=2))
