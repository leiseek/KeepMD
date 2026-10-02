"""Reader dispatch-to-draw measurements; does not claim physical display frame times."""
import json
import math
import subprocess
import time
import psutil
from test_gui import ROOT,OUT,u,windows,children,name,wait

results=[]
for fixture in ('s100','l10','images100','flow200'):
    config=OUT/'scroll.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
    report=OUT/f'scroll-{fixture}.json'
    process=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(ROOT/f'bench/corpus/{fixture}.md'),'--config',str(config),'--report',str(report)],cwd=ROOT)
    try:
        hwnd=wait(lambda:(windows(process.pid)or[None])[0]);wait(lambda:fixture+'.md' in name(hwnd))
        reader=next(h for h in children(hwnd)if name(h,True)=='KeepMD.Reader')
        time.sleep(.4)
        for frame in range(240):
            tick=time.perf_counter();direction=0x28 if (frame//60)%2==0 else 0x26
            u.PostMessageW(reader,0x100,direction,0)
            time.sleep(max(0,1/60-(time.perf_counter()-tick)))
        time.sleep(.2)
        p=psutil.Process(process.pid);before=p.cpu_times();t0=time.monotonic();time.sleep(3);after=p.cpu_times()
        idle_time=time.monotonic()-t0;idle_cpu=after.user+after.system-before.user-before.system
        u.SendMessageW(hwnd,0x8000+100,0,0)
        stats=json.loads(report.read_text());samples=stats['scroll_samples']
        assert len(samples)>=200
        def percentile(key,p):return sorted(s[key]for s in samples)[min(len(samples)-1,math.ceil(len(samples)*p)-1)]
        result={'fixture':fixture,'samples':len(samples),'draw_p95_ms':percentile('draw_ms',.95),'draw_p99_ms':percentile('draw_ms',.99),'dispatch_to_submit_p95_ms':percentile('dispatch_ms',.95),'dispatch_to_submit_p99_ms':percentile('dispatch_ms',.99),'idle_cpu_seconds':idle_cpu,'idle_observation_seconds':idle_time,'private_mib':stats['private_bytes']/1048576}
        results.append(result);print(result,flush=True)
        u.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=5)
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)
summary={'scope':'Application timing from delivered scroll event through native draw submission. Not hardware input latency or physical monitor frame interval. 60 events/second target.', 'records':results}
(ROOT/'bench/results/scroll.json').write_text(json.dumps(summary,indent=2)+'\n')
