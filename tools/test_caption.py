"""Native borderless caption: actual drag/resize, maximize work area, window/menu buttons and close guards."""
import ctypes as C
from ctypes import wintypes as W
import hashlib,json,subprocess,time
from test_gui import ROOT,OUT,u,k,children,name,wait,activate,click,screenshot,CALLBACK,windows
u.GetWindowLongPtrW.argtypes=[W.HWND,C.c_int];u.GetWindowLongPtrW.restype=C.c_ssize_t
u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetMenu.argtypes=[W.HWND];u.GetMenu.restype=W.HMENU
u.IsZoomed.argtypes=[W.HWND];u.IsIconic.argtypes=[W.HWND]
u.ShowWindow.argtypes=[W.HWND,C.c_int]
u.GetClientRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
u.MonitorFromWindow.argtypes=[W.HWND,W.DWORD];u.MonitorFromWindow.restype=W.HANDLE
u.keybd_event.argtypes=[W.BYTE,W.BYTE,W.DWORD,C.c_size_t]
class MONITOR(C.Structure):_fields_=[('size',W.DWORD),('monitor',W.RECT),('work',W.RECT),('flags',W.DWORD)]
u.GetMonitorInfoW.argtypes=[W.HANDLE,C.POINTER(MONITOR)]
def rect(h):
    r=W.RECT();u.GetWindowRect(h,C.byref(r));return r.left,r.top,r.right,r.bottom
def first(pid,cls):
    result=[]
    def collect(h,_):
        p=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(p))
        if p.value==pid and name(h,True)==cls and u.IsWindowVisible(h):result.append(h)
        return True
    cb=CALLBACK(collect);u.EnumWindows(cb,0);return (result or [None])[0]
def keys(root,*vks):
    assert u.GetForegroundWindow()==root,'Focus left test window; no input sent'
    for v in vks:u.keybd_event(v,0,0,0)
    for v in reversed(vks):u.keybd_event(v,0,2,0)
    time.sleep(.12)
