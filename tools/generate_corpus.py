"""Deterministic benchmark and compatibility documents, generated outside source control."""
from pathlib import Path
from PIL import Image, ImageDraw
import hashlib
import json
ROOT=Path(__file__).resolve().parents[1]
CORPUS=ROOT/'bench/corpus'
CORPUS.mkdir(parents=True,exist_ok=True)
paragraph='中文阅读性能测试。KeepMD renders text using native Windows APIs. **重点** 与 `code`。\n\n'
for name,limit in [('s100',100*1024),('m1',1024*1024),('l10',10*1024*1024),('l50',50*1024*1024)]:
    chunk=('## Document section\n\n'+paragraph*10).encode()
    content=b'# KeepMD benchmark\n\n'+chunk*(limit//len(chunk))
    (CORPUS/f'{name}.md').write_bytes(content)
(CORPUS/'long-paragraph.md').write_text('# Long paragraph\n\n'+paragraph.replace('\n',' ')*10000,encoding='utf-8')
(CORPUS/'long-code.md').write_text('# Long code line\n\n```text\n'+'x'*1000000+'\n```\n',encoding='utf-8')
(CORPUS/'oversized-diagram.md').write_text('# Oversized diagram\n\n```mermaid\ngraph LR\n%%'+'x'*1000000+'\nA-->B\n```\n',encoding='utf-8')
(CORPUS/'table5000.md').write_text('# Large table\n\n| Index | 中文 | Value |\n|--:|:--|:--|\n'+''.join(f'|{i}|内容{i}|value {i}|\n' for i in range(5000)),encoding='utf-8')
for count in (50,200,1000):
    (CORPUS/f'flow{count}.md').write_text('# Flow stress\n\n```mermaid\nflowchart TD\n'+''.join(f'N{i}[节点 {i}] --> N{i+1}[节点 {i+1}]\n' for i in range(count-1))+'```\n',encoding='utf-8')
image=Image.new('RGB',(3200,1800),'#e5eff9');draw=ImageDraw.Draw(image)
for i in range(16):draw.rectangle((i*200,0,i*200+80,1800),fill=(35+i*8,90+i*5,130+i*5))
image.save(CORPUS/'fixture.png')
(CORPUS/'images100.md').write_text('# Images\n\n'+''.join(f'## Image {i}\n\n![sample {i}](fixture.png)\n\n' for i in range(100)),encoding='utf-8')
manifest={p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in CORPUS.iterdir() if p.is_file() and p.name!='manifest.json'}
(CORPUS/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
diagrams=['# Mermaid flowchart compatibility\n\nNative rendering examples.\n']
for direction in ('TD','TB','BT','LR','RL'):
    for shape,node in [('rectangle','A[开始]'),('round','A(开始)'),('decision','A{判断}'),('circle','A((圆形))'),('stadium','A([结束])'),('quoted','A["中文 label"]')]:
        diagrams.append(f'## {direction} / {shape}\n\n```mermaid\nflowchart {direction}\n{node} -->|标签| B[完成]\n```\n')
diagrams.append('## Cycle\n\n```mermaid\ngraph TD\nA[开始] --> B{判断}\nB -->|继续| A\nB -->|结束| C[结束]\n```\n')
diagrams.append('## Unsupported syntax\n\n```mermaid\nflowchart TD\nA@{ shape: rounded } --> B\n```\n')
(ROOT/'tests/fixtures/flowcharts.md').write_text('\n'.join(diagrams),encoding='utf-8')
print(f'Generated {len(manifest)} fixtures in {CORPUS}')
