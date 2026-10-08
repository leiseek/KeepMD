"""Toolbar collapse/expand remains reachable in reading, editing, split preview and prompt modes."""
import ctypes as C
from ctypes import wintypes as W
import hashlib,json,subprocess,time
from test_gui import ROOT,OUT,u,windows,children,name,wait,activate,click,screenshot,CALLBACK
u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetWindowRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
u.keybd_event.argtypes=[W.BYTE,W.BYTE,W.DWORD,C.c_size_t]
def rect(h):
    r=W.RECT();u.GetWindowRect(h,C.byref(r));return r.left,r.top,r.right,r.bottom
def command(h,id):u.SendMessageW(h,0x111,id,0)
def keys(h,*vks):
    assert u.GetForegroundWindow()==h
    for v in vks:u.keybd_event(v,0,0,0)
    for v in reversed(vks):u.keybd_event(v,0,2,0)
    time.sleep(.1)
def toggle_button(h):
    caption=next(c for c in children(h) if name(c,True)=='KeepMD.Caption')
    return u.GetDlgItem(caption,9104)
def first_prompt(pid):
    found=[]
    def f(h,_):
        p=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(p))
        if p.value==pid and name(h,True).startswith('KeepMD.Prompt.') and u.IsWindowVisible(h):found.append(h)
        return True
    cb=CALLBACK(f);u.EnumWindows(cb,0);return (found or [None])[0]
config=OUT/'toolbar-toggle.ini';config.write_text('[Reader]\nDark=1\nToolbar=1\n',encoding='utf-8')
config.with_suffix('.prompt.ini').write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=0\nToolbar=1\n',encoding='utf-8')
config.with_suffix('.prompt.md').write_text('# 提示词\n\n保持输入内容。\n',encoding='utf-8')
exe=ROOT/'build/release/keepmd.exe';prompt_exe=ROOT/'build/release/KeepPrompt.exe';checks=[];prompt=None
def launch():return subprocess.Popen([str(exe),str(ROOT/'tests/fixtures/welcome.md'),'--config',str(config)],cwd=ROOT)
p=launch()
try:
    main=wait(lambda:(windows(p.pid)or[None])[0]);wait(lambda:'welcome.md' in name(main))
    toolbar=next(h for h in children(main) if name(h,True)=='ToolbarWindow32')
    reader=next(h for h in children(main) if name(h,True)=='KeepMD.Reader')
    before=rect(reader);button=toggle_button(main)
    click(button,16,16);wait(lambda:not u.IsWindowVisible(toolbar))
    assert name(button)=='展开工具栏' and u.IsWindowVisible(button)
    assert rect(reader)[1]<before[1]-30
    click(button,16,16);wait(lambda:u.IsWindowVisible(toolbar));assert name(button)=='收起工具栏'
    checks.append('reader caption toggle stays visible and returns space to document when toolbar is collapsed')
    command(main,117);edit=wait(lambda:u.GetDlgItem(main,202));text=name(edit)
    activate(main,edit);keys(main,0x11,0x10,ord('T'));wait(lambda:not u.IsWindowVisible(toolbar))
    assert u.IsWindowVisible(reader) and u.IsWindowVisible(edit) and name(edit)==text
    screenshot(main,'toolbar-split-collapsed.png')
    command(main,124);assert not u.IsWindowVisible(reader)
    click(button,16,16);wait(lambda:u.IsWindowVisible(toolbar))
    command(main,124);command(main,117)
    assert not u.GetDlgItem(main,202) and u.IsWindowVisible(toolbar)
    checks.append('Ctrl+Shift+T and caption toggle work in split and source-only editing without changing content')
    prompt=subprocess.Popen([str(prompt_exe),'--show','--config',str(config)],cwd=ROOT);panel=wait(lambda:first_prompt(prompt.pid));e=wait(lambda:u.GetDlgItem(panel,500))
    formats=[u.GetDlgItem(panel,id) for id in range(540,550)];assert all(u.IsWindowVisible(h) for h in formats)
    before=rect(e);prompt_toggle=toggle_button(panel);text=name(e)
    click(prompt_toggle,16,16);wait(lambda:all(not u.IsWindowVisible(h) for h in formats))
    assert rect(e)[1]<before[1]-30 and name(e)==text and u.IsWindowVisible(prompt_toggle)
    assert u.IsWindowVisible(u.GetDlgItem(panel,510)) # copying remains available
    screenshot(panel,'toolbar-prompt-collapsed.png')
    activate(panel,e);keys(panel,0x11,0x10,ord('T'));wait(lambda:all(u.IsWindowVisible(h) for h in formats))
    assert u.IsWindowVisible(toolbar)
    checks.append('prompt format toolbar collapses independently; copy action and caption toggle remain accessible')
    command(panel,528)
    assert u.IsWindowVisible(toolbar) and all(not u.IsWindowVisible(h) for h in formats)
    u.PostMessageW(panel,0x10,0,0);wait(lambda:not u.IsWindowVisible(panel))
    click(toggle_button(main),16,16);wait(lambda:not u.IsWindowVisible(toolbar))
    command(main,101);p.wait(timeout=8);assert p.returncode==0
    assert 'Toolbar=0' in config.read_text(encoding='utf-8')
    assert 'Toolbar=0' in config.with_suffix('.prompt.ini').read_text(encoding='utf-8')
    p=launch();main=wait(lambda:(windows(p.pid)or[None])[0]);wait(lambda:'welcome.md' in name(main))
    toolbar=next(h for h in children(main) if name(h,True)=='ToolbarWindow32');assert not u.IsWindowVisible(toolbar)
    broker=subprocess.Popen([str(exe),'--prompt','--config',str(config)],cwd=ROOT);broker.wait(timeout=8);panel=wait(lambda:first_prompt(prompt.pid));wait(lambda:u.GetDlgItem(panel,540))
    assert not u.IsWindowVisible(u.GetDlgItem(panel,540))
    click(toggle_button(panel),16,16);wait(lambda:u.IsWindowVisible(u.GetDlgItem(panel,540)))
    # The prompt is intentionally topmost; hide it before physically clicking
    # the reader button behind it, rather than accidentally clicking through it.
    u.PostMessageW(panel,0x10,0,0);wait(lambda:not u.IsWindowVisible(panel))
    click(toggle_button(main),16,16);wait(lambda:u.IsWindowVisible(toolbar))
    checks.append('reader and prompt toolbar preferences survive restart and can both be expanded again')
    command(main,101);p.wait(timeout=8);assert p.returncode==0
    command(panel,525);prompt.wait(timeout=8);assert prompt.returncode==0
    result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':checks}
    (ROOT/'bench/results/toolbar-toggle-e2e.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result,indent=2))
finally:
    if prompt and prompt.poll() is None:prompt.terminate();prompt.wait(timeout=5)
    if p.poll() is None:p.terminate();p.wait(timeout=5)
