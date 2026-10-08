"""Check every packaged checksum and launch the EXE extracted from the actual ZIP."""
from pathlib import Path
import hashlib
import json
import subprocess
import zipfile
import ctypes as C
from ctypes import wintypes as W
from test_gui import ROOT,OUT,windows,wait,activate,u,name,children,CALLBACK,clipboard
u.GetWindowLongPtrW.argtypes=[W.HWND,C.c_int];u.GetWindowLongPtrW.restype=C.c_ssize_t

archive=ROOT/'dist/KeepMD-0.4.6-windows-x64.zip'
destination=(ROOT/'.cache/package-smoke').resolve()
destination.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(archive)as bundle:
    for item in bundle.infolist():
        target=(destination/item.filename).resolve()
        assert target.is_relative_to(destination),'Unsafe archive member'
    bundle.extractall(destination)
checks=0
for line in (destination/'SHA256SUMS.txt').read_text(encoding='utf-8-sig').splitlines():
    digest,relative=line.split('  ',1)
    assert hashlib.sha256((destination/relative).read_bytes()).hexdigest()==digest,relative
    checks+=1
summary=json.loads((destination/'bench/results/release-summary.json').read_text(encoding='utf-8'))
assert hashlib.sha256((destination/'keepmd.exe').read_bytes()).hexdigest()==summary['exe_sha256']
assert (destination/'KeepPrompt.exe').exists()
report=OUT/'packaged.json';snapshot=OUT/'packaged-reader.png';config=OUT/'packaged.ini'
config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
process=subprocess.Popen([str(destination/'keepmd.exe'),str(destination/'examples/欢迎.md'),'--config',str(config),'--report',str(report),'--snapshot',str(snapshot),'--exit-after','900'],cwd=destination)
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);activate(hwnd);process.wait(timeout=10);assert process.returncode==0
finally:
    if process.poll()is None:process.terminate();process.wait(timeout=5)
metrics=json.loads(report.read_text());assert metrics['blocks']>=15 and metrics['first_paint_ms']>0
assert '../使用说明.md' in (destination/'examples/欢迎.md').read_text(encoding='utf-8-sig')
assert 'KeepPrompt.exe' in (destination/'输入提示词.cmd').read_text(encoding='ascii')
promptConfig=OUT/'packaged-prompt.ini'
promptConfig.write_text('[Reader]\nDark=0\n',encoding='utf-8')
promptConfig.with_suffix('.prompt.ini').write_text('[Prompt]\nHotkey=Ctrl+Alt+Space\n',encoding='utf-8')
process=subprocess.Popen([str(destination/'KeepPrompt.exe'),'--resident','--config',str(promptConfig)],cwd=destination)
def prompt_window():
    found=[]
    def collect(h,_):
        pid=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(pid))
        if pid.value==process.pid and name(h,True).startswith('KeepMD.Prompt.') and u.IsWindowVisible(h):found.append(h)
        return True
    cb=CALLBACK(collect);u.EnumWindows(cb,0);return (found or [None])[0]
def prompt_window_any():
    found=[]
    def collect(h,_):
        pid=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(pid))
        if pid.value==process.pid and name(h,True).startswith('KeepMD.Prompt.'):found.append(h)
        return True
    cb=CALLBACK(collect);u.EnumWindows(cb,0);return (found or [None])[0]
try:
    panel=wait(prompt_window_any)
    u.SendMessageW(panel,0x8000+31,1,0)
    wait(lambda:u.IsWindowVisible(panel))
    assert any(name(h,True)=='KeepMD.Scrollbar' for h in children(panel))
    assert any(name(h,True)=='KeepMD.Caption' for h in children(panel))
    assert any(name(h)=='关闭窗口' for h in children(panel))
    assert not any(name(h,True)=='KeepMD.PromptPreview' for h in children(panel))
    assert any(name(h)=='标题 1' for h in children(panel))
    editor=next(h for h in children(panel) if name(h,True)=='RICHEDIT50W')
    assert not u.GetWindowLongPtrW(editor,-16)&0x00300000,'Packaged editor still has native scrollbar styles'
    sample='# Packaged prompt\n\n**中文提示词**\n'
    value=C.create_unicode_buffer(sample);u.SendMessageW(editor,0xC,0,C.cast(value,C.c_void_p).value)
    u.SendMessageW(panel,0x111,510,0)
    wait(lambda:not u.IsWindowVisible(panel));assert clipboard()==sample
    assert promptConfig.with_suffix('.prompt.md').read_text(encoding='utf-8')==sample
    u.SendMessageW(panel,0x111,525,0);process.wait(timeout=8);assert process.returncode==0
finally:
    if process.poll()is None:process.terminate();process.wait(timeout=5)
result={'zip_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'exe_sha256':summary['exe_sha256'],'verified_members':checks,'extracted_package_launch':'passed','extracted_prompt_copy_and_save':'passed','extracted_keep_prompt_process':'passed','snapshot':str(snapshot.relative_to(ROOT))}
(ROOT/'bench/results/package.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
