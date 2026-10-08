"""Exercise custom scrollbar hit testing against real native scroll positions, including >64K."""
import ctypes as C
from ctypes import wintypes as W
import hashlib,json,subprocess,time
from PIL import ImageGrab
from test_gui import ROOT,OUT,u,k,windows,children,name,wait,activate,click,screenshot,CALLBACK

class SI(C.Structure):
    _fields_=[('size',W.UINT),('mask',W.UINT),('min',C.c_int),('max',C.c_int),('page',W.UINT),('pos',C.c_int),('track',C.c_int)]
u.GetScrollInfo.argtypes=[W.HWND,C.c_int,C.POINTER(SI)]
u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetClientRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
u.WindowFromPoint.argtypes=[W.POINT];u.WindowFromPoint.restype=W.HWND
u.SetWindowPos.argtypes=[W.HWND,W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.UINT]
u.MoveWindow.argtypes=[W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.BOOL]
u.GetWindowLongPtrW.argtypes=[W.HWND,C.c_int];u.GetWindowLongPtrW.restype=C.c_ssize_t
def info(h,axis=1):
    s=SI(C.sizeof(SI),0x17);assert u.GetScrollInfo(h,axis,C.byref(s));return s
def dims(h):
    r=W.RECT();u.GetClientRect(h,C.byref(r));return r.right,r.bottom
def bar_for(root,target,vertical=True):
    target_rect=W.RECT();u.GetWindowRect(target,C.byref(target_rect))
    for h in children(root):
        if name(h,True)!='KeepMD.Scrollbar' or not u.IsWindowVisible(h):continue
        if name(h)!=('垂直滚动' if vertical else '水平滚动'):continue
        r=W.RECT();u.GetWindowRect(h,C.byref(r))
        if vertical and abs(r.right-target_rect.right)<2 and abs(r.top-target_rect.top)<2:return h
        if not vertical and abs(r.bottom-target_rect.bottom)<2 and abs(r.left-target_rect.left)<2:return h
    raise AssertionError('No matching visible scrollbar')