def drag(root,x,y,dx,dy):
    activate(root);u.SetCursorPos(x,y);time.sleep(.05)
    u.mouse_event(2,0,0,0,0);time.sleep(.08)
    for i in range(1,9):u.SetCursorPos(x+dx*i//8,y+dy*i//8);time.sleep(.025)
    u.mouse_event(4,0,0,0,0);time.sleep(.2)
def press(root,caption,id):
    h=u.GetDlgItem(caption,id);assert h
    r=W.RECT();u.GetClientRect(h,C.byref(r));click(h,r.right//2,r.bottom//2);time.sleep(.1)
exe=ROOT/'build/release/keepmd.exe';config=OUT/'caption.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
config.with_suffix('.prompt.ini').write_text('[Prompt]\nResident=0\nHotkey=Ctrl+Alt+Space\n',encoding='utf-8')
p=subprocess.Popen([str(exe),str(ROOT/'tests/fixtures/welcome.md'),'--config',str(config)],cwd=ROOT)
checks=[]
try:
    main=wait(lambda:(windows(p.pid)or[None])[0]);wait(lambda:'welcome.md' in name(main))
    caption=next(h for h in children(main) if name(h,True)=='KeepMD.Caption')
    assert u.GetWindowLongPtrW(main,-16)&0x00C00000==0 and not u.GetMenu(main)
    activate(main);screenshot(main,'caption-reader-light.png')
    before=rect(main);r=rect(caption)
    drag(main,r[0]+500,r[1]+22,50,35)
    moved=rect(main);assert abs(moved[0]-before[0]-50)<4 and abs(moved[1]-before[1]-35)<4,(before,moved)
    checks.append('native caption style and menu strip removed; custom header drag moves reader')
    # Real right-edge sizing rather than an artificial WM_SIZE.
    r=rect(main);drag(main,r[2]-2,(r[1]+r[3])//2,75,0)
    after=rect(main);assert after[2]-after[0]>=r[2]-r[0]+65,(r,after)
    checks.append('right edge retains native interactive resizing')
    press(main,caption,9102);wait(lambda:u.IsZoomed(main))
    monitor=MONITOR();monitor.size=C.sizeof(monitor);u.GetMonitorInfoW(u.MonitorFromWindow(main,2),C.byref(monitor))
    client=W.RECT();u.GetClientRect(main,C.byref(client));origin=W.POINT(0,0);u.ClientToScreen(main,C.byref(origin))
    assert (origin.x,origin.y,origin.x+client.right,origin.y+client.bottom)==(monitor.work.left,monitor.work.top,monitor.work.right,monitor.work.bottom)
    press(main,caption,9102);wait(lambda:not u.IsZoomed(main))
    checks.append('custom maximize/restore buttons respect monitor work area and leave taskbar uncovered')
    # Actual double click on header toggles maximize.
    r=rect(caption);activate(main);u.SetCursorPos(r[0]+500,r[1]+22)
    for _ in range(2):u.mouse_event(2,0,0,0,0);time.sleep(.03);u.mouse_event(4,0,0,0,0);time.sleep(.07)
    wait(lambda:u.IsZoomed(main));press(main,caption,9102);wait(lambda:not u.IsZoomed(main))
    press(main,caption,9101);wait(lambda:u.IsIconic(main));u.ShowWindow(main,9);activate(main)
    checks.append('double-click maximize and custom minimize work')
    # OS docking still sees a resizable/maximizable native top-level window.
    before=rect(main);activate(main);keys(main,0x5B,0x25)
    wait(lambda:rect(main)!=before)
    snap=rect(main);assert snap[2]-snap[0]<monitor.work.right-monitor.work.left
    keys(main,0x5B,0x27);time.sleep(.2)
    u.ShowWindow(main,9);activate(main)
    checks.append('Win+Left docking remains available with the custom frame')
    # Opening file menu uses real buttons and original native popup commands.
    press(main,caption,9200);wait(lambda:first(p.pid,'#32768'))
    keys(main,0x1B)
    activate(main);keys(main,0x79);keys(main,0x28);wait(lambda:first(p.pid,'#32768'));keys(main,0x1B);keys(main,0x1B)
    activate(main);keys(main,0x12,0x20);wait(lambda:first(p.pid,'#32768'));keys(main,0x1B)
    checks.append('custom menu button, F10 and Alt+Space open native menus')
    # Click actual prompt menu command in new header.
    press(main,caption,9202)
    def prompt():
        values=[]
        def f(h,_):
            pid=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(pid))
            if pid.value==p.pid and name(h,True).startswith('KeepMD.Prompt.') and u.IsWindowVisible(h):values.append(h)
            return True
        cb=CALLBACK(f);u.EnumWindows(cb,0);return (values or [None])[0]
    panel=wait(prompt);bar=next(h for h in children(panel) if name(h,True)=='KeepMD.Caption')
    assert u.GetWindowLongPtrW(panel,-16)&0x00C00000==0 and not u.GetMenu(panel)
    u.SendMessageW(panel,0x111,527,0);time.sleep(.15);screenshot(panel,'caption-prompt-dark.png')
    press(panel,bar,9103);wait(lambda:not u.IsWindowVisible(panel));assert p.poll() is None
    press(main,caption,9202);wait(prompt);activate(panel);keys(panel,0x12,0x73);wait(lambda:not u.IsWindowVisible(panel))
    checks.append('prompt custom close and Alt+F4 preserve save-and-hide semantics')
    # Unsaved editing must still protect data through the replacement close button.
    activate(main);u.SendMessageW(main,0x111,117,0);edit=wait(lambda:u.GetDlgItem(main,202))
    u.SendMessageW(edit,0xB1,-1,-1);value=C.create_unicode_buffer('\nunsaved caption test')
    u.SendMessageW(edit,0xC2,1,C.cast(value,C.c_void_p).value)
    press(main,caption,9103);box=wait(lambda:first(p.pid,'#32770'));u.PostMessageW(box,0x111,2,0)
    wait(lambda:not u.IsWindowVisible(box));assert p.poll() is None and u.IsWindowVisible(main)
    u.SendMessageW(edit,0xB9,0,0) # clear modify bit; generated input is discarded without saving fixture
    press(main,caption,9103);p.wait(timeout=8);assert p.returncode==0
    checks.append('reader custom close keeps unsaved-change cancel protection and normal exit behavior')
    result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':checks,'scope':'real mouse/keyboard on Windows 11 at 96 DPI; physical multi-monitor changes not exercised'}
    (ROOT/'bench/results/caption-e2e.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result,indent=2))
finally:
    if p.poll() is None:p.terminate();p.wait(timeout=5)
