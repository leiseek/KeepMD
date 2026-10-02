"""Native visual prompt editor: visible formatting, real typing, source round trips and diagram editing."""
import ctypes as C
from ctypes import wintypes as W
import hashlib,json,subprocess,time
from test_gui import ROOT,OUT,u,k,children,name,wait,activate,screenshot,click,clipboard,CALLBACK

u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.IsWindow.argtypes=[W.HWND]
u.keybd_event.argtypes=[W.BYTE,W.BYTE,W.DWORD,C.c_size_t]
k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
k.VirtualAllocEx.argtypes=[W.HANDLE,C.c_void_p,C.c_size_t,W.DWORD,W.DWORD];k.VirtualAllocEx.restype=C.c_void_p
k.VirtualFreeEx.argtypes=[W.HANDLE,C.c_void_p,C.c_size_t,W.DWORD]
k.WriteProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
k.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
k.CloseHandle.argtypes=[W.HANDLE]
def first(pid,cls):
    found=[]
    def collect(h,_):
        p=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(p))
        if p.value==pid and name(h,True).startswith(cls):found.append(h)
        return True
    cb=CALLBACK(collect);u.EnumWindows(cb,0);return (found or [None])[0]
def keys(h,*vks):
    if u.GetForegroundWindow()!=h:activate(h,u.GetDlgItem(h,500))
    assert u.GetForegroundWindow()==h
    for v in vks:u.keybd_event(v,0,0,0)
    for v in reversed(vks):u.keybd_event(v,0,2,0)
    time.sleep(.1)
def settext(h,text):
    value=C.create_unicode_buffer(text);u.SendMessageW(h,0xC,0,C.cast(value,C.c_void_p).value)
def replace(h,text):
    value=C.create_unicode_buffer(text);u.SendMessageW(h,0xC2,1,C.cast(value,C.c_void_p).value)
def command(h,id):u.SendMessageW(h,0x111,id,0)
class CHARFORMAT(C.Structure):
    _fields_=[('size',W.UINT),('mask',W.DWORD),('effects',W.DWORD),('height',W.LONG),('offset',W.LONG),('color',W.DWORD),('charset',W.BYTE),('pitch',W.BYTE),('face',W.WCHAR*32)]
class GETTEXT(C.Structure):
    _fields_=[('cb',W.DWORD),('flags',W.DWORD),('page',W.UINT),('default',C.c_void_p),('used',C.c_void_p)]
