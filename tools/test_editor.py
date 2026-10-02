"""Native editor E2E on generated files only; verifies bytes, undo and dialogs."""
import ctypes as C
from ctypes import wintypes as W
import json
import subprocess
import time
from test_gui import ROOT,OUT,u,windows,children,name,wait,screenshot,CALLBACK

u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.IsWindow.argtypes=[W.HWND]
def command(hwnd,value):u.SendMessageW(hwnd,0x111,value,0)
def set_text(hwnd,value):
    text=C.create_unicode_buffer(value);u.SendMessageW(hwnd,0x000C,0,C.cast(text,C.c_void_p).value)
def replace_text(hwnd,value):
    u.SendMessageW(hwnd,0x00B1,0,-1)
    text=C.create_unicode_buffer(value);u.SendMessageW(hwnd,0x00C2,1,C.cast(text,C.c_void_p).value)
def dialog(pid,caption=None):
    result=[]
    def collect(hwnd,_):
        found=W.DWORD();u.GetWindowThreadProcessId(hwnd,C.byref(found))
        if found.value==pid and name(hwnd,True)=='#32770' and u.IsWindowVisible(hwnd) and (caption is None or name(hwnd)==caption):result.append(hwnd)
        return True
    callback=CALLBACK(collect);u.EnumWindows(callback,0);return (result or [None])[0]

checks=[]
def encode(value,encoding):return b'\xfe\xff'+value.encode('utf-16-be') if encoding=='utf-16-be-bom' else value.encode(encoding)
for encoding,newline in [('utf-8','\n'),('utf-8-sig','\r\n'),('utf-16','\r\n'),('utf-16-be-bom','\r\n')]:
    path=OUT/f'edit-{encoding}.md'
    initial=('# Before\n\n中文 原文 😀\n').replace('\n',newline)
    path.write_bytes(encode(initial,encoding));original=path.read_bytes()
    config=OUT/'editor-test.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
    process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(path),'--config',str(config)],cwd=ROOT)
    try:
        hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:name(hwnd).startswith(path.name))
        command(hwnd,117)
        editor=wait(lambda:u.GetDlgItem(hwnd,202))
        assert '中文 原文' in name(editor)
        command(hwnd,118);assert path.read_bytes()==original  # Save without editing preserves original bytes.
        replacement='# Edited\n\n中文 修改 😀\n'
        replace_text(editor,replacement)
        assert name(hwnd).startswith('* ')
        u.SendMessageW(editor,0x00C7,0,0) # Undo
        assert '原文' in name(editor)
        command(hwnd,118);assert path.read_bytes()==original
        u.SendMessageW(editor,0x400+84,0,0) # Redo
        assert '修改' in name(editor)
        command(hwnd,121)
        set_text(u.GetDlgItem(hwnd,200),'中文')
        set_text(u.GetDlgItem(hwnd,203),'更新')
        command(hwnd,123)
        assert '更新 修改' in name(editor)
        u.SendMessageW(editor,0x00C7,0,0)
        assert '中文 修改' in name(editor)
        u.SendMessageW(editor,0x400+84,0,0)
        assert '更新 修改' in name(editor)
        command(hwnd,118)
        expected=encode(replacement.replace('中文','更新').replace('\n',newline),encoding)
        assert path.read_bytes()==expected, (encoding,path.read_bytes(),expected)
        command(hwnd,117) # Return to reading and release the editor.
        wait(lambda:not u.GetDlgItem(hwnd,202))
        checks.append(f'{encoding}: load, unchanged save, Unicode edit, undo/redo, replace, encoded save, release')
        u.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=5)
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)

# Conflict cancellation and unsaved-close behavior.
path=OUT/'editor-conflict.md';path.write_text('# Original\n',encoding='utf-8')
process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(path),'--config',str(config)],cwd=ROOT)
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:name(hwnd).startswith(path.name));command(hwnd,117)
    editor=wait(lambda:u.GetDlgItem(hwnd,202));replace_text(editor,'# Unsaved 中文\n')
    time.sleep(.5);screenshot(hwnd,'editor-split.png')
    path.write_text('# External change\n',encoding='utf-8')
    u.PostMessageW(hwnd,0x111,118,0)
    conflict=wait(lambda:dialog(process.pid,'文件冲突'))
    u.PostMessageW(conflict,0x111,7,0) # No
    wait(lambda:not u.IsWindow(conflict));assert path.read_text()=='# External change\n'
    u.PostMessageW(hwnd,0x10,0,0)
    close=wait(lambda:dialog(process.pid));u.PostMessageW(close,0x111,2,0) # Cancel
    wait(lambda:not u.IsWindow(close));assert u.IsWindow(hwnd) and 'Unsaved' in name(editor)
    u.PostMessageW(hwnd,0x10,0,0)
    close=wait(lambda:dialog(process.pid));u.PostMessageW(close,0x111,7,0) # Discard only the generated test draft.
    process.wait(timeout=5);assert path.read_text()=='# External change\n'
    checks.append('external conflict does not overwrite; cancelled close retains draft; explicit discard closes')
finally:
    if process.poll() is None:process.terminate();process.wait(timeout=5)

# Editing ordinary text must reuse an unchanged, already rendered diagram.
path=OUT/'editor-cache.md';path.write_text('# Cache\n\n```mermaid\ngraph LR\nA[开始]-->B[完成]\n```\n\nTail.\n',encoding='utf-8')
report=OUT/'editor-cache.json'
process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(path),'--config',str(config),'--report',str(report)],cwd=ROOT)
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:path.name in name(hwnd))
    def stats():
        u.SendMessageW(hwnd,0x8000+100,0,0);return json.loads(report.read_text())
    wait(lambda:stats()['asset_bytes']>0)
    command(hwnd,117);editor=wait(lambda:u.GetDlgItem(hwnd,202))
    time.sleep(.2)
    replace_text(editor,path.read_text(encoding='utf-8').replace('Tail.','Changed tail.'))
    wait(lambda:stats()['reused_diagrams']>=1)
    assert stats()['reused_layouts']>=1
    command(hwnd,118);command(hwnd,117)
    wait(lambda:not u.GetDlgItem(hwnd,202))
    u.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=5)
    checks.append('unchanged diagram and text layouts reused across preview update')
finally:
    if process.poll() is None:process.terminate();process.wait(timeout=5)
(ROOT/'bench/results/editor-e2e.json').write_text(json.dumps({'checks':checks},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'checks':checks},ensure_ascii=False,indent=2))
