"""Exercise the real Windows file dialogs and persisted reading navigation."""
import ctypes as C
from ctypes import wintypes as W
import json
import subprocess
import time
from test_gui import ROOT,OUT,u,windows,children,name,wait,screenshot,CALLBACK,click

u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetDlgCtrlID.argtypes=[W.HWND];u.GetDlgCtrlID.restype=C.c_int
u.IsWindow.argtypes=[W.HWND]
u.IsWindowEnabled.argtypes=[W.HWND]
u.GetDpiForWindow.argtypes=[W.HWND];u.GetDpiForWindow.restype=W.UINT
def text(hwnd,value):
    data=C.create_unicode_buffer(value);u.SendMessageW(hwnd,0xC,0,C.cast(data,C.c_void_p).value)
def dialog(pid):
    values=[]
    def collect(hwnd,_):
        found=W.DWORD();u.GetWindowThreadProcessId(hwnd,C.byref(found))
        if found.value==pid and name(hwnd,True)=='#32770' and u.IsWindowVisible(hwnd):values.append(hwnd)
        return True
    callback=CALLBACK(collect);u.EnumWindows(callback,0);return (values or [None])[0]
def choose_file(process,hwnd,command,path):
    u.PostMessageW(hwnd,0x111,command,0)
    box=wait(lambda:dialog(process.pid))
    controls=children(box)
    filename=next((h for h in controls if name(h,True)=='Edit' and u.GetDlgCtrlID(h) in (1148,1001)),None)
    if not filename:
        edits=[h for h in controls if name(h,True)=='Edit']
        if len(edits)==1:filename=edits[0]
    if not filename:raise AssertionError([(name(h,True),u.GetDlgCtrlID(h),name(h))for h in controls])
    text(filename,str(path))
    u.PostMessageW(box,0x111,1,0)
    wait(lambda:not u.IsWindow(box))

initial=OUT/'navigation 原文.md';saved=OUT/'navigation 另存为.md';nextfile=OUT/'navigation next.md'
initial.write_bytes(('# Navigation\r\n\r\n[Next](navigation%20next.md)\n\n'+''.join(f'## Section {i}\n\nParagraph {i}.\n\n'for i in range(80))).encode('utf-8'))
nextfile.write_text('# Companion\n\nNext document.\n',encoding='utf-8')
if saved.exists():saved.unlink()
config=OUT/'navigation.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
report=OUT/'navigation.json';checks=[]
def launch(path):return subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(path),'--config',str(config),'--report',str(report)],cwd=ROOT)
process=launch(initial)
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:name(hwnd).startswith(initial.name))
    choose_file(process,hwnd,119,saved)
    wait(lambda:saved.exists() and saved.name in name(hwnd))
    assert saved.read_bytes()==initial.read_bytes()
    checks.append('Save As dialog and identical source bytes')
    u.SendMessageW(hwnd,0x111,117,0)
    wait(lambda:not u.GetDlgItem(hwnd,202))
    reader=next(h for h in children(hwnd)if name(h,True)=='KeepMD.Reader')
    scale=u.GetDpiForWindow(reader)/96
    click(reader,int(65*scale),int(112*scale))
    wait(lambda:nextfile.name in name(hwnd))
    u.SendMessageW(hwnd,0x111,114,0);wait(lambda:saved.name in name(hwnd))
    checks.append('Mouse link hit testing and percent-encoded local path')
    for _ in range(8):u.SendMessageW(reader,0x100,0x22,0);time.sleep(.03)
    u.SendMessageW(hwnd,0x8000+100,0,0);before=json.loads(report.read_text())['anchor_block'];assert before>0
    choose_file(process,hwnd,100,nextfile)
    wait(lambda:nextfile.name in name(hwnd))
    u.SendMessageW(hwnd,0x111,114,0)
    wait(lambda:saved.name in name(hwnd));time.sleep(.1)
    u.SendMessageW(hwnd,0x8000+100,0,0);after=json.loads(report.read_text())['anchor_block']
    assert abs(after-before)<=1,(before,after)
    checks.append('Open dialog and Back restores logical reading block')
    u.SendMessageW(hwnd,0x111,107,0) # Zoom in
    time.sleep(.1);u.SendMessageW(hwnd,0x8000+100,0,0);before_close=json.loads(report.read_text())
    u.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=5)
finally:
    if process.poll() is None:process.terminate();process.wait(timeout=5)
process=launch(saved)
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:saved.name in name(hwnd));time.sleep(.1)
    u.SendMessageW(hwnd,0x8000+100,0,0);restored=json.loads(report.read_text())
    assert restored['zoom']==before_close['zoom']
    assert abs(restored['anchor_block']-before_close['anchor_block'])<=1
    checks.append('Unicode recent file, reading block and zoom survive restart')
    # An invalid external target must not replace the current document.
    u.SendMessageW(hwnd,0x111,117,0);editor=u.GetDlgItem(hwnd,202)
    before_text=name(editor)
    # Replace the on-disk file with invalid bytes, then use Reload on a clean editor.
    saved.write_bytes(b'\xffinvalid')
    u.SendMessageW(hwnd,0x111,112,0)
    wait(lambda:u.IsWindowEnabled(editor))
    assert u.GetDlgItem(hwnd,202)==editor and name(editor)==before_text
    checks.append('Failed reload preserves current source editor')
    saved.write_bytes(initial.read_bytes())
    u.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=5)
finally:
    if process.poll() is None:process.terminate();process.wait(timeout=5)
(ROOT/'bench/results/navigation-e2e.json').write_text(json.dumps({'checks':checks},indent=2)+'\n')
print(json.dumps({'checks':checks},indent=2))
