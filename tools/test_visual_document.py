"""KeepMD visual (WYSIWYG) editing E2E: New opens it, typing and formatting keep the exact Markdown."""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import subprocess
import time
from test_gui import ROOT,OUT,u,windows,children,name,wait,activate,screenshot,CALLBACK

u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.IsWindow.argtypes=[W.HWND]
u.GetDlgCtrlID.argtypes=[W.HWND];u.GetDlgCtrlID.restype=C.c_int
class KEYBDINPUT(C.Structure):_fields_=[('vk',W.WORD),('scan',W.WORD),('flags',W.DWORD),('time',W.DWORD),('extra',C.c_size_t)]
class MOUSEINPUT(C.Structure):_fields_=[('dx',W.LONG),('dy',W.LONG),('data',W.DWORD),('flags',W.DWORD),('time',W.DWORD),('extra',C.c_size_t)]
class UNION(C.Union):_fields_=[('keyboard',KEYBDINPUT),('mouse',MOUSEINPUT)]
class INPUT(C.Structure):_fields_=[('type',W.DWORD),('data',UNION)]
u.SendInput.argtypes=[W.UINT,C.POINTER(INPUT),C.c_int];u.SendInput.restype=W.UINT
Edit,Save,SaveAs,New,Split,EditMode,FormatBold=117,118,119,120,124,129,142
def command(hwnd,value):u.SendMessageW(hwnd,0x111,value,0)
def editable(hwnd):return u.GetDlgItem(hwnd,202)
def toolbar(hwnd):return next(h for h in children(hwnd) if name(h,True)=='ToolbarWindow32')
def hidden(hwnd,id_):return (u.SendMessageW(toolbar(hwnd),0x412,id_,0)&8)!=0 # TB_GETSTATE & TBSTATE_HIDDEN
def visual(hwnd):return bool(editable(hwnd)) and not hidden(hwnd,FormatBold)
def source(hwnd):
    # WM_GETTEXT reports an inline diagram object as one placeholder character in front of its hidden fence;
    # the editor's own Markdown never contains it, which the saved bytes verify separately.
    value=name(editable(hwnd)).replace('\r\n','\n').replace('\r','\n')
    return value.replace('\n\ufffc```','\n```').replace('\n ```mermaid','\n```mermaid')
def modified(hwnd):return name(hwnd).startswith('* ')
def send(events):
    assert u.SendInput(len(events),events,C.sizeof(INPUT))==len(events)
def keys(expected,*vks):
    assert u.GetForegroundWindow()==expected,'Focus left the test window; input not sent'
    events=(INPUT*(2*len(vks)))()
    for i,vk in enumerate(vks):events[i].type=1;events[i].data.keyboard.vk=vk
    for i,vk in enumerate(reversed(vks),len(vks)):events[i].type=1;events[i].data.keyboard.vk=vk;events[i].data.keyboard.flags=2
    send(events);time.sleep(.15)
def type_text(hwnd,value):
    # Characters arrive as KEYEVENTF_UNICODE key input and line breaks as the real Enter key, so the
    # editor's own typing path runs; nothing is injected with WM_SETTEXT.
    for index,line in enumerate(value.split('\n')):
        if index:keys(hwnd,0x0D)
        units=[int.from_bytes(c.encode('utf-16-be')[i:i+2],'big') for c in line for i in range(0,len(c.encode('utf-16-be')),2)]
        if not units:continue
        assert u.GetForegroundWindow()==hwnd,'Focus left the test window; no text sent'
        events=(INPUT*(2*len(units)))()
        for i,unit in enumerate(units):
            events[2*i].type=1;events[2*i].data.keyboard.scan=unit;events[2*i].data.keyboard.flags=4
            events[2*i+1].type=1;events[2*i+1].data.keyboard.scan=unit;events[2*i+1].data.keyboard.flags=4|2
        send(events);time.sleep(.3)
def dialog(pid):
    found=[]
    def collect(hwnd,_):
        owner=W.DWORD();u.GetWindowThreadProcessId(hwnd,C.byref(owner))
        if owner.value==pid and name(hwnd,True)=='#32770' and u.IsWindowVisible(hwnd):found.append(hwnd)
        return True
    callback=CALLBACK(collect);u.EnumWindows(callback,0);return (found or [None])[0]