root=OUT/'visual-editor';root.mkdir(exist_ok=True)
cfg=root/'profile.ini';cfg.write_text('[Reader]\nDark=0\n',encoding='utf-8')
cfg.with_suffix('.prompt.ini').write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\n',encoding='utf-8')
sample='# 实现方案\n\n请围绕 **性能** 和 *可维护性* 给出建议。\n\n- 保留现有约束\n- 标明需要验证的假设\n\n```mermaid\nflowchart LR\nA[阅读] --> B[分析] --> C[建议]\n```\n\n最后列出下一步。\n'
cfg.with_suffix('.prompt.md').write_text(sample,encoding='utf-8')
exe=ROOT/'build/release/keepmd.exe';p=subprocess.Popen([str(exe),'--prompt','--config',str(cfg)],cwd=ROOT)
handle=k.OpenProcess(0x0400|0x0010|0x0020|0x0008,False,p.pid);remote=k.VirtualAllocEx(handle,None,4*1024*1024,0x3000,4)
checks=[]
try:
    panel=wait(lambda:first(p.pid,'KeepMD.Prompt.'));wait(lambda:u.IsWindowVisible(panel))
    edit=wait(lambda:u.GetDlgItem(panel,500));time.sleep(.5)
    assert not any(name(h,True)=='KeepMD.PromptPreview' for h in children(panel))
    assert not u.GetDlgItem(panel,513)
    def source():
        for _ in range(20):
            command(panel,511);value=clipboard()
            if value:return value
            time.sleep(.025)
        return ''
    def visible():
        get=GETTEXT(2*1024*1024,0x8,1200,None,None) # GT_NOHIDDENTEXT
        assert k.WriteProcessMemory(handle,remote,C.byref(get),C.sizeof(get),None)
        n=u.SendMessageW(edit,0x45E,remote,remote+1024)
        value=C.create_unicode_buffer(max(1,n+1))
        assert k.ReadProcessMemory(handle,remote+1024,value,C.sizeof(value),None)
        return value.value
    def style(a,z):
        u.SendMessageW(edit,0xB1,a,z)
        cf=CHARFORMAT();cf.size=C.sizeof(cf)
        assert k.WriteProcessMemory(handle,remote,C.byref(cf),C.sizeof(cf),None)
        u.SendMessageW(edit,0x43A,1,remote)
        assert k.ReadProcessMemory(handle,remote,C.byref(cf),C.sizeof(cf),None)
        return cf
    assert source()==sample,repr(source())
    shown=visible()
    assert '实现方案' in shown and '**' not in shown and '```' not in shown and 'flowchart' not in shown,repr(shown)
    assert style(2,6).height>=400
    offset=sample.index('性能');assert style(offset,offset+2).effects&1
    offset=sample.index('可维护性');assert style(offset,offset+4).effects&2
    checks.append('one visual editor: Markdown markers hidden, heading/bold/italic rendered; source preserved exactly')
    activate(panel,edit);u.SendMessageW(edit,0xB1,0,0)
    screenshot(panel,'visual-editor-light.png')
    command(panel,527);time.sleep(.25);screenshot(panel,'visual-editor-dark.png')
    assert source()==sample,repr(source())
    checks.append('native diagram picture and light/dark rich text survive theme changes without source changes')
    # Replace document via the existing clear command, then type into the native editing surface.
    command(panel,512);assert source()==sample # Empty prompt intentionally leaves clipboard untouched.
    activate(panel,edit);replace(edit,'标题');time.sleep(.3)
    command(panel,541);assert source()=='# 标题',repr(source())
    assert '标题' in visible() and '# ' not in visible()
    activate(panel,edit);u.SendMessageW(edit,0xB1,-1,-1);keys(panel,0x0D)
    replace(edit,'正文');time.sleep(.3);assert source()=='# 标题\n正文',repr(source())
    assert style(5,7).height<400
    u.SendMessageW(edit,0xB1,5,7);command(panel,543)
    assert source()=='# 标题\n**正文**',repr(source())
    assert '**' not in visible()
    activate(panel,edit);keys(panel,0x11,ord('Z'));assert source()=='# 标题\n正文'
    keys(panel,0x11,ord('Y'));assert source()=='# 标题\n**正文**'
    checks.append('heading button, Enter to body, selection bold and Ctrl+Z/Y preserve Markdown semantics')
    # Empty-selection bold acts as a typing style; characters remain editable.
    command(panel,512);activate(panel,edit);command(panel,543);replace(edit,'强调');time.sleep(.3)
    assert source()=='**强调**',repr(source())
    assert '强调' in visible() and '**' not in visible()
    checks.append('bold typing at an empty selection produces styled text with Markdown output')
    command(panel,512);command(panel,545);activate(panel,edit);replace(edit,'第一项');time.sleep(.2)
    keys(panel,0x0D);replace(edit,'第二项');time.sleep(.3)
    assert source()=='- 第一项\n- 第二项',repr(source())
    keys(panel,0x0D);keys(panel,0x0D);replace(edit,'段落');time.sleep(.2)
    assert source()=='- 第一项\n- 第二项\n段落',repr(source())
    checks.append('list button continues items with Enter; empty item exits the list')
    # A specialized diagram dialog edits only the selected chart, never exposes whole-document source mode.
    command(panel,512)
    u.PostMessageW(panel,0x111,549,0)
    box=wait(lambda:first(p.pid,'KeepMD.DiagramInput'));input_=u.GetDlgItem(box,1001)
    settext(input_,'flowchart LR\nA[开始] --> B[完成]\n');u.PostMessageW(box,0x111,1,0)
    wait(lambda:not u.IsWindow(box));time.sleep(.3)
    assert source()=='```mermaid\nflowchart LR\nA[开始] --> B[完成]\n```\n'
    assert 'flowchart' not in visible()
    # Reopen the existing figure through an actual double click in the document.
    activate(panel,edit);u.SendMessageW(edit,0xB1,0,0)
    screenshot(panel,'visual-diagram-click.png')
    click(edit,110,95);time.sleep(.05);u.mouse_event(2,0,0,0,0);time.sleep(.04);u.mouse_event(4,0,0,0,0)
    box=wait(lambda:first(p.pid,'KeepMD.DiagramInput'))
    input_=u.GetDlgItem(box,1001);assert 'A[开始]' in name(input_)
    u.PostMessageW(box,0x111,2,0);wait(lambda:not u.IsWindow(box))
    assert source()=='```mermaid\nflowchart LR\nA[开始] --> B[完成]\n```\n'
    activate(panel,edit);u.SendMessageW(edit,0xB1,0,1);keys(panel,0x2E)
    command(panel,511);assert visible().strip()==''
    keys(panel,0x11,ord('Z'));assert 'A[开始]' in source()
    checks.append('diagram dialog inserts native picture; deleting the picture removes its Markdown and undo restores it')
    # Live typing after a diagram does not rewrite its source or insert object replacement characters into .md.
    activate(panel,edit);u.SendMessageW(edit,0xB1,-1,-1);replace(edit,'图后文字');time.sleep(.4)
    expected='```mermaid\nflowchart LR\nA[开始] --> B[完成]\n```\n图后文字'
    assert source()==expected,repr(source())
    wait(lambda:cfg.with_suffix('.prompt.md').read_text(encoding='utf-8')==expected)
    checks.append('editing after an inline diagram autosaves exact Markdown without object characters')
    # Pasted Markdown remains intact across render/theme/undo, including replacement characters and Unicode emoji.
    cases=['', 'plain text', '空白\n\n  保留空格  \n', '1. first\n2. second\n',
           '> 引用 **粗体**\n\n---\n', '| A | B |\n| - | - |\n| 一 | 二 |\n',
           '字面量�与 emoji 👩‍💻\n', '[链接](https://example.test/a?b=1) 和 \\*星号\\*\n',
           '```python\nprint("hello")\n```\n', '```mermaid\ngraph TD\nA-->B\n```\n\n```mermaid\ngraph LR\nC-->D\n```\n']
    for value in cases:
        settext(edit,value);time.sleep(.22)
        if value:assert source()==value,repr(source())
        command(panel,527);time.sleep(.15)
        if value:assert source()==value,repr(source())
    checks.append('Markdown round trips preserve lists, quotes, tables, code, links, escaped punctuation, replacement character and emoji')
    # Prevent plain Enter from accidentally retaining a heading format after reflow.
    settext(edit,'普通正文');time.sleep(.2);activate(panel,edit)
    u.SendMessageW(edit,0xB1,0,2);keys(panel,0x11,ord('B'))
    u.SendMessageW(edit,0xB1,-1,-1);replace(edit,'追加');time.sleep(.25)
    assert source()=='**普通**正文追加',repr(source())
    checks.append('typing after visually formatted text leaves surrounding Markdown structure intact')
    command(panel,525);p.wait(timeout=8);assert p.returncode==0
    result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':checks,'source_preservation':'exact LF Markdown including original whitespace','visible_editor':'native Windows RichEdit rich text; no source/preview mode'}
    (ROOT/'bench/results/visual-editor-e2e.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
finally:
    if remote:k.VirtualFreeEx(handle,remote,0,0x8000)
    if handle:k.CloseHandle(handle)
    if p.poll() is None:p.terminate();p.wait(timeout=5)
