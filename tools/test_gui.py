"""Real Win32 smoke test: launches only KeepMD, exercises its window and clipboard."""
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import subprocess
import time
from PIL import ImageGrab

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/output'
OUT.mkdir(parents=True, exist_ok=True)
u = C.WinDLL('user32', use_last_error=True)
k = C.WinDLL('kernel32', use_last_error=True)
u.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.SendMessageW.restype = W.LPARAM
u.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.SetWindowTextW.argtypes = [W.HWND, W.LPCWSTR]
u.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
u.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
u.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
u.IsWindowVisible.argtypes = [W.HWND]
u.GetWindowRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.SetForegroundWindow.argtypes = [W.HWND]
u.GetForegroundWindow.restype=W.HWND
u.AttachThreadInput.argtypes=[W.DWORD,W.DWORD,W.BOOL]
u.BringWindowToTop.argtypes=[W.HWND]
u.SetFocus.argtypes=[W.HWND]
u.ClientToScreen.argtypes=[W.HWND,C.POINTER(W.POINT)]
u.GetAncestor.argtypes=[W.HWND,W.UINT];u.GetAncestor.restype=W.HWND
u.GetDpiForWindow.argtypes=[W.HWND];u.GetDpiForWindow.restype=W.UINT
u.mouse_event.argtypes=[W.DWORD,W.DWORD,W.DWORD,W.DWORD,C.c_size_t]
u.GetClipboardData.argtypes = [W.UINT]
u.GetClipboardData.restype = W.HANDLE
k.GlobalLock.argtypes = [W.HANDLE]
k.GlobalLock.restype = C.c_void_p
k.GlobalUnlock.argtypes = [W.HANDLE]
CALLBACK = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)

def name(hwnd, cls=False):
    text = C.create_unicode_buffer(2048)
    if cls:
        u.GetClassNameW(hwnd, text, len(text))
    else:
        u.SendMessageW(hwnd, 0x000D, len(text), C.cast(text,C.c_void_p).value)
    return text.value

def windows(pid):
    result = []
    def collect(hwnd, _):
        found = W.DWORD()
        u.GetWindowThreadProcessId(hwnd, C.byref(found))
        if found.value == pid and name(hwnd, True) == 'KeepMD.Window':
            result.append(hwnd)
        return True
    callback = CALLBACK(collect)
    u.EnumWindows(callback, 0)
    return result

def children(hwnd):
    found = []
    callback = CALLBACK(lambda child, _: found.append(child) or True)
    u.EnumChildWindows(W.HWND(hwnd), callback, 0)
    return found

def wait(predicate, timeout=10):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError('Timed out waiting for GUI state')

def screenshot(hwnd, name_):
    u.SetForegroundWindow(hwnd)
    time.sleep(.15)
    rect = W.RECT()
    u.GetWindowRect(hwnd, C.byref(rect))
    ImageGrab.grab(bbox=(rect.left,rect.top,rect.right,rect.bottom)).save(OUT / name_)

def activate(hwnd,focus=None):
    current=k.GetCurrentThreadId();attached=[]
    for target in {u.GetWindowThreadProcessId(u.GetForegroundWindow(),None),u.GetWindowThreadProcessId(hwnd,None)}:
        if target and target!=current and u.AttachThreadInput(current,target,True):attached.append(target)
    try:
        u.BringWindowToTop(hwnd);u.SetForegroundWindow(hwnd)
        if focus:u.SetFocus(focus)
    finally:
        for target in attached:u.AttachThreadInput(current,target,False)

def click(hwnd,x,y):
    root=u.GetAncestor(hwnd,2);activate(root,hwnd)
    if u.GetForegroundWindow()!=root:raise RuntimeError('Test window is not foreground; no click sent')
    point=W.POINT(x,y);u.ClientToScreen(hwnd,C.byref(point));u.SetCursorPos(point.x,point.y);time.sleep(.05)
    u.mouse_event(2,0,0,0,0);time.sleep(.04);u.mouse_event(4,0,0,0,0)

