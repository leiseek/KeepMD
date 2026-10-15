"""Native menu and caption regressions for the compact workspace; uses disposable profiles and files."""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import subprocess
import time
from test_gui import ROOT, OUT, u, windows, children, name, wait, CALLBACK

u.GetDlgItem.argtypes = [W.HWND, C.c_int]
u.GetDlgItem.restype = W.HWND
u.MoveWindow.argtypes = [W.HWND, C.c_int, C.c_int, C.c_int, C.c_int, W.BOOL]
u.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.ScreenToClient.argtypes = [W.HWND, C.POINTER(W.POINT)]


def command(h, value):
    u.SendMessageW(h, 0x111, value, 0)


def caption(h):
    return next(c for c in children(h) if name(c, True) == 'Keep.Caption')


def control(h, value):
    return u.GetDlgItem(h, value) or u.GetDlgItem(caption(h), value)


def button(h, value):
    child = control(h, value)
    assert child and u.IsWindowVisible(child), value
    u.SendMessageW(child, 0xF5, 0, 0)  # BM_CLICK, native button command routing


def rect(h, parent):
    r = W.RECT()
    u.GetWindowRect(h, C.byref(r))
    a, b = W.POINT(r.left, r.top), W.POINT(r.right, r.bottom)
    u.ScreenToClient(parent, C.byref(a))
    u.ScreenToClient(parent, C.byref(b))
    return a.x, a.y, b.x, b.y


def contains(h, parent):
    a, b, c, d = rect(h, parent)
    r = W.RECT()
    u.GetClientRect(parent, C.byref(r))
    assert 0 <= a < c <= r.right and 0 <= b < d <= r.bottom, (name(h), (a,b,c,d), r.right, r.bottom)


def disjoint(h, ids):
    boxes = [rect(control(h, i), h) for i in ids]
    for i, a in enumerate(boxes):
        for b in boxes[i+1:]:
            assert a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1], (a, b)


def top(pid, prefix):
    found = []
    def collect(h, _):
        p = W.DWORD()
        u.GetWindowThreadProcessId(h, C.byref(p))
        if p.value == pid and name(h, True).startswith(prefix):
            found.append(h)
        return True
    cb = CALLBACK(collect)
    u.EnumWindows(cb, 0)
    return (found or [None])[0]


def text(h):
    return name(h).replace('\r\n', '\n').replace('\r', '\n')


folder = OUT / 'brand-interactions'
folder.mkdir(exist_ok=True)
checks = []
config = folder / 'reader.ini'
config.write_text('[Reader]\nDark=0\nVisual=1\n', encoding='utf-8')
path = folder / 'document.md'
original = '# 编辑与预览\n\n原始内容。\n'
path.write_text(original, encoding='utf-8', newline='')
p = subprocess.Popen([str(ROOT/'build/release/keepmd.exe'), str(path), '--config', str(config)], cwd=ROOT)
try:
    h = wait(lambda: (windows(p.pid) or [None])[0])
    wait(lambda: path.name in name(h))
    status = wait(lambda: next((c for c in children(h) if name(c, True) == 'msctls_statusbar32'), None))
    wait(lambda: name(status).startswith('阅读'))
    for width in (640, 850, 1180):
        u.MoveWindow(h, 40, 40, width, 700, True)
        bar = caption(h)
        for i in (100, 120, 118, 130, 131, 132):
            assert not u.GetDlgItem(h, i), 'the standalone action row must be removed'
        for i in (130,131,132):
            contains(control(h, i), bar)
        disjoint(h, (130,131,132,9101,9102,9103,9104,9200,9201,9202))
        toolbar = next(c for c in children(h) if name(c, True) == 'ToolbarWindow32')
        assert rect(toolbar,h)[1] - rect(bar,h)[3] <= 8 * u.GetDpiForWindow(h)/96
    assert [name(control(h,i)) for i in (130,131,132)] == ['阅读','可视化编辑','源码编辑']
    checks.append('no standalone action row; named mode icons fit the 32px caption at 640, 850 and 1180px')
    button(h, 131)
    edit = wait(lambda: u.GetDlgItem(h, 202))
    wait(lambda: text(edit) == original)
    u.SendMessageW(edit, 0xB1, -1, -1)  # select end
    extra = C.create_unicode_buffer('追加的内容。')
    u.SendMessageW(edit, 0xC2, 1, C.cast(extra, C.c_void_p).value)
    expected = original + '追加的内容。'
    wait(lambda: text(edit) == expected)
    assert name(h).startswith('* ')
    button(h, 130)
    wait(lambda: not u.IsWindowVisible(edit))
    assert not top(p.pid, '#32770'), 'reading preview must not force Save/Discard'
    assert path.read_text(encoding='utf-8') == original
    button(h, 131)
    assert u.GetDlgItem(h, 202) == edit and u.IsWindowVisible(edit)
    assert text(edit) == expected and name(h).startswith('* ')
    u.SendMessageW(edit, 0x304, 0, 0)  # WM_UNDO: same editor and undo history across preview
    wait(lambda: text(edit) == original)
    checks.append('reading preview preserves the unsaved buffer and undo history without a save dialog')
    button(h, 132)
    edit = wait(lambda: u.GetDlgItem(h, 202))
    assert text(edit) == original
    button(h, 131)
    edit = wait(lambda: u.GetDlgItem(h, 202))
    assert text(edit) == original
    command(h, 118)
    assert path.read_text(encoding='utf-8') == original
    checks.append('source / visual buttons preserve Markdown and the normal Save action')
    command(h, 128)
    assert all(u.IsWindowVisible(control(h, i)) for i in (130,131,132))
    command(h, 106)
    command(h, 128)
    checks.append('caption mode icons remain available with the toolbar collapsed and in dark theme')
    u.PostMessageW(caption(h), 0x111, 9200, 0)
    menu = wait(lambda: top(p.pid, '#32768'))
    u.PostMessageW(h, 0x1F, 0, 0)
    wait(lambda: not u.IsWindowVisible(menu))
    command(h, 120)
    edit = wait(lambda: u.GetDlgItem(h, 202))
    assert not text(edit) and u.IsWindowVisible(edit)
    assert name(status) == '可视化编辑'
    checks.append('File menu opens from the caption and its New command creates a blank visual document')
    u.PostMessageW(h, 0x10, 0, 0)
    p.wait(timeout=8)
