"""Real keyboard/installed IME check, restricted to the generated KeepMD window."""
import ctypes as C
from ctypes import wintypes as W
import json
import subprocess
import time
from test_gui import ROOT,OUT,u,windows,children,name,wait,screenshot

u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetForegroundWindow.restype=W.HWND
u.AttachThreadInput.argtypes=[W.DWORD,W.DWORD,W.BOOL]
u.BringWindowToTop.argtypes=[W.HWND]
u.SetFocus.argtypes=[W.HWND]
class KEYBDINPUT(C.Structure):_fields_=[('vk',W.WORD),('scan',W.WORD),('flags',W.DWORD),('time',W.DWORD),('extra',C.c_size_t)]
class MOUSEINPUT(C.Structure):_fields_=[('dx',W.LONG),('dy',W.LONG),('data',W.DWORD),('flags',W.DWORD),('time',W.DWORD),('extra',C.c_size_t)]
class UNION(C.Union):_fields_=[('keyboard',KEYBDINPUT),('mouse',MOUSEINPUT)]
class INPUT(C.Structure):_fields_=[('type',W.DWORD),('data',UNION)]
u.SendInput.argtypes=[W.UINT,C.POINTER(INPUT),C.c_int];u.SendInput.restype=W.UINT

path=OUT/'ime-input.md';path.write_text('# IME\n\n',encoding='utf-8')
config=OUT/'ime.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(path),'--config',str(config)],cwd=ROOT)
attempts=[];toggled=False
previousForeground=u.GetForegroundWindow()
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:path.name in name(hwnd))
    u.SetForegroundWindow(hwnd);u.SendMessageW(hwnd,0x111,117,0)
    editor=wait(lambda:u.GetDlgItem(hwnd,202))
    currentThread=C.windll.kernel32.GetCurrentThreadId()
    foregroundThread=u.GetWindowThreadProcessId(u.GetForegroundWindow(),None)
    editorThread=u.GetWindowThreadProcessId(hwnd,None)
    attached=[]
    for target in {foregroundThread,editorThread}:
        if target and target!=currentThread and u.AttachThreadInput(currentThread,target,True):attached.append(target)
    try:
        u.BringWindowToTop(hwnd);u.SetForegroundWindow(hwnd);u.SetFocus(editor)
    finally:
        for target in attached:u.AttachThreadInput(currentThread,target,False)
    time.sleep(.2)
    u.SendMessageW(editor,0xB1,-1,-1)
    def key(vk):
        if u.GetForegroundWindow()!=hwnd:raise RuntimeError('Focus left the test window; no keys sent')
        events=(INPUT*2)();events[0].type=events[1].type=1
        events[0].data.keyboard.vk=events[1].data.keyboard.vk=vk
        events[1].data.keyboard.flags=2
        if u.SendInput(2,events,C.sizeof(INPUT))!=2:raise RuntimeError('SendInput failed')
        time.sleep(.08)
    for attempt in range(2):
        for c in 'NIHAO':key(ord(c))
        time.sleep(.15);screenshot(hwnd,f'ime-candidate-{attempt}.png')
        key(0x20);time.sleep(.2)
        current=name(editor);attempts.append(current)
        if any('\u4e00'<=c<='\u9fff' for c in current):break
        key(0x1B)
        u.SendMessageW(editor,0xB1,0,-1)
        empty=C.create_unicode_buffer('# IME\n\n');u.SendMessageW(editor,0xC2,1,C.cast(empty,C.c_void_p).value)
        key(0x10);toggled=not toggled
    else:raise AssertionError(f'Installed IME did not commit Chinese text: {attempts}')
    screenshot(hwnd,'ime-committed.png')
    u.SendMessageW(hwnd,0x111,118,0)
    assert any('\u4e00'<=c<='\u9fff' for c in path.read_text(encoding='utf-8'))
    if toggled:key(0x10);toggled=False
    u.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=5)
    result={'real_keyboard_input':'NIHAO + Space','attempts':attempts,'Chinese_commit_and_save':'passed'}
    (ROOT/'bench/results/ime-e2e.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
finally:
    if process.poll() is None:
        if toggled and u.GetForegroundWindow()==hwnd:
            try:key(0x10)
            except Exception:pass
        process.terminate();process.wait(timeout=5)
    if previousForeground:u.SetForegroundWindow(previousForeground)
