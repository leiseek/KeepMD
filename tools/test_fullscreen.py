"""Full screen reading: real F11/Esc, monitor coverage, hidden chrome, presenter paging, idle pointer, restore."""
import ctypes as C
from ctypes import wintypes as W
import hashlib,json,subprocess,time
from test_gui import ROOT,OUT,u,children,name,wait,activate,windows
u.IsZoomed.argtypes=[W.HWND];u.ShowWindow.argtypes=[W.HWND,C.c_int]
u.GetClientRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
u.MonitorFromWindow.argtypes=[W.HWND,W.DWORD];u.MonitorFromWindow.restype=W.HANDLE
u.keybd_event.argtypes=[W.BYTE,W.BYTE,W.DWORD,C.c_size_t]
u.GetDpiForWindow.argtypes=[W.HWND]
class MONITOR(C.Structure):_fields_=[('size',W.DWORD),('monitor',W.RECT),('work',W.RECT),('flags',W.DWORD)]
class CURSOR(C.Structure):_fields_=[('size',W.DWORD),('flags',W.DWORD),('cursor',W.HANDLE),('point',W.POINT)]
u.GetMonitorInfoW.argtypes=[W.HANDLE,C.POINTER(MONITOR)];u.GetCursorInfo.argtypes=[C.POINTER(CURSOR)]
VK_SHIFT,VK_ESCAPE,VK_SPACE,VK_NEXT,VK_PRIOR,VK_END,VK_HOME,VK_LEFT,VK_RIGHT,VK_F6,VK_F11=16,27,32,34,33,35,36,37,39,117,122
def rect(h):
    r=W.RECT();u.GetWindowRect(h,C.byref(r));return r.left,r.top,r.right,r.bottom
def monitor(h):
    m=MONITOR();m.size=C.sizeof(m);u.GetMonitorInfoW(u.MonitorFromWindow(h,2),C.byref(m))
    return m.monitor.left,m.monitor.top,m.monitor.right,m.monitor.bottom
def keys(root,*vks):
    assert u.GetForegroundWindow()==root,'Focus left test window; no input sent'
    for v in vks:u.keybd_event(v,0,0,0)
    for v in reversed(vks):u.keybd_event(v,0,2,0)
    time.sleep(.15)
def cursor_shown():
    c=CURSOR();c.size=C.sizeof(c);u.GetCursorInfo(C.byref(c));return bool(c.cursor)
exe=ROOT/'build/release/keepmd.exe';report=OUT/'fullscreen.json';config=OUT/'fullscreen.ini'
config.write_text('[Reader]\nVisual=0\nDark=0\n',encoding='utf-8')
doc=OUT/'fullscreen.md'
doc.write_text(''.join(f'## 第 {i} 节\n\n'+'全屏阅读按页前进与后退，指针静止后隐藏。'*6+'\n\n' for i in range(1,80)),encoding='utf-8',newline='')
p=subprocess.Popen([str(exe),str(doc),'--config',str(config),'--report',str(report)],cwd=ROOT)
checks=[]
def state():
    u.SendMessageW(main,0x8000+100,0,0);return json.loads(report.read_text())
