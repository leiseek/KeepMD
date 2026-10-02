"""Fetch pinned native source dependencies; no runtime network dependency."""
from pathlib import Path
import concurrent.futures
import hashlib
import json
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MD4C = '729e6b8b320caa96328968ab27d7db2235e4fb47'
TINTA = 'db70698e49a1f700c7ad98add1bebe713ac796bd'
SOURCES = [
    ('mity/md4c', MD4C, f'src/{name}', f'third_party/md4c/{name}')
    for name in ('md4c.c', 'md4c.h', 'entity.c', 'entity.h')
] + [('mity/md4c', MD4C, 'LICENSE.md', 'third_party/md4c/LICENSE.md')] + [
    ('oipoistar/tinta', TINTA, src, f'third_party/tinta/{dest}')
    for src, dest in (
        ('include/mermaid.h', 'mermaid.h'), ('src/mermaid.cpp', 'mermaid.cpp'),
        ('LICENSE', 'LICENSE'), ('tests/mermaid_tests.cpp', 'upstream_mermaid_tests.cpp'))
]

def fetch(item):
    repo, rev, src, target = item
    url = f'https://raw.githubusercontent.com/{repo}/{rev}/{src}'
    path = ROOT / target
    path.parent.mkdir(parents=True, exist_ok=True)
    data = urllib.request.urlopen(url, timeout=30).read()
    path.write_bytes(data)
    return dict(path=target, source=url, sha256=hashlib.sha256(data).hexdigest())

if __name__ == '__main__':
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        manifest = list(pool.map(fetch, SOURCES))
    (ROOT / 'third_party/manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(f'Fetched {len(manifest)} pinned source files.')
