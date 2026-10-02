"""Native Prompt Flow integration E2E. Uses real global keyboard input and a private target window."""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import psutil
from test_gui import ROOT, OUT, u, k, children, name, wait, activate, screenshot, clipboard, CALLBACK, click

u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetDlgCtrlID.argtypes=[W.HWND];u.GetDlgCtrlID.restype=C.c_int
u.IsWindow.argtypes=[W.HWND]
u.CreateWindowExW.argtypes=[W.DWORD,W.LPCWSTR,W.LPCWSTR,W.DWORD,C.c_int,C.c_int,C.c_int,C.c_int,W.HWND,W.HMENU,W.HINSTANCE,C.c_void_p]
u.CreateWindowExW.restype=W.HWND
u.RegisterHotKey.argtypes=[W.HWND,C.c_int,W.UINT,W.UINT]
u.UnregisterHotKey.argtypes=[W.HWND,C.c_int]
u.FindWindowW.argtypes=[W.LPCWSTR,W.LPCWSTR];u.FindWindowW.restype=W.HWND
u.ShowWindow.argtypes=[W.HWND,C.c_int]
u.GetMessageW.argtypes=[C.POINTER(W.MSG),W.HWND,W.UINT,W.UINT]
u.DispatchMessageW.argtypes=[C.POINTER(W.MSG)];u.DispatchMessageW.restype=W.LPARAM
u.TranslateMessage.argtypes=[C.POINTER(W.MSG)]
if '--target' in sys.argv:
    h=u.CreateWindowExW(0,'EDIT','KeepMD Test Paste Target',0x00CF0000|0x0004|0x1000,80,80,520,300,None,None,None,None)
    assert h
    u.ShowWindow(h,5)
    message=W.MSG()
    while u.GetMessageW(C.byref(message),None,0,0)>0:
        u.TranslateMessage(C.byref(message));u.DispatchMessageW(C.byref(message))
    sys.exit(0)

class KEYBDINPUT(C.Structure):_fields_=[('vk',W.WORD),('scan',W.WORD),('flags',W.DWORD),('time',W.DWORD),('extra',C.c_size_t)]
class MOUSEINPUT(C.Structure):_fields_=[('dx',W.LONG),('dy',W.LONG),('data',W.DWORD),('flags',W.DWORD),('time',W.DWORD),('extra',C.c_size_t)]
class UNION(C.Union):_fields_=[('keyboard',KEYBDINPUT),('mouse',MOUSEINPUT)]
class INPUT(C.Structure):_fields_=[('type',W.DWORD),('data',UNION)]
u.SendInput.argtypes=[W.UINT,C.POINTER(INPUT),C.c_int];u.SendInput.restype=W.UINT
def keys(expected,*vks):
    assert u.GetForegroundWindow()==expected,'Focus left the private test window; input not sent'
    events=(INPUT*(2*len(vks)))()
    for i,vk in enumerate(vks):events[i].type=1;events[i].data.keyboard.vk=vk
    for i,vk in enumerate(reversed(vks),len(vks)):
        events[i].type=1;events[i].data.keyboard.vk=vk;events[i].data.keyboard.flags=2
    assert u.SendInput(len(events),events,C.sizeof(INPUT))==len(events)
    time.sleep(.09)
def text(h,value):
    s=C.create_unicode_buffer(value);u.SendMessageW(h,0xC,0,C.cast(s,C.c_void_p).value)
def tops(pid,cls):
    found=[]
    def collect(h,_):
        p=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(p))
        if p.value==pid and name(h,True).startswith(cls):found.append(h)
        return True
    cb=CALLBACK(collect);u.EnumWindows(cb,0);return found
