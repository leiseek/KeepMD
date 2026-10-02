"""Additional release checks for rendered class styles, image formats and standalone Mermaid."""
import ctypes as C
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import time
from PIL import Image
from test_gui import ROOT,OUT,u,windows,name,wait,activate

parser=argparse.ArgumentParser();parser.add_argument('--exe',default=str(ROOT/'build/release/keepmd.exe'));args=parser.parse_args()
exe=Path(args.exe).resolve();records=[]
image=Image.new('RGB',(120,80),(210,70,30))
for extension in ('png','jpg','bmp'):image.save(OUT/f'media.{extension}')
media=OUT/'media-formats.md';media.write_text('# Media\n\n'+''.join(f'![{ext}](media.{ext})\n\n'for ext in ('png','jpg','bmp')),encoding='utf-8')
mmd=OUT/'standalone.MMD';mmd.write_text('flowchart LR\nA[独立文件]-->B[完成]\n',encoding='utf-8')
for label,path in [('style',ROOT/'tests/fixtures/flow-details.md'),('media',media),('mmd',mmd)]:
    report=OUT/f'release-{label}.json';picture=OUT/f'release-{label}.png';config=OUT/'release-smoke.ini'
    config.write_text('[Reader]\nDark=0\n',encoding='utf-8')
    process=subprocess.Popen([str(exe),str(path),'--config',str(config),'--report',str(report),'--snapshot',str(picture),'--exit-after','800'],cwd=ROOT)
    try:
        hwnd=wait(lambda:(windows(process.pid)or[None])[0]);activate(hwnd)
        process.wait(timeout=10);assert process.returncode==0
    finally:
        if process.poll()is None:process.terminate();process.wait(timeout=5)
    metrics=json.loads(report.read_text());assert metrics['asset_bytes']>0
    pixels=Image.open(picture).convert('RGB')
    if label=='style':
        green=sum(1 for r,g,b in pixels.get_flattened_data() if abs(r-224)<5 and abs(g-245)<5 and abs(b-238)<5)
        assert green>500,f'Expected actual classDef fill, found {green} pixels'
        records.append({'classDef_rendered_pixels':green,'shape_and_group_snapshot':picture.name})
    elif label=='media':
        orange=sum(1 for r,g,b in pixels.get_flattened_data()if abs(r-210)<5 and abs(g-70)<5 and abs(b-30)<5)
        assert orange>24000,f'Not all three image formats visible: {orange}'
        records.append({'PNG_JPEG_BMP_rendered_pixels':orange,'asset_bytes':metrics['asset_bytes']})
    else:records.append({'uppercase_MMD_native_render':'passed','asset_bytes':metrics['asset_bytes']})
result={'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'checks':records}
(ROOT/'bench/results/release-smoke.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
