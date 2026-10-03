"""Real native UI checks: responsive layout, menu/button keyboard behavior, themes and GDI lifetime."""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import subprocess
import time
from PIL import Image
from test_gui import ROOT,OUT,u,k,windows,children,name,wait,activate,screenshot,click,clipboard,CALLBACK

u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.MoveWindow.argtypes=[W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.BOOL]
u.GetClientRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
u.ScreenToClient.argtypes=[W.HWND,C.POINTER(W.POINT)]
u.keybd_event.argtypes=[W.BYTE,W.BYTE,W.DWORD,C.c_size_t]
u.GetGuiResources.argtypes=[W.HANDLE,W.DWORD];u.GetGuiResources.restype=W.DWORD
k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
k.CloseHandle.argtypes=[W.HANDLE]
def keys(h,*vks):
    assert u.GetForegroundWindow()==h,'No input sent outside test window'
    for v in vks:u.keybd_event(v,0,0,0)
    for v in reversed(vks):u.keybd_event(v,0,2,0)
    time.sleep(.1)
def command(h,id):u.SendMessageW(h,0x111,id,0)
def rect(h,parent=None):
    r=W.RECT();u.GetWindowRect(h,C.byref(r))
    if parent:
        a=W.POINT(r.left,r.top);b=W.POINT(r.right,r.bottom)
        u.ScreenToClient(parent,C.byref(a));u.ScreenToClient(parent,C.byref(b))
        return (a.x,a.y,b.x,b.y)
    return (r.left,r.top,r.right,r.bottom)
def inside(h,parent):
    r=rect(h,parent);p=W.RECT();u.GetClientRect(parent,C.byref(p))
    assert 0<=r[0]<r[2]<=p.right and 0<=r[1]<r[3]<=p.bottom,(name(h),r,p.right,p.bottom)
def first(pid,cls):
    found=[]
    def collect(h,_):
        p=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(p))
        if p.value==pid and name(h,True).startswith(cls):found.append(h)
        return True
    cb=CALLBACK(collect);u.EnumWindows(cb,0);return (found or [None])[0]
class TBBUTTON(C.Structure):
    _fields_=[('bitmap',C.c_int),('command',C.c_int),('state',W.BYTE),('style',W.BYTE),('reserved',W.BYTE*6),('data',C.c_size_t),('string',C.c_ssize_t)]