def close(process,hwnd,expect_prompt=False):
    u.PostMessageW(hwnd,0x10,0,0)
    if expect_prompt:
        box=wait(lambda:dialog(process.pid));u.PostMessageW(box,0x111,7,0) # Discard only the generated test draft.
    process.wait(timeout=8);assert process.returncode==0
def launch(*args):
    process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),*map(str,args),'--config',str(config)],cwd=ROOT)
    main=wait(lambda:(windows(process.pid)or[None])[0])
    return process,main
def reading(main,path):
    wait(lambda:path.name in name(main))
    wait(lambda:'阅读' in name(main),timeout=30)
    return None
def preference():
    return next((line.split('=',1)[1] for line in config.read_text(encoding='utf-8').splitlines() if line.startswith('Visual=')),None)

checks=[];process=None
config=OUT/'visual-document.ini';config.write_text('[Reader]\nDark=0\nVisual=1\n',encoding='utf-8')
path=OUT/'visual-document.md'
original=('# 标题\n\n**粗体** 与 *斜体* 正文。\n\n- 第一项\n- 第二项\n\n| 模块 | 实现 |\n| :--- | ---: |\n'
          '| 正文 | DirectWrite |\n\n> 引用行\n\n```mermaid\nflowchart LR\nA[输入] --> B[结果]\n```\n')