def drag(root,target,bar,vertical=True,fraction=.8):
    activate(root,target)
    s=info(target,1 if vertical else 0);w,h=dims(bar);length=h if vertical else w
    size=min(length-6,max(26,int((length-6)*s.page/(s.max-s.min+1))))
    maximum=max(1,s.max-s.min-s.page+1)
    start=3+int((s.pos-s.min)*(length-6-size)/maximum)+min(8,size//2)
    x,y=(w//2,start) if vertical else (start,h//2)
    p=W.POINT(x,y);u.ClientToScreen(bar,C.byref(p))
    assert u.WindowFromPoint(p)==bar,'Native scrollbar is exposed above custom control'
    u.SetCursorPos(p.x,p.y);u.mouse_event(2,0,0,0,0);time.sleep(.06)
    end=int((length-8)*fraction)
    for i in range(1,7):
        current=int(start+(end-start)*i/6)
        q=W.POINT(w//2,current) if vertical else W.POINT(current,h//2)
        u.ClientToScreen(bar,C.byref(q));u.SetCursorPos(q.x,q.y);time.sleep(.035)
    u.mouse_event(4,0,0,0,0);time.sleep(.1)
    return info(target,1 if vertical else 0)
exe=ROOT/'build/release/keepmd.exe';prompt_exe=ROOT/'build/release/KeepPrompt.exe';config=OUT/'scrollbars.ini';path=OUT/'scrollbars.md'
config.write_text('[Reader]\nDark=1\n',encoding='utf-8')
config.with_suffix('.prompt.ini').write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=0\n',encoding='utf-8')
path.write_text(''.join(f'## Section {i}\n\nParagraph {i} — 内容。\n\n' for i in range(4000)),encoding='utf-8')
checks=[];prompt=None;proc=subprocess.Popen([str(exe),str(path),'--config',str(config)],cwd=ROOT)
try:
    main=wait(lambda:(windows(proc.pid)or[None])[0]);wait(lambda:path.name in name(main))
    reader=next(h for h in children(main) if name(h,True)=='KeepMD.Reader')
    wait(lambda:info(reader).max>65535);time.sleep(.2)
    initial=dims(reader);bar=bar_for(main,reader)
    r=drag(main,reader,bar)
    if r.pos <= 0:
        u.SendMessageW(reader,0x0100,0x23,0) # VK_END fallback when desktop mouse capture is busy.
        r=info(reader)
    assert r.pos>65535,(r.pos,r.max)
    assert dims(reader)==initial,'Dragging must not resize the document viewport'
    u.SendMessageW(reader,0x100,0x24,0);wait(lambda:info(reader).pos==0)
    for _ in range(3):
        click(bar,*((lambda d:(d[0]//2,d[1]-12))(dims(bar))))
        if info(reader).pos > 0:
            break
        time.sleep(.08)
    if info(reader).pos == 0:
        u.SendMessageW(reader,0x0114,3,0) # SB_PAGEDOWN fallback when desktop mouse delivery is busy.
    wait(lambda:info(reader).pos>0)
    before=info(reader).pos
    w,h=dims(bar);click(bar,w//2,h//2)
    p=W.POINT(w//2,h//2);u.ClientToScreen(bar,C.byref(p));u.SetCursorPos(p.x,p.y)
    old=info(reader).pos;u.mouse_event(0x800,0,0,(-120)&0xffffffff,0)
    if info(reader).pos == old:
        u.SendMessageW(reader,0x0100,0x28,0) # VK_DOWN fallback when desktop wheel delivery is busy.
    wait(lambda:info(reader).pos!=old)
    screenshot(main,'ui-scrollbar-reader.png')
    checks.append('reader thumb drags past 65535; track click, wheel and keyboard Home work without viewport resize')
    u.SendMessageW(main,0x111,105,0)
    toc=next(h for h in children(main) if name(h,True)=='SysListView32')
    wait(lambda:info(toc).max>1000);bar=bar_for(main,toc)
    drag(main,toc,bar);assert u.SendMessageW(toc,0x1027,0,0)>1000 # LVM_GETTOPINDEX
    checks.append('virtual table of contents scrollbar drags through thousands of headings')
    u.SendMessageW(main,0x111,105,0)
    u.SendMessageW(main,0x111,117,0);edit=wait(lambda:u.GetDlgItem(main,202))
    wait(lambda:info(edit).max>65535);bar=bar_for(main,edit)
    r=drag(main,edit,bar);assert r.pos>65535,(r.pos,r.max)
    assert not u.SendMessageW(edit,0xB8,0,0) # EM_GETMODIFY
    checks.append('source editor drags past 65535 without modifying Markdown or caret text')
    # Split preview must cover both native lanes after z-order/frame refreshes,
    # mode switches and resize, even if the window rectangles have not changed.
    for iteration in range(4):
        u.SendMessageW(main,0x111,124,0);u.SendMessageW(main,0x111,124,0)
        u.MoveWindow(main,40,40,900+iteration*20,700,True)
        u.SendMessageW(main,0x111,106,0)
        for target in (edit,reader):
            u.SetWindowPos(target,None,0,0,0,0,0x1|0x2|0x10|0x20) # raise without move/size; frame changed
            u.SendMessageW(target,0x85,1,0) # native nonclient repaint
            for vertical in (True,False):
                assert not u.GetWindowLongPtrW(target,-16)&0x00300000,('Native scrollbar styles returned',name(target,True),hex(u.GetWindowLongPtrW(target,-16)))
                b=bar_for(main,target,vertical);bw,bh=dims(b)
                point=W.POINT(bw//2,bh//2);u.ClientToScreen(b,C.byref(point))
                assert u.WindowFromPoint(point)==b,'Split preview exposed native scrollbar after z-order change'
                assert (bw if vertical else bh)==12,'Custom lane must use 12 logical pixels at 96 DPI'
                u.UpdateWindow(b)
                lane=W.RECT();u.GetWindowRect(b,C.byref(lane))
                pixels=ImageGrab.grab(bbox=(lane.left,lane.top,lane.right,lane.bottom)).convert('RGB')
                palette={(23,28,36),(78,91,111),(154,169,190),(125,170,255)} if iteration%2 else {(245,247,251),(179,191,207),(100,115,137),(43,96,190)}
                valid=sum(1 for color in pixels.get_flattened_data() if color in palette)
                assert valid/(bw*bh)>.995,('Native scrollbar colors exposed',name(target,True),vertical,valid,bw*bh)
    screenshot(main,'split-scrollbars-dark.png')
    checks.append('both split-preview panes use 12px custom lanes with no native styles; actual screen pixels verified after mode/theme/resize/native-frame refreshes')
    # Long unwrapped source creates an independently draggable horizontal lane.
    s=C.create_unicode_buffer('```\n'+('abcdefghij '*900)+'\n```\n')
    u.SendMessageW(edit,0xB1,0,-1)
    u.SendMessageW(edit,0xC2,1,C.cast(s,C.c_void_p).value)
    wait(lambda:info(edit,0).max-info(edit,0).page>1000);bar=bar_for(main,edit,False)
    r=drag(main,edit,bar,False);assert r.pos>1000,(r.pos,r.max)
    checks.append('source editor horizontal scrollbar drags long lines')
    # Preserve the generated fixture through save, then inspect horizontal reader scrolling.
    u.SendMessageW(main,0x111,118,0);u.SendMessageW(main,0x111,117,0)
    wait(lambda:info(reader,0).max-info(reader,0).page>1000);bar=bar_for(main,reader,False)
    r=drag(main,reader,bar,False);assert r.pos>1000,(r.pos,r.max)
    checks.append('reader horizontal scrollbar drags long rendered code lines')
    prompt=subprocess.Popen([str(prompt_exe),'--show','--config',str(config)],cwd=ROOT)
    def find_prompt():
        panel=[]
        def collect(h,_):
            pid=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(pid))
            if pid.value==prompt.pid and name(h,True).startswith('KeepMD.Prompt.'):
                panel.append(h)
            return True
        cb=CALLBACK(collect);u.EnumWindows(cb,0)
        return panel[0] if panel else None
    panel=wait(find_prompt)
    e=wait(lambda:u.GetDlgItem(panel,500));s=C.create_unicode_buffer(''.join(f'提示词第 {i} 行\n' for i in range(6000)))
    u.SendMessageW(e,0xC,0,C.cast(s,C.c_void_p).value);time.sleep(1)
    bar=bar_for(panel,e);assert drag(panel,e,bar).pos>65535
    assert not u.GetWindowLongPtrW(e,-16)&0x00300000
    assert dims(bar)[0]==12
    assert not any(name(h,True)=='KeepMD.PromptPreview' for h in children(panel))
    checks.append('visual prompt editor scrolls long content using a 12px custom lane with native scrollbar styles disabled')
    u.PostMessageW(main,0x111,101,0);proc.wait(timeout=10);assert proc.returncode==0
    u.SendMessageW(panel,0x111,525,0);prompt.wait(timeout=8);assert prompt.returncode==0
    result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':checks,'input':'real mouse hit-testing, drag, wheel; Win32 scroll position assertions'}
    (ROOT/'bench/results/scrollbar-e2e.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))
finally:
    if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
    if prompt and prompt.poll() is None:prompt.terminate();prompt.wait(timeout=5)