finally:
    if p.poll() is None:
        p.terminate(); p.wait(timeout=5)

config = folder / 'prompt.ini'
config.write_text('[Prompt]\nDark=0\nResident=0\nHotkey=Ctrl+Alt+F11\n', encoding='utf-8')
draft = config.with_suffix('.draft.md')
sample = '# 原始草稿\n\n这些内容必须保留。\n'
draft.write_text(sample, encoding='utf-8', newline='')
p = subprocess.Popen([str(ROOT/'build/release/KeepPrompt.exe'), '--show', '--config', str(config)], cwd=ROOT)
try:
    h = wait(lambda: top(p.pid, 'KeepPrompt.'))
    edit = wait(lambda: u.GetDlgItem(h, 500))
    wait(lambda: text(edit) == sample)
    for width, height in ((720,520), (1040,800)):
        u.MoveWindow(h, 80, 50, width, height, True)
        for child in children(h):
            if u.IsWindowVisible(child):
                contains(child, h)
        disjoint(h, (510,511,512,500,501,549))
        assert all(not u.GetDlgItem(h,i) for i in (514,527,529)), 'prompt header actions must only live in menus'
        assert rect(u.GetDlgItem(h,540),h)[1] - rect(caption(h),h)[3] <= 18 * u.GetDpiForWindow(h)/96
    checks.append('prompt header row is removed; formatting and editor move up at compact and large sizes')
    command(h, 514)
    u.MoveWindow(h, 80, 50, 720, 620, True)
    for child in children(h):
        if u.IsWindowVisible(child):
            contains(child, h)
    disjoint(h, (502,515,516,517,500,510,511,512,501))
    u.PostMessageW(u.GetDlgItem(h,502), 0x100, 0x1B, 0)
    wait(lambda: not u.IsWindowVisible(u.GetDlgItem(h,502)))
    assert u.IsWindowVisible(h)
    checks.append('settings fit at 720px; Escape closes settings before hiding the panel')
    # The copy action is explicitly tested even with an empty draft elsewhere. Holding the clipboard
    # proves Esc can hide without copying or replacing the clipboard, unlike Ctrl+Enter.
    wait(lambda: u.OpenClipboard(None))
    try:
        u.PostMessageW(edit, 0x100, 0x1B, 0)
        wait(lambda: not u.IsWindowVisible(h))
    finally:
        u.CloseClipboard()
    assert draft.read_text(encoding='utf-8') == sample
    broker = subprocess.Popen([str(ROOT/'build/release/KeepPrompt.exe'), '--show', '--config', str(config)], cwd=ROOT)
    broker.wait(timeout=8)
    wait(lambda: u.IsWindowVisible(h))
    assert text(edit) == sample
    checks.append('Escape hides even while clipboard is busy and keeps the draft exact')
    # The menu and its commands are addressed to this process, without taking desktop keyboard focus.
    u.PostMessageW(caption(h), 0x111, 9201, 0)
    menu = wait(lambda: top(p.pid, '#32768'))
    u.PostMessageW(h, 0x1F, 0, 0)  # WM_CANCELMODE closes the menu without modifying the draft.
    wait(lambda: not u.IsWindowVisible(menu))
    assert text(edit) == sample
    command(h, 560)  # The same task-template command returned by the native menu.
    wait(lambda: '输出要求' in text(edit))
    assert text(edit).startswith(sample)
    u.SendMessageW(edit, 0x304, 0, 0)
    wait(lambda: text(edit) == sample)
    checks.append('template menu appends structured text and one Undo restores the exact existing draft')
    button(h, 512)
    wait(lambda: not text(edit))
    u.SendMessageW(edit, 0x304, 0, 0)
    wait(lambda: text(edit) == sample)
    checks.append('Clear remains reversible after template insertion')
    command(h, 525)
    p.wait(timeout=8)
finally:
    if p.poll() is None:
        p.terminate(); p.wait(timeout=5)

result = {'exe_sha256': hashlib.sha256((ROOT/'build/release/keepmd.exe').read_bytes()).hexdigest(),
          'prompt_exe_sha256': hashlib.sha256((ROOT/'build/release/KeepPrompt.exe').read_bytes()).hexdigest(),
          'checks': checks}
(folder/'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
(ROOT/'bench/results/brand-ui-e2e.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print(json.dumps(result, ensure_ascii=False, indent=2))
