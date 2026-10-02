"""Unmodified Tinta reference: memory and idle CPU, no unsupported first-paint claims."""
from pathlib import Path
import ctypes as C
from ctypes import wintypes as W
import json
import subprocess
import time
import threading
import psutil
from PIL import ImageGrab

ROOT=Path(__file__).resolve().parents[1]
u=C.windll.user32
u.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
u.SendMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
u.GetClassNameW.argtypes=[W.HWND,W.LPWSTR,C.c_int]
u.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
u.GetWindowRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
callback=C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
exe=ROOT/'.cache/tinta-build/tinta.exe'
results=[]
def persist():
    report={'commit':'db70698e49a1f700c7ad98add1bebe713ac796bd','build':'Release x64 unchanged source',
        'scope':'single exploratory run per fixture; 512 MiB process stop limit; fixed settle windows; startup not measured; not a timing ranking',
        'earlier_probe':'An initial unbounded L10 probe was stopped manually after >10 GiB Private Bytes; excluded from ordinary metrics.',
        'records':results}
    (ROOT/'bench/results/tinta-reference.json').write_text(json.dumps(report,indent=2)+'\n')
    return report
for fixture in ('s100','l10','images100'):
    (exe.parent/'settings.ini').write_text('[Settings]\nhasAskedFileAssociation=1\ncheckUpdates=0\nopenInTabs=0\nwindowWidth=1060\nwindowHeight=820\nlanguage=en\n',encoding='utf-8')
    process=subprocess.Popen([str(exe),str(ROOT/f'bench/corpus/{fixture}.md')],cwd=exe.parent)
    stop=threading.Event();samples=[];reason=[]
    def monitor():
        p=psutil.Process(process.pid)
        while not stop.wait(.05):
            try:
                private=p.memory_info().private;samples.append(private)
                if private>512*1048576:
                    reason.append('512 MiB process memory budget exceeded');p.terminate();return
            except psutil.NoSuchProcess:return
    watcher=threading.Thread(target=monitor,daemon=True);watcher.start()
    try:
        handle=[]
        def collect(hwnd,_):
            pid=W.DWORD();u.GetWindowThreadProcessId(hwnd,C.byref(pid))
            cls=C.create_unicode_buffer(100);u.GetClassNameW(hwnd,cls,100)
            if pid.value==process.pid and cls.value=='Tinta':handle.append(hwnd)
            return True
        until=time.monotonic()+10
        while not handle and time.monotonic()<until:
            cb=callback(collect);u.EnumWindows(cb,0);time.sleep(.05)
        if not handle:raise RuntimeError('Reference window not found')
        p=psutil.Process(process.pid)
        time.sleep(3)
        top=p.memory_info()
        cpu=p.cpu_times();t0=time.monotonic();time.sleep(3);cpu2=p.cpu_times()
        u.PostMessageW(handle[0],0x100,0x23,0)
        time.sleep(2)
        end=p.memory_info()
        rect=W.RECT();u.GetWindowRect(handle[0],C.byref(rect))
        ImageGrab.grab(bbox=(rect.left,rect.top,rect.right,rect.bottom)).save(ROOT/f'tests/output/tinta-{fixture}.png')
        results.append(dict(fixture=fixture,private_top_mib=top.private/1048576,private_end_mib=end.private/1048576,
                            sampled_peak_mib=max(samples,default=0)/1048576,idle_cpu_seconds=(cpu2.user+cpu2.system)-(cpu.user+cpu.system),idle_window_seconds=time.monotonic()-t0-2))
        u.PostMessageW(handle[0],0x10,0,0);process.wait(timeout=10)
    except Exception as error:
        results.append(dict(fixture=fixture,stopped=reason or [str(error)],sampled_peak_mib=max(samples,default=0)/1048576))
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)
        stop.set();watcher.join(timeout=2)
        persist()
        print(f'{fixture}: {results[-1]}',flush=True)
print(json.dumps(persist(),indent=2))
