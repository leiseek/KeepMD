"""30-second idle observation and best-effort per-process GPU counters."""
import json
import hashlib
import subprocess
import time
import psutil
from test_gui import ROOT,OUT,u,windows,name,wait

report=OUT/'idle.json';config=OUT/'idle.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(ROOT/'bench/corpus/s100.md'),'--report',str(report),'--config',str(config)],cwd=ROOT)
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:'s100.md' in name(hwnd));time.sleep(.5)
    p=psutil.Process(process.pid)
    u.SendMessageW(hwnd,0x8000+100,0,0);initial=json.loads(report.read_text())
    before=p.cpu_times();start=time.monotonic();time.sleep(30);after=p.cpu_times();elapsed=time.monotonic()-start
    u.SendMessageW(hwnd,0x8000+100,0,0);final=json.loads(report.read_text())
    assert final['paint_count']-initial['paint_count']<=1
    query="$ErrorActionPreference='Stop'; $r=Get-Counter -Counter '\\GPU Process Memory(*)\\Dedicated Usage','\\GPU Process Memory(*)\\Shared Usage'; @($r.CounterSamples | Where-Object {$_.InstanceName -like 'pid_%d_*'} | Select-Object InstanceName,Path,CookedValue) | ConvertTo-Json -Compress"%process.pid
    gpu=subprocess.run(['powershell','-NoProfile','-Command',query],capture_output=True,text=True,timeout=15)
    try:gpu_rows=json.loads(gpu.stdout)if gpu.returncode==0 and gpu.stdout.strip() else []
    except json.JSONDecodeError:gpu_rows=[]
    result={'exe_sha256':hashlib.sha256((ROOT/'build/release/keepmd.exe').read_bytes()).hexdigest(),'seconds':elapsed,'cpu_seconds_delta':after.user+after.system-before.user-before.system,'paint_count_delta':final['paint_count']-initial['paint_count'],'private_bytes':final['private_bytes'],'working_set':final['working_set'],'gpu_process_counter_rows':gpu_rows,'gpu_note':'An empty list means no matching provider instance/readable counter; it is not a claim of zero system compositor GPU use.'}
    (ROOT/'bench/results/idle.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    u.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=5)
finally:
    if process.poll()is None:process.terminate();process.wait(timeout=5)