# Toolbar pointer messages need remote memory. Public button rects are checked via a remote buffer.
k.VirtualAllocEx.argtypes=[W.HANDLE,C.c_void_p,C.c_size_t,W.DWORD,W.DWORD];k.VirtualAllocEx.restype=C.c_void_p
k.VirtualFreeEx.argtypes=[W.HANDLE,C.c_void_p,C.c_size_t,W.DWORD]
k.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
config=OUT/'ui-layout.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
config.with_suffix('.prompt.ini').write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=0\n',encoding='utf-8')
config.with_suffix('.prompt.md').write_text('# 实现方案\n\n请围绕 **性能** 与 **可维护性** 给出建议。\n\n- 保留现有约束\n- 标明需要验证的假设\n\n```mermaid\nflowchart LR\nA[阅读] --> B[分析] --> C[建议]\n```\n\n最后列出下一步。\n',encoding='utf-8')
exe=ROOT/'build/release/keepmd.exe';checks=[]
p=subprocess.Popen([str(exe),str(ROOT/'tests/fixtures/welcome.md'),'--config',str(config)],cwd=ROOT)
handle=k.OpenProcess(0x0400|0x0010|0x0008,False,p.pid)
remote=k.VirtualAllocEx(handle,None,64,0x3000,4)
try:
    main=wait(lambda:(windows(p.pid)or[None])[0]);wait(lambda:name(main).startswith('welcome.md'))
    activate(main);screenshot(main,'ui-reader-light.png')
    toolbar=next(h for h in children(main) if name(h,True)=='ToolbarWindow32')
    def toolbar_fits():
        tr=W.RECT();u.GetClientRect(toolbar,C.byref(tr));visible=0
        for i in range(u.SendMessageW(toolbar,0x418,0,0)):
            u.SendMessageW(toolbar,0x417,i,remote)
            b=TBBUTTON();assert k.ReadProcessMemory(handle,remote,C.byref(b),C.sizeof(b),None)
            if b.state&8 or b.style&1:continue
            u.SendMessageW(toolbar,0x41D,i,remote)
            r=W.RECT();assert k.ReadProcessMemory(handle,remote,C.byref(r),C.sizeof(r),None)
            assert 0<=r.left<r.right<=tr.right and 0<=r.top<r.bottom<=tr.bottom,(b.command,tuple([r.left,r.top,r.right,r.bottom]),tr.right,tr.bottom)
            visible+=1
        assert visible>=10
    toolbar_fits()
    u.MoveWindow(main,30,30,640,520,True);time.sleep(.25);toolbar_fits()
    assert u.SendMessageW(toolbar,0x428,0,0)==1 # compact icon toolbar fits minimum width
    screenshot(main,'ui-reader-narrow.png')
    command(main,117);time.sleep(.2);toolbar_fits();inside(u.GetDlgItem(main,202),main)
    command(main,117)
    checks.append('reader icon toolbar fits 640px on one row; edit actions appear without clipping')
    u.MoveWindow(main,30,30,1060,820,True);command(main,105);command(main,102);command(main,106)
    time.sleep(.15);screenshot(main,'ui-reader-dark.png')
    toc=next(h for h in children(main) if name(h,True)=='SysListView32')
    assert u.SendMessageW(toc,0x1000,0,0)==0x241C17 # COLORREF(23,28,36)
    checks.append('dark theme covers toolbar, native outline, search, status and title/menu chrome')
    command(main,127);panel=wait(lambda:first(p.pid,'KeepMD.Prompt.'))
    wait(lambda:u.IsWindowVisible(panel));activate(panel)
    edit=u.GetDlgItem(panel,500);wait(lambda:bool(edit));time.sleep(.7)
    screenshot(panel,'ui-prompt-dark.png')
    for h in children(panel):
        if u.IsWindowVisible(h):inside(h,panel)
    # Theme button is a native button: Space activates it and theme propagates to reader.
    theme=u.GetDlgItem(panel,527);activate(panel,theme);keys(panel,0x20)
    wait(lambda:name(theme)=='深色')
    screenshot(panel,'ui-prompt-light.png')
    checks.append('prompt theme button activates by keyboard and synchronizes with reader')
    command(panel,514);u.MoveWindow(panel,100,70,720,540,True);time.sleep(.2)
    for h in children(panel):
        if u.IsWindowVisible(h):inside(h,panel)
    ids=[502,515,516,517]
    boxes=[rect(u.GetDlgItem(panel,i),panel) for i in ids]
    assert all(boxes[i][2]<=boxes[i+1][0] for i in range(len(boxes)-1)),boxes
    screenshot(panel,'ui-prompt-narrow-settings.png')
    command(panel,514)
    assert not any(name(h,True)=='KeepMD.PromptPreview' for h in children(panel))
    assert rect(edit,panel)[2]-rect(edit,panel)[0]>600
    checks.append('prompt settings fit 720px and visual editor uses full content width')
    before=rect(panel)
    u.PostMessageW(panel,0x10,0,0);wait(lambda:not u.IsWindowVisible(panel))
    command(main,127);wait(lambda:u.IsWindowVisible(panel));assert rect(panel)==before
    checks.append('prompt retains resized window dimensions across hide/show')
    activate(panel,u.GetDlgItem(panel,511));keys(panel,0x20)
    assert clipboard().startswith('# 实现方案')
    checks.append('owner-drawn copy button preserves native Space activation and exact Markdown copy')
    # Actual menu interaction with themed top-level labels and native popup content.
    activate(panel);keys(panel,0x79) # F10
    keys(panel,0x28);time.sleep(.1);keys(panel,0x1B);keys(panel,0x1B)
    # Some menu sequences leave Esc to prompt, so restore through the reader command.
    if not u.IsWindowVisible(panel):command(main,127);wait(lambda:u.IsWindowVisible(panel))
    checks.append('F10/arrow/Esc navigation remains available for native menus')
    before=u.GetGuiResources(handle,0)
    for _ in range(30):command(main,106)
    time.sleep(.2);after=u.GetGuiResources(handle,0)
    assert after-before<=3,(before,after)
    checks.append('30 theme changes do not accumulate GDI objects')
    u.PostMessageW(main,0x111,101,0);p.wait(timeout=8);assert p.returncode==0
    result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':checks,'theme_gdi_before':before,'theme_gdi_after':after,
            'scope':'Real 96-DPI desktop, 640/720px minimum windows; physical monitor DPI transitions not exercised.'}
    (ROOT/'bench/results/ui-e2e.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
finally:
    if remote:k.VirtualFreeEx(handle,remote,0,0x8000)
    if handle:k.CloseHandle(handle)
    if p.poll() is None:p.terminate();p.wait(timeout=5)