try:
    main=wait(lambda:(windows(p.pid)or[None])[0])
    status=wait(lambda:next((h for h in children(main) if name(h,True)=='msctls_statusbar32'),None))
    wait(lambda:name(status).startswith('阅读'))
    reader=next(h for h in children(main) if name(h,True)=='KeepMD.Reader')
    caption=next(h for h in children(main) if name(h,True)=='Keep.Caption')
    toolbar=next(h for h in children(main) if name(h,True)=='ToolbarWindow32')
    windowed=rect(main)
    activate(main,reader);keys(main,VK_F11)
    wait(lambda:rect(main)==monitor(main))
    client=W.RECT();u.GetClientRect(main,C.byref(client))
    assert not any(u.IsWindowVisible(h) for h in (caption,toolbar,status))
    assert rect(reader)==rect(main),(rect(reader),rect(main),client.right,client.bottom)
    assert state()['fullscreen'] is True
    checks.append({'case':'F11 covers the monitor; caption, toolbar, status bar and page card hidden','window':rect(main)})
    activate(main,reader);dpi=u.GetDpiForWindow(main)/96
    # Every presenter key moves exactly as far as the existing PageDown: 90% of the view, whose height
    # excludes the horizontal scrollbar track.
    keys(main,VK_NEXT);page=state()['scroll_y'];keys(main,VK_HOME)
    visible=(rect(reader)[3]-rect(reader)[1])/dpi
    assert .85*visible<=page<=.92*visible,(page,visible)
    steps=[]
    for label,combo,sign in [('Space',(VK_SPACE,),1),('Shift+Space',(VK_SHIFT,VK_SPACE),-1),('Right',(VK_RIGHT,),1),
                             ('Left',(VK_LEFT,),-1),('PageDown',(VK_NEXT,),1),('PageUp',(VK_PRIOR,),-1)]:
        before=state()['scroll_y'];keys(main,*combo);after=state()['scroll_y']
        assert abs((after-before)-sign*page)<=1,(label,before,after,page)
        steps.append([label,round(after-before,1)])
    keys(main,VK_END);bottom=state()['scroll_y'];assert bottom>page*5,bottom
    keys(main,VK_HOME);assert state()['scroll_y']==0
    checks.append({'case':'presenter keys page by 90% of the full-screen view; Home/End reach the ends','page':round(page,1),'steps':steps})
    r=rect(reader);u.SetCursorPos((r[0]+r[2])//2,(r[1]+r[3])//2);time.sleep(.3)
    assert cursor_shown()
    time.sleep(3.2);hidden=not cursor_shown()
    u.SetCursorPos((r[0]+r[2])//2+40,(r[1]+r[3])//2+25);time.sleep(.3)
    assert hidden and cursor_shown(),(hidden,cursor_shown())
    checks.append({'case':'pointer hides after 2.5 s idle over the document and returns on movement'})
    activate(main,reader);u.SendMessageW(main,0x111,102,0)  # find
    search=next(h for h in children(main) if name(h,True)=='Edit' and u.IsWindowVisible(h))
    assert rect(search)[1]<monitor(main)[1]+80 and rect(reader)[1]>rect(search)[3]
    activate(main,search);keys(main,VK_ESCAPE)
    assert not u.IsWindowVisible(search) and state()['fullscreen'] is True and rect(reader)==rect(main)
    activate(main,reader);keys(main,VK_ESCAPE)
    wait(lambda:rect(main)==windowed)
    assert all(u.IsWindowVisible(h) for h in (caption,toolbar,status)) and state()['fullscreen'] is False
    checks.append({'case':'find bar opens inside full screen; first Esc closes it, second Esc restores the window','restored':rect(main)})
    u.ShowWindow(main,3);wait(lambda:u.IsZoomed(main));maximized=rect(main)
    activate(main,reader);keys(main,VK_F11);wait(lambda:rect(main)==monitor(main))
    activate(main,reader);keys(main,VK_F11);wait(lambda:u.IsZoomed(main) and rect(main)==maximized)
    assert u.IsWindowVisible(caption)
    u.ShowWindow(main,9);wait(lambda:not u.IsZoomed(main))
    checks.append({'case':'F11 from a maximized window returns to maximized','maximized':maximized})
    activate(main,reader);keys(main,VK_F6)
    editor=wait(lambda:next((h for h in children(main) if name(h,True).startswith('RICHEDIT') and u.IsWindowVisible(h)),None))
    activate(main,editor);keys(main,VK_F11);wait(lambda:rect(main)==monitor(main))
    activate(main,editor);keys(main,VK_ESCAPE);time.sleep(.3)
    assert rect(main)==monitor(main) and state()['fullscreen'] is True,'Esc in an editor must stay in the editor'
    activate(main,editor);keys(main,VK_F11);wait(lambda:not state()['fullscreen'])
    checks.append({'case':'editor fills full screen; Esc stays with the editor and F11 leaves'})
finally:
    p.terminate();p.wait(timeout=5)
result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':checks}
(ROOT/'bench/results/fullscreen-e2e.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,ensure_ascii=False,indent=2))