path.write_text(original,encoding='utf-8',newline='')
edited=original.replace('- 第一项','- **第一项**')+'\n结尾段落 😀\n'
try:
    # An existing document: exact source, typing, Ctrl+B, exact save and a mode round trip.
    process,main=launch(path);wait(lambda:path.name in name(main))
    command(main,Edit);wait(lambda:visual(main));time.sleep(.6)
    assert source(main)==original,repr(source(main))
    assert hidden(main,Split) and all(u.IsWindowVisible(u.GetDlgItem(next(c for c in children(main) if name(c,True)=='Keep.Caption'),i)) for i in (130,131,132)) and not modified(main)
    command(main,Save);assert path.read_text(encoding='utf-8')==original,'unused save changed the file'
    checks.append('existing document opens in visual editing with the exact Markdown; split-only actions are hidden')
    edit=editable(main);activate(main,edit);keys(main,0x11,0x23) # Ctrl+End
    type_text(main,'\n结尾段落 😀\n')
    wait(lambda:source(main)==original+'\n结尾段落 😀\n')
    start=original.index('第一项');u.SendMessageW(edit,0xB1,start,start+3)
    activate(main,edit);keys(main,0x11,ord('B'))
    wait(lambda:source(main)==edited);assert modified(main)
    screenshot(main,'visual-document-light.png')
    checks.append('real Unicode typing, Enter and Ctrl+B edit visually while the Markdown markers stay in the source')
    command(main,Save);assert path.read_text(encoding='utf-8')==edited,repr(path.read_text(encoding='utf-8'))
    assert not modified(main)
    checks.append('saving a visual edit writes the exact Markdown, including the untouched table and flowchart')
    command(main,EditMode);wait(lambda:hidden(main,FormatBold));time.sleep(.3)
    assert not hidden(main,Split) and name(editable(main)).replace('\r\n','\n').replace('\r','\n')==edited
    command(main,EditMode);wait(lambda:visual(main));time.sleep(.4)
    assert source(main)==edited and not modified(main)
    command(main,EditMode);wait(lambda:hidden(main,FormatBold))
    close(process,main);assert preference()=='0',config.read_text(encoding='utf-8')
    checks.append('Ctrl+E switches visual/source both ways without changing the text and remembers the last mode')

    # The remembered source preference applies to Edit, while New always starts in visual editing.
    process,main=launch(path);wait(lambda:path.name in name(main))
    command(main,Edit);wait(lambda:editable(main));time.sleep(.3)
    assert hidden(main,FormatBold),'Visual=0 must open the Markdown source editor'
    command(main,Edit);wait(lambda:not editable(main))
    target=OUT/'visual-new.md'
    if target.exists():target.unlink()
    command(main,New);wait(lambda:visual(main));time.sleep(.5)
    edit=editable(main);activate(main,edit)
    typed='# 新文档\n\n正文 **加粗**\n'
    type_text(main,typed)
    wait(lambda:source(main)==typed);assert modified(main)
    screenshot(main,'visual-document-new.png')
    # Each typing burst is one undo step; a pause starts the next one.
    type_text(main,'追加');wait(lambda:source(main)==typed+'追加')
    time.sleep(1.3);type_text(main,'内容');wait(lambda:source(main)==typed+'追加内容')
    keys(main,0x11,ord('Z'));wait(lambda:source(main)==typed+'追加')
    keys(main,0x11,ord('Z'));wait(lambda:source(main)==typed)
    keys(main,0x11,ord('Y'));wait(lambda:source(main)==typed+'追加')
    keys(main,0x11,ord('Z'));wait(lambda:source(main)==typed)
    checks.append('Ctrl+Z/Ctrl+Y undo and redo whole typing bursts; a pause separates two undo steps')
    u.PostMessageW(main,0x111,SaveAs,0)
    box=wait(lambda:dialog(process.pid))
    field=wait(lambda:next((c for c in children(box) if name(c,True)=='Edit' and u.GetDlgCtrlID(c) in (1148,1001)
                            and u.IsWindowVisible(c) and u.IsWindowEnabled(c)),None))
    def named():
        value=C.create_unicode_buffer(str(target));u.SendMessageW(field,0xC,0,C.cast(value,C.c_void_p).value)
        return name(field)==str(target)
    wait(named);u.PostMessageW(box,0x111,1,0)
    wait(lambda:not u.IsWindow(box));wait(lambda:target.exists() and not modified(main))
    assert target.read_bytes()=='# 新文档\n\n正文 **加粗**\n'.encode('utf-8')
    close(process,main)
    checks.append('New opens visual editing even when Edit prefers source; typed Markdown saves as a new UTF-8 file')

    # RichEdit turns U+FFFC into a space, so such a document stays in reading mode rather than being rewritten.
    config.write_text('[Reader]\nDark=0\nVisual=1\n',encoding='utf-8')
    reserved=OUT/'visual-reserved.md';reserved.write_text('# 保留 \ufffc 字符\n',encoding='utf-8',newline='')
    process,main=launch(reserved);reading(main,reserved)
    command(main,Edit);time.sleep(.3)
    assert 'U+FFFC' in name(main),name(main)
    assert not editable(main) and reserved.read_text(encoding='utf-8')=='# 保留 \ufffc 字符\n'
    close(process,main)
    checks.append('a document with U+FFFC stays in reading mode and says why, instead of being silently rewritten')

    # Documents beyond the responsive visual limit (256 Ki UTF-16 units) open, and stay, in source editing.
    section=('## 第 {i} 节\n\nKeepMD 在可视化编辑时保持 Markdown 源码不变，and the reader renders it with '
             'DirectWrite。这一段包含中文、English words 与数字 {i}。\n\n- 列表项 **重点** 与 `code`\n- 第二项\n\n')
    body='# 大文档\n\n'+''.join(section.format(i=i) for i in range(2200))
    assert len(body)>256*1024,len(body)
    large=OUT/'visual-large.md';large.write_text(body,encoding='utf-8',newline='')
    process,main=launch(large);reading(main,large)
    command(main,Edit);edit=wait(lambda:editable(main),timeout=20)
    assert hidden(main,FormatBold) and '26 万字符' in name(main),name(main)
    command(main,EditMode);time.sleep(.3)
    assert editable(main)==edit and hidden(main,FormatBold) and '26 万字符' in name(main),name(main)
    assert not modified(main);close(process,main)
    checks.append('documents beyond the visual limit open in source editing, say why and refuse the visual switch')
finally:
    if process and process.poll() is None:process.terminate();process.wait(timeout=5)
result={'exe_sha256':hashlib.sha256((ROOT/'build/release/keepmd.exe').read_bytes()).hexdigest(),'checks':checks}
(ROOT/'bench/results/visual-document-e2e.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,ensure_ascii=False,indent=2))
