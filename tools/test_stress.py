"""Bounded real-window media/large-document/refresh checks."""
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import subprocess
import time
import psutil
from test_gui import ROOT,OUT,u,windows,children,name,wait,screenshot,clipboard

u.SendMessageTimeoutW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM,W.UINT,W.UINT,C.POINTER(C.c_size_t)]
u.SendMessageTimeoutW.restype=W.LPARAM
def send(hwnd,message,w=0,l=0):
    value=C.c_size_t()
    if not u.SendMessageTimeoutW(hwnd,message,w,l,2,2000,C.byref(value)):
        raise AssertionError('Window did not respond within 2 seconds')
    return value.value

results=[]
for fixture in ('s100','l10','long-paragraph','long-code','oversized-diagram','table5000','images100','flow200','flow1000'):
    config=OUT/'stress.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
    report=OUT/f'stress-{fixture}.json'
    process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(ROOT/f'bench/corpus/{fixture}.md'),'--config',str(config),'--report',str(report)],cwd=ROOT)
    peak=0
    try:
        hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:any(name(h,True)=='KeepMD.Reader' for h in children(hwnd)));child=children(hwnd)
        reader=next(h for h in child if name(h,True)=='KeepMD.Reader')
        status=next(h for h in child if name(h,True)=='msctls_statusbar32')
        wait(lambda:name(status).startswith('阅读'),timeout=10)
        p=psutil.Process(process.pid)
        for step in range(35):
            send(reader,0x100,0x22,0)  # Page Down exercises new blocks/resources.
            time.sleep(.04)
            peak=max(peak,p.memory_info().private)
            if peak>256*1048576:raise AssertionError('256 MiB stress budget exceeded')
        send(reader,0x100,0x23,0)
        time.sleep(.3)
        send(reader,0x100,0x24,0)
        time.sleep(.3)
        if fixture in ('images100','flow200','flow1000'):screenshot(hwnd,f'stress-{fixture}.png')
        if fixture in ('images100','flow200'):
            for _ in range(3):send(hwnd,0x8000+101);time.sleep(.3)
            send(hwnd,0x8000+100)
            assert json.loads(report.read_text())['asset_bytes']>0,'Resources did not recover after target reset'
        send(hwnd,0x8000+100)
        paint_before=json.loads(report.read_text())['paint_count']
        time.sleep(.5)
        send(hwnd,0x8000+100)
        idle_paints=json.loads(report.read_text())['paint_count']-paint_before
        assert idle_paints<=1, f'{fixture}: unexpected idle redraw loop ({idle_paints} paints)'
        if fixture=='s100':
            before=p.cpu_times();time.sleep(5);after=p.cpu_times()
            idle=(after.user+after.system)-(before.user+before.system)
        else:idle=None
        send(hwnd,0x10)
        process.wait(timeout=5)
        metrics=json.loads(report.read_text())
        assert metrics['layouts']<=128
        assert metrics['asset_bytes']<=32*1048576
        result=dict(fixture=fixture,peak_mib=peak/1048576,idle_cpu_seconds_over_5s=idle,idle_paints_over_half_second=idle_paints,metrics=metrics)
        results.append(result);print(f'{fixture}: peak={peak/1048576:.1f} MiB, layouts={metrics["layouts"]}, assets={metrics["asset_bytes"]}',flush=True)
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)

# Work only on generated documents when testing external changes.
path=OUT/'refresh 中文.md';path.write_text('# Initial\n\nBefore refresh.\n',encoding='utf-8')
process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(path),'--config',str(OUT/'stress.ini')],cwd=ROOT)
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);time.sleep(.3)
    path.write_text('# Updated\n\n中文更新成功 😀\n',encoding='utf-8')
    def copied_update():
        send(hwnd,0x111,111);send(hwnd,0x111,110)
        return '中文更新成功' in clipboard()
    wait(copied_update,timeout=5)
    send(hwnd,0x10);process.wait(timeout=5)
    results.append({'external_refresh':'passed','unicode_path':'passed'})
finally:
    if process.poll() is None:process.terminate();process.wait(timeout=5)
(ROOT/'bench/results/stress.json').write_text(json.dumps(results,indent=2)+'\n')
print('Stress and external-refresh checks passed.')
