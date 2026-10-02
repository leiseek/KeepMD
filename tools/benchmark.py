"""Repeat new-process, warm-file-cache measurements; never labels these cold starts."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import time
import ctypes
import hashlib
import psutil

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--runs',type=int,default=30)
parser.add_argument('--backend',choices=['software','hardware'],default='software')
parser.add_argument('--fixture',default='s100')
args=parser.parse_args()
out=ROOT/'bench/results/raw'
out.mkdir(parents=True,exist_ok=True)
records=[]
env=os.environ.copy();env['KEEPMD_RENDERER']=args.backend
config=out/'benchmark.ini';config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
for run in range(args.runs):
    report=out/f'{args.backend}-{args.fixture}-{run:02}.json'
    start=time.perf_counter()
    stamp=ctypes.c_longlong();ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(stamp))
    proc=subprocess.Popen([str(ROOT/'build/release/keepmd.exe'),str(ROOT/f'bench/corpus/{args.fixture}.md'),'--report',str(report),'--config',str(config),'--exit-after','500'],cwd=ROOT,env=env)
    try:
        while proc.poll() is None:
            if time.perf_counter()-start>30:raise RuntimeError('Benchmark exceeded 30 seconds')
            try:
                if psutil.Process(proc.pid).memory_info().private>768*1048576:raise RuntimeError('Benchmark exceeded 768 MiB memory guard')
            except psutil.NoSuchProcess:pass
            time.sleep(.02)
    finally:
        if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
    if proc.returncode or not report.exists():raise RuntimeError(f'run {run} failed')
    record=json.loads(report.read_text());record['process_wall_ms']=(time.perf_counter()-start)*1000
    if record.get('first_paint_qpc'):
        record['launch_to_first_paint_ms']=(record['first_paint_qpc']-stamp.value)*1000/record['qpc_frequency']
    records.append(record)
def percentile(key,p):
    ordered=sorted(r[key] for r in records)
    return ordered[min(len(ordered)-1,__import__('math').ceil(len(ordered)*p)-1)]
summary={'backend':args.backend,'fixture':args.fixture,'runs':args.runs,'cache':'warm file cache; new processes; interactive desktop',
         'exe_sha256':hashlib.sha256((ROOT/'build/release/keepmd.exe').read_bytes()).hexdigest(),'memory_guard_mib':768,
         'startup_scope':'internal process initialization to submitted document first paint; excludes OS process creation',
         'first_paint_p50_ms':statistics.median(r['first_paint_ms'] for r in records),
         'first_paint_p95_ms':percentile('first_paint_ms',.95),'first_paint_max_ms':max(r['first_paint_ms'] for r in records),
         'private_p50_mib':statistics.median(r['private_bytes'] for r in records)/1048576,
         'private_max_mib':max(r['private_bytes'] for r in records)/1048576,
         'records':records}
destination=ROOT/f'bench/results/{args.backend}-{args.fixture}.json'
if records[0].get('launch_to_first_paint_ms'):
    summary['launch_to_first_paint_p50_ms']=statistics.median(r['launch_to_first_paint_ms'] for r in records)
    summary['launch_to_first_paint_p95_ms']=percentile('launch_to_first_paint_ms',.95)
destination.write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='records'},indent=2))