def first(pid,cls):return (tops(pid,cls)or[None])[0]
def cmd(h,id):u.SendMessageW(h,0x111,id,0)
def choose(process,h,command,path,replace=False):
    u.PostMessageW(h,0x111,command,0)
    d=wait(lambda:first(process.pid,'#32770'))
    f=wait(lambda:next((c for c in children(d) if name(c,True)=='Edit' and u.GetDlgCtrlID(c) in (1148,1001)),None))
    text(f,str(path));u.PostMessageW(d,0x111,1,0)
    if replace:
        def confirmation():
            return next((box for box in tops(process.pid,'#32770')
                         if any('用导入内容替换' in name(c) for c in children(box))),None)
        box=wait(confirmation);u.PostMessageW(box,0x111,1,0)
        wait(lambda:not u.IsWindow(box))
    else:wait(lambda:not u.IsWindow(d))

exe=ROOT/'build/release/keepmd.exe'
folder=OUT/'prompt-e2e';folder.mkdir(exist_ok=True)
config=folder/'profile.ini';prefs=config.with_suffix('.prompt.ini');draft=config.with_suffix('.prompt.md')
config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
prefs.write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\n',encoding='utf-8')
if draft.exists():draft.unlink()
checks=[];measurements={};process=None;target_process=None;held=False
old_foreground=u.GetForegroundWindow()
def launch(*args):return subprocess.Popen([str(exe),'--config',str(config),*args],cwd=ROOT)
try:
    target_process=subprocess.Popen([sys.executable,__file__,'--target'],cwd=ROOT)
    target=wait(lambda:first(target_process.pid,'Edit'));text(target,'');activate(target)
    process=launch('--resident')
    panel=wait(lambda:first(process.pid,'KeepMD.Prompt.'))
    main=wait(lambda:first(process.pid,'KeepMD.Window'))
    assert not u.IsWindowVisible(panel) and not u.IsWindowVisible(main)
    assert not u.GetDlgItem(panel,500)
    modules=[m.path.lower() for m in psutil.Process(process.pid).memory_maps()]
    assert not any('msftedit.dll' in m for m in modules)
    checks.append('resident startup: hidden, editor and RichEdit DLL not loaded')
    mem=psutil.Process(process.pid).memory_info();measurements['cold_resident_private_mib']=mem.private/2**20
    activate(target);start=time.perf_counter();keys(target,0x11,0x12,0x20)
    wait(lambda:u.IsWindowVisible(panel) and u.GetForegroundWindow()==panel)
    measurements['first_summon_observed_ms']=(time.perf_counter()-start)*1000
    edit=wait(lambda:u.GetDlgItem(panel,500));status=u.GetDlgItem(panel,501)
    sample='# 提示词 😀\n\n请按下列步骤处理：\n\n- 保留 **Markdown**\n- 输出简明结论\n\n```mermaid\nflowchart LR\nA[输入] --> B[检查] --> C[结果]\n```\n\n    缩进代码\n\n'
    text(edit,sample);wait(lambda:draft.exists() and draft.read_text(encoding='utf-8')==sample)
    time.sleep(.5);screenshot(panel,'prompt-preview.png')
    assert not any(name(h,True)=='KeepMD.PromptPreview' for h in children(panel))
    activate(panel,edit);keys(panel,0x11,ord('A'));keys(panel,0x11,ord('C'))
    assert '提示词' in clipboard() and 'Markdown' in clipboard()
    checks.append('single visual editor, Markdown/flowchart formatting and exact source autosave')
    activate(panel,edit);keys(panel,0x11,0x0D)
    wait(lambda:not u.IsWindowVisible(panel));wait(lambda:u.GetForegroundWindow()==target)
    assert clipboard()==sample,(repr(clipboard()),repr(sample))
    keys(target,0x11,ord('V'));assert name(target).replace('\r\n','\n')==sample,repr(name(edit))
    checks.append('Ctrl+Enter copies exact Markdown and returns focus; target pastes source')
    keys(target,0x11,0x12,0x20);wait(lambda:u.GetForegroundWindow()==panel)
    cmd(panel,511);assert clipboard()==sample
    assert u.OpenClipboard(None);held=True
    cmd(panel,510)
    assert u.IsWindowVisible(panel) and '剪贴板正忙' in name(status)
    u.CloseClipboard();held=False
    checks.append('clipboard contention keeps panel and draft visible')
    keys(panel,0x1B);wait(lambda:not u.IsWindowVisible(panel))
    keys(target,0x11,0x12,0x20);wait(lambda:u.GetForegroundWindow()==panel)
    # Conflicting replacement must not release the currently working shortcut.
    cmd(panel,514);hk=u.GetDlgItem(panel,502)
    assert u.RegisterHotKey(None,997,0x4000|2|4,ord('J'))
    try:
        text(hk,'Ctrl+Shift+J');cmd(panel,515)
        assert '注册失败' in name(status)
    finally:u.UnregisterHotKey(None,997)
    keys(panel,0x1B);wait(lambda:not u.IsWindowVisible(panel))
    keys(target,0x11,0x12,0x20);wait(lambda:u.GetForegroundWindow()==panel)
    checks.append('failed shortcut change preserves previous registered shortcut')
    text(hk,'Ctrl+Shift+J');cmd(panel,515)
    keys(panel,0x1B);wait(lambda:u.GetForegroundWindow()==target)
    keys(target,0x11,0x10,ord('J'));wait(lambda:u.GetForegroundWindow()==panel)
    checks.append('custom shortcut summons from another process')
    # Double Ctrl and Ctrl+C must not interfere with each other.
    cmd(panel,516)
    if u.FindWindowW('PromptFlow_MSVC_Class',None):
        assert '旧 Prompt Flow 正在运行' in name(status)
        checks.append('existing Prompt Flow double-Ctrl conflict is explicit and preserves combination shortcut')
        measurements['double_ctrl_live']='not run: existing user Prompt Flow is active; state machine covered by core tests'
    else:
        keys(panel,0x1B);wait(lambda:u.GetForegroundWindow()==target)
        keys(target,0xA2);keys(target,0xA2);wait(lambda:u.GetForegroundWindow()==panel)
        keys(panel,0x1B);wait(lambda:u.GetForegroundWindow()==target)
        keys(target,0x11,ord('C'));keys(target,0xA2);time.sleep(.2)
        assert not u.IsWindowVisible(panel)
        time.sleep(.4);keys(target,0xA2);keys(target,0xA2);wait(lambda:u.GetForegroundWindow()==panel)
        checks.append('double left Ctrl works; Ctrl+C then Ctrl does not summon')
    cmd(panel,517)
    # The integrated window must keep the source editor's undo chain.
    cmd(panel,512);assert not name(edit)
    activate(panel,edit);keys(panel,0x11,ord('Z'));cmd(panel,511);assert clipboard()==sample
    export=folder/'导出.md'
    if export.exists():export.unlink()
    choose(process,panel,522,export)
    def exported():
        try:return export.read_text(encoding='utf-8')==sample
        except (PermissionError,FileNotFoundError):return False
    wait(exported)
    legacy=folder/'legacy.json'
    legacy_text='# 从 Prompt Flow 导入\n\n  缩进 😀\n'
    legacy.write_text(json.dumps({'Prompt':legacy_text,'Hotkey':'Ctrl+Alt+Space','AlwaysOnTop':False,'Opacity':90,'Autostart':True}),encoding='utf-8')
    original=legacy.read_bytes()
    choose(process,panel,523,legacy,replace=True)
    wait(lambda:name(edit).replace('\r\n','\n').replace('\ufffc','')==legacy_text)
    assert legacy.read_bytes()==original
    checks.append('undoable clear, Markdown export and original Prompt Flow JSON import')
    # Second invocation forwards to existing service and exits.
    keys(panel,0x1B);wait(lambda:not u.IsWindowVisible(panel))
    other=launch('--prompt');other.wait(timeout=8);assert other.returncode==0
    wait(lambda:u.IsWindowVisible(panel));assert len(tops(process.pid,'KeepMD.Prompt.'))==1
    checks.append('second --prompt invocation reuses profile owner')
    # Close main while resident, then explicit exit.
    cmd(panel,524);wait(lambda:u.IsWindowVisible(main))
    u.PostMessageW(main,0x10,0,0);wait(lambda:not u.IsWindowVisible(main));assert process.poll() is None
    cmd(panel,525);process.wait(timeout=8)
    process=launch('--resident');panel=wait(lambda:first(process.pid,'KeepMD.Prompt.'));main=first(process.pid,'KeepMD.Window')
    activate(target);keys(target,0x11,0x12,0x20);wait(lambda:u.IsWindowVisible(panel))
    edit=u.GetDlgItem(panel,500);assert name(edit).replace('\r\n','\n').replace('\ufffc','')==legacy_text
    checks.append('close-to-tray, explicit exit and draft/settings restoration')
    # Real IME input: Escape cancels composition; it must not copy/hide the panel.
    activate(panel,edit);text(edit,'');toggled=False;attempts=[]
    for attempt in range(2):
        for c in 'NIHAO':keys(panel,ord(c))
        screenshot(panel,f'prompt-ime-{attempt}.png')
        keys(panel,0x20);current=name(edit);attempts.append(current)
        if any('\u4e00'<=c<='\u9fff' for c in current):break
        text(edit,'');keys(panel,0x10);toggled=not toggled
    else:raise AssertionError(f'IME did not commit Chinese: {attempts}')
    for c in 'NIHAO':keys(panel,ord(c))
    keys(panel,0x1B);assert u.IsWindowVisible(panel),'Escape during IME composition must not dismiss prompt'
    if toggled:keys(panel,0x10)
    checks.append('installed Chinese IME commits text; Esc cancels composition without dismissing prompt')
    text(edit,legacy_text);keys(panel,0x11,0x0D);wait(lambda:not u.IsWindowVisible(panel))
    time.sleep(1) # Allow focus/IME teardown messages to finish before measuring steady idle.
    proc=psutil.Process(process.pid);before=proc.cpu_times();io_before=proc.io_counters();stamp=draft.stat().st_mtime_ns;time.sleep(5)
    after=proc.cpu_times();io_after=proc.io_counters()
    measurements.update(hidden_idle_seconds=5,hidden_idle_cpu_seconds=(after.user+after.system)-(before.user+before.system),
                        hidden_idle_write_bytes=io_after.write_bytes-io_before.write_bytes,
                        hidden_private_mib=proc.memory_info().private/2**20,hidden_draft_changed=draft.stat().st_mtime_ns!=stamp)
    assert measurements['hidden_idle_cpu_seconds']<.15 and not measurements['hidden_draft_changed'], measurements
    checks.append('hidden prompt idles without rewriting draft or significant CPU')
    u.PostMessageW(main,0x111,101,0);process.wait(timeout=8)
    # Failed initial hotkey registration must be visible and recoverable.
    prefs.write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=1\n',encoding='utf-8')
    assert u.RegisterHotKey(None,998,0x4000|2|1,0x20)
    try:
        process=launch('--resident');panel=wait(lambda:first(process.pid,'KeepMD.Prompt.'))
        wait(lambda:u.IsWindowVisible(panel));status=u.GetDlgItem(panel,501)
        assert '注册失败' in name(status)
        text(u.GetDlgItem(panel,502),'Ctrl+Shift+J');cmd(panel,515)
        assert '已设置' in name(status)
        cmd(panel,525);process.wait(timeout=8)
    finally:u.UnregisterHotKey(None,998)
    checks.append('startup conflict shows settings and accepts alternative shortcut')
    # File replacement failure and an externally changed draft must never lose content.
    prefs.write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=0\n',encoding='utf-8')
    process=launch('--prompt');panel=wait(lambda:first(process.pid,'KeepMD.Prompt.'))
    edit=wait(lambda:u.GetDlgItem(panel,500));status=u.GetDlgItem(panel,501)
    k.CreateFileW.argtypes=[W.LPCWSTR,W.DWORD,W.DWORD,C.c_void_p,W.DWORD,W.DWORD,W.HANDLE];k.CreateFileW.restype=W.HANDLE
    k.CloseHandle.argtypes=[W.HANDLE]
    locked=k.CreateFileW(str(draft),0x80000000,1,None,3,0,None)
    assert locked and locked!=C.c_void_p(-1).value
    original=draft.read_bytes();text(edit,'# 锁定文件时的输入\n')
    try:
        cmd(panel,510)
        assert u.IsWindowVisible(panel) and '草稿未保存' in name(status)
        assert draft.read_bytes()==original
    finally:k.CloseHandle(locked)
    cmd(panel,510);wait(lambda:not u.IsWindowVisible(panel))
    assert draft.read_text(encoding='utf-8')=='# 锁定文件时的输入\n'
    activate(target);keys(target,0x11,0x12,0x20);wait(lambda:u.IsWindowVisible(panel))
    draft.write_text('# 磁盘上的外部修改\n',encoding='utf-8')
    text(edit,'# 内存中的未保存输入\n');cmd(panel,510)
    assert u.IsWindowVisible(panel) and '外部发生变化' in name(status)
    assert draft.read_text(encoding='utf-8')=='# 磁盘上的外部修改\n'
    u.PostMessageW(panel,0x111,525,0)
    box=wait(lambda:first(process.pid,'#32770'));u.PostMessageW(box,0x111,2,0)
    wait(lambda:not u.IsWindow(box));assert process.poll() is None
    rescue=folder/'recover.md'
    if rescue.exists():rescue.unlink()
    choose(process,panel,522,rescue)
    wait(lambda:rescue.exists());assert rescue.read_text(encoding='utf-8')=='# 内存中的未保存输入\n'
    u.PostMessageW(panel,0x111,525,0)
    box=wait(lambda:first(process.pid,'#32770'));u.PostMessageW(box,0x111,7,0)
    process.wait(timeout=8)
    assert draft.read_text(encoding='utf-8')=='# 磁盘上的外部修改\n'
    checks.append('locked save retries safely; external draft conflict supports cancel/export/discard without overwriting disk')
    # Corrupt persisted text stays untouched; no read failure may silently initialize a new draft.
    draft.write_bytes(b'\xffinvalid UTF8')
    process=launch('--prompt');panel=wait(lambda:first(process.pid,'KeepMD.Prompt.'))
    wait(lambda:u.IsWindowVisible(panel))
    edit=wait(lambda:u.GetDlgItem(panel,500));wait(lambda:not u.IsWindowEnabled(edit))
    time.sleep(1);assert draft.read_bytes()==b'\xffinvalid UTF8'
    cmd(panel,525);process.wait(timeout=8)
    draft.write_text(legacy_text,encoding='utf-8')
    checks.append('corrupt saved draft preserved and editing disabled until repaired')
    # Ordinary reader retains ordinary close behavior and has no prompt editor.
    prefs.write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=0\n',encoding='utf-8')
    process=launch(str(ROOT/'tests/fixtures/welcome.md'));main=wait(lambda:first(process.pid,'KeepMD.Window'))
    panel=wait(lambda:first(process.pid,'KeepMD.Prompt.'));assert not u.GetDlgItem(panel,500)
    u.PostMessageW(main,0x10,0,0);process.wait(timeout=8)
    checks.append('ordinary reader exits normally and never creates a prompt editor')
    # A command-line resident request must also enable residency in an existing ordinary reader.
    process=launch(str(ROOT/'tests/fixtures/welcome.md'));main=wait(lambda:first(process.pid,'KeepMD.Window'))
    panel=wait(lambda:first(process.pid,'KeepMD.Prompt.'))
    other=launch('--resident');other.wait(timeout=8);assert other.returncode==0
    wait(lambda:'Resident=1' in prefs.read_text(encoding='utf-8'))
    u.PostMessageW(main,0x10,0,0);wait(lambda:not u.IsWindowVisible(main));assert process.poll() is None
    u.PostMessageW(main,0x111,101,0);process.wait(timeout=8)
    checks.append('forwarded --resident request enables tray residency in an existing ordinary reader')
    result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':checks,'measurements':measurements}
    (ROOT/'bench/results/prompt-e2e.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
finally:
    if held:u.CloseClipboard()
    for p in (process,target_process):
        if p and p.poll() is None:p.terminate();p.wait(timeout=5)
    if old_foreground:u.SetForegroundWindow(old_foreground)