def clipboard():
    if not u.OpenClipboard(None):
        return ''
    try:
        handle = u.GetClipboardData(13)
        ptr = k.GlobalLock(handle) if handle else 0
        if not ptr:
            return ''
        value = C.wstring_at(ptr)
        k.GlobalUnlock(handle)
        return value
    finally:
        u.CloseClipboard()

def run():
    report = OUT / 'gui-smoke.json'
    env = os.environ.copy()
    if '--software' in __import__('sys').argv:
        env['KEEPMD_RENDERER'] = 'software'
    elif '--hardware' in __import__('sys').argv:
        env['KEEPMD_RENDERER'] = 'hardware'
    config=OUT/'gui-test.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
    process = subprocess.Popen([str(ROOT/'build/release/keepmd.exe'), str(ROOT/'tests/fixtures/welcome.md'), '--report', str(report), '--config',str(config)], cwd=ROOT, env=env)
    checks = []
    try:
        hwnd = wait(lambda: (windows(process.pid) or [None])[0])
        wait(lambda:any(name(h,True)=='KeepMD.Reader' for h in children(hwnd)))
        child = children(hwnd)
        reader = next(h for h in child if name(h,True)=='KeepMD.Reader')
        status = next(h for h in child if name(h,True)=='msctls_statusbar32')
        wait(lambda: name(status).startswith('阅读'))
        checks.append('document loaded')
        screenshot(hwnd,'gui-top.png')
        u.SendMessageW(hwnd,0x111,105,0)  # table of contents
        toc = next(h for h in children(hwnd) if name(h,True)=='SysListView32')
        assert u.IsWindowVisible(toc)
        assert u.SendMessageW(toc,0x1004,0,0) >= 5
        scale=u.GetDpiForWindow(toc)/96
        click(toc,int(20*scale),int(28*scale))
        u.SendMessageW(hwnd,0x8000+100,0,0)
        assert json.loads(report.read_text())['anchor_block']>0
        before=json.loads(report.read_text())['anchor_block']
        u.SendMessageW(toc,0x102,ord('Z'),0)
        u.SendMessageW(hwnd,0x8000+100,0,0)
        assert json.loads(report.read_text())['anchor_block']==before
        checks.append('outline populated')
        u.SendMessageW(hwnd,0x111,105,0)
        u.SendMessageW(hwnd,0x111,102,0)  # find
        edit = next(h for h in children(hwnd) if name(h,True)=='Edit')
        query=C.create_unicode_buffer('数据')
        u.SendMessageW(edit,0x000C,0,C.cast(query,C.c_void_p).value)
        print('Search text:',name(edit),'Status:',name(status),flush=True)
        screenshot(hwnd,'gui-search.png')
        wait(lambda: '1 / 1' in name(status))
        checks.append('find Chinese text')
        u.SendMessageW(hwnd,0x111,102,0)
        u.SendMessageW(hwnd,0x111,111,0)  # select all
        u.SendMessageW(hwnd,0x111,110,0)  # copy
        assert '原生阅读' in clipboard() and 'return 0;' in clipboard()
        checks.append('copy full semantic text')
        u.SendMessageW(reader,0x100,0x23,0)  # End
        time.sleep(.7)
        screenshot(hwnd,'gui-diagram.png')
        u.SendMessageW(hwnd,0x111,106,0)  # theme
        screenshot(hwnd,'gui-dark.png')
        checks.append('scroll and theme')
        u.PostMessageW(hwnd,0x10,0,0)
        process.wait(timeout=10)
        assert process.returncode == 0
        assert report.exists()
        print(json.dumps({'checks':checks,'metrics':json.loads(report.read_text())},ensure_ascii=False,indent=2))
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)

if __name__ == '__main__':
    run()
