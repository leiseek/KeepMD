"""Real icon toolbar checks: hover explanations, keyboard/visual editing and bounded GDI use."""
import ctypes as C
from ctypes import wintypes as W
import hashlib,json,subprocess,time
from test_gui import ROOT,OUT,u,k,children,name,wait,activate,click,screenshot,clipboard,CALLBACK,windows
u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetClientRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
u.keybd_event.argtypes=[W.BYTE,W.BYTE,W.DWORD,C.c_size_t]
u.GetGuiResources.argtypes=[W.HANDLE,W.DWORD];u.GetGuiResources.restype=W.DWORD
u.InvalidateRect.argtypes=[W.HWND,C.c_void_p,W.BOOL]
u.UpdateWindow.argtypes=[W.HWND]
k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
k.CloseHandle.argtypes=[W.HANDLE]
def first(pid,cls,visible=True):
    found=[]
    def collect(h,_):
        p=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(p))
        if p.value==pid and name(h,True).startswith(cls) and (not visible or u.IsWindowVisible(h)):found.append(h)
        return True
    cb=CALLBACK(collect);u.EnumWindows(cb,0);return (found or [None])[0]
def tip(pid,expected):
    h=first(pid,'tooltips_class32');return h if h and expected in name(h) else None
def hover(root,h,x=None,y=None):
    activate(root);r=W.RECT();u.GetClientRect(h,C.byref(r))
    pt=W.POINT(r.right//2 if x is None else x,r.bottom//2 if y is None else y)
    u.ClientToScreen(h,C.byref(pt));u.SetCursorPos(pt.x,pt.y)
def keys(h,*vks):
    assert u.GetForegroundWindow()==h
    for v in vks:u.keybd_event(v,0,0,0)
    for v in reversed(vks):u.keybd_event(v,0,2,0)
    time.sleep(.1)
def command(h,id):u.SendMessageW(h,0x111,id,0)
config=OUT/'icons.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
config.with_suffix('.prompt.ini').write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=0\n',encoding='utf-8')
sample='# 图标工具栏\n\n在这里直接编辑 **提示词**。\n'
config.with_suffix('.prompt.md').write_text(sample,encoding='utf-8')
exe=ROOT/'build/release/keepmd.exe';p=subprocess.Popen([str(exe),str(ROOT/'tests/fixtures/welcome.md'),'--config',str(config)],cwd=ROOT)
handle=k.OpenProcess(0x0400,False,p.pid);checks=[]
try:
    main=wait(lambda:(windows(p.pid)or[None])[0]);wait(lambda:'welcome.md' in name(main))
    toolbar=next(h for h in children(main) if name(h,True)=='ToolbarWindow32')
    hover(main,toolbar,18,18);tooltip=wait(lambda:tip(p.pid,'打开 Markdown'),timeout=5)
    assert 'Ctrl+O' in name(tooltip)
    screenshot(main,'icons-reader-tooltip.png')
    checks.append('reader folder icon exposes native hover explanation and Ctrl+O shortcut')
    command(main,126);panel=wait(lambda:first(p.pid,'KeepMD.Prompt.'));edit=wait(lambda:u.GetDlgItem(panel,500));time.sleep(.4)
    # Every icon keeps a meaningful native control name despite hiding its label visually.
    for id,label in [(540,'正文'),(541,'标题 1'),(542,'标题 2'),(543,'加粗'),(544,'斜体'),(545,'列表'),
                     (546,'编号'),(547,'引用'),(548,'代码'),(549,'流程图'),(514,'快捷键'),(512,'清空'),(511,'复制全文')]:
        assert name(u.GetDlgItem(panel,id))==label
    assert '复制并收起' in name(u.GetDlgItem(panel,510))
    checks.append('format and utility icons retain descriptive native names; primary copy action retains visible text')
    bold=u.GetDlgItem(panel,543);hover(panel,bold);tooltip=wait(lambda:tip(p.pid,'加粗'),timeout=5)
    assert 'Ctrl+B' in name(tooltip)
    screenshot(panel,'icons-prompt-tooltip.png')
    checks.append('prompt bold icon exposes Chinese tooltip and Ctrl+B on actual mouse hover')
    # Real icon click applies the existing Markdown command, without changing selection first.
    offset=sample.index('直接');u.SendMessageW(edit,0xB1,offset,offset+2)
    click(bold,18,18)
    def source():
        for _ in range(20):
            command(panel,511);value=clipboard()
            if value:return value
            time.sleep(.025)
        return ''
    result=source();assert '**直接**' in result,repr(result)
    activate(panel,edit);keys(panel,0x11,ord('Z'));assert source()==sample,repr(source())
    activate(panel,u.GetDlgItem(panel,511));keys(panel,0x20);assert clipboard()==sample,repr(clipboard())
    checks.append('icon click executes formatting and undo; icon-only copy still activates with Space')
    # Check hot/pressed and themes without leaking per-paint supersampling bitmaps.
    command(panel,527);time.sleep(.1);command(panel,527);time.sleep(.1)
    before=u.GetGuiResources(handle,0)
    for _ in range(60):
        for id in (540,543,545,549,512,511):
            h=u.GetDlgItem(panel,id);u.InvalidateRect(h,None,False);u.UpdateWindow(h)
        command(panel,527)
    time.sleep(.15);after=u.GetGuiResources(handle,0)
    assert after-before<=3,(before,after)
    checks.append('360 icon repaints and 60 theme changes do not accumulate GDI objects')
    command(panel,525);p.wait(timeout=8);assert p.returncode==0
    result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':checks,'gdi_before':before,'gdi_after':after,
            'implementation':'native GDI vector paths, temporary supersampling bitmap; no icon font or third-party UI runtime'}
    (ROOT/'bench/results/icons-e2e.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
finally:
    if handle:k.CloseHandle(handle)
    if p.poll() is None:p.terminate();p.wait(timeout=5)
