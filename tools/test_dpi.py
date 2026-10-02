"""Native reader target-DPI matrix on one desktop; does not alter system display settings."""
import ctypes as C
from ctypes import wintypes as W
import json
import subprocess
import time
from test_gui import ROOT,OUT,u,windows,children,name,wait,screenshot,clipboard,click

u.GetClientRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
source=OUT/'dpi-reader.md';nextfile=OUT/'dpi-next.md'
source.write_text('# DPI 中文\n\n[Next](dpi-next.md)\n\n中文与 **粗体**、组合字符 é 和 emoji 👩‍💻 🇨🇳。\n\n```mermaid\ngraph LR\nA[开始] --> B{判断}\nB -->|是| C[完成]\n```\n\n| Column | 中文 |\n|:--|--:|\n|Value|内容|\n',encoding='utf-8')
nextfile.write_text('# Next\n',encoding='utf-8')
results=[]
for dpi in (96,120,144,192):
    config=OUT/'dpi.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
    report=OUT/f'dpi-{dpi}.json'
    process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(source),'--config',str(config),'--report',str(report),'--render-dpi',str(dpi)],cwd=ROOT)
    try:
        hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:source.name in name(hwnd))
        reader=next(h for h in children(hwnd)if name(h,True)=='KeepMD.Reader')
        def stats():u.SendMessageW(hwnd,0x8000+100,0,0);return json.loads(report.read_text())
        wait(lambda:stats()['asset_bytes']>0)
        screenshot(hwnd,f'dpi-{dpi}-light.png')
        u.SendMessageW(hwnd,0x111,111,0);u.SendMessageW(hwnd,0x111,110,0)
        assert '👩‍💻 🇨🇳' in clipboard() and 'é' in clipboard()
        u.SendMessageW(hwnd,0x111,106,0)
        screenshot(hwnd,f'dpi-{dpi}-dark.png')
        rect=W.RECT();u.GetClientRect(reader,C.byref(rect));scale=dpi/96
        logical=rect.right/scale;content=max(180,min(logical-64,920));left=max(24,(logical-content)*.5)
        click(reader,int((left+12)*scale),int(110*scale))
        wait(lambda:nextfile.name in name(hwnd))
        u.SendMessageW(hwnd,0x111,114,0);wait(lambda:source.name in name(hwnd))
        assert stats()['render_dpi']==dpi
        results.append({'dpi':dpi,'text_copy_and_emoji':'passed','link_hit_testing':'passed','light_dark_snapshots':'captured'})
        u.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=5)
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)
result={'method':'Direct2D reader target DPI explicitly set to 96/120/144/192 on a 96-DPI desktop. Tests rendering and input coordinate conversion; physical cross-monitor transitions not exercised.','records':results}
(ROOT/'bench/results/dpi-e2e.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
