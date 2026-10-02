"""Check every packaged checksum and launch the EXE extracted from the actual ZIP."""
from pathlib import Path
import hashlib
import json
import subprocess
import zipfile
from test_gui import ROOT,OUT,windows,wait,activate

archive=ROOT/'dist/KeepMD-0.2.0-windows-x64.zip'
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
report=OUT/'packaged.json';snapshot=OUT/'packaged-reader.png';config=OUT/'packaged.ini'
config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
process=subprocess.Popen([str(destination/'keepmd.exe'),str(destination/'examples/欢迎.md'),'--config',str(config),'--report',str(report),'--snapshot',str(snapshot),'--exit-after','900'],cwd=destination)
try:
    hwnd=wait(lambda:(windows(process.pid)or[None])[0]);activate(hwnd);process.wait(timeout=10);assert process.returncode==0
finally:
    if process.poll()is None:process.terminate();process.wait(timeout=5)
metrics=json.loads(report.read_text());assert metrics['blocks']>=15 and metrics['first_paint_ms']>0
assert '../使用说明.md' in (destination/'examples/欢迎.md').read_text(encoding='utf-8-sig')
result={'zip_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'exe_sha256':summary['exe_sha256'],'verified_members':checks,'extracted_package_launch':'passed','snapshot':str(snapshot.relative_to(ROOT))}
(ROOT/'bench/results/package.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
