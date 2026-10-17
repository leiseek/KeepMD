"""Fetch pinned native source dependencies; no runtime network dependency."""
from pathlib import Path
import concurrent.futures
import hashlib
import json
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MD4C = '729e6b8b320caa96328968ab27d7db2235e4fb47'
TINTA = 'db70698e49a1f700c7ad98add1bebe713ac796bd'
MICROTEX = '0e3707f6dafebb121d98b53c64364d16fefe481d'
TINYXML2 = '9148bdf719e997d1f474be6bcc7943881046dba1'  # 11.0.0
MATPLOTLIB = 'fed55c63a314351cd39a12783f385009782c06e1'  # Only for the BaKoMa font licence text it carries.
# Fonts MicroTeX ships whose licence or provenance is unclear: JLaTeXMath's licence lists neither its own
# Latin text fonts nor special.ttf, and special.ttf has glyphs of unknown origin (MicroTeX issue #85). KeepMD
# draws their few glyphs as Unicode characters from Windows fonts instead (src/keepmd/math_formula.cpp).
MICROTEX_EXCLUDED = {'res/fonts/maths/special.ttf'} | {
    f'res/fonts/latin/{name}.ttf' for name in ('bi10', 'bx10', 'i10', 'r10', 'sb10', 'sbi10', 'si10', 'ss10', 'tt10')}
SOURCES = [
    ('mity/md4c', MD4C, f'src/{name}', f'third_party/md4c/{name}')
    for name in ('md4c.c', 'md4c.h', 'entity.c', 'entity.h')
] + [('mity/md4c', MD4C, 'LICENSE.md', 'third_party/md4c/LICENSE.md')] + [
    ('oipoistar/tinta', TINTA, src, f'third_party/tinta/{dest}')
    for src, dest in (
        ('include/mermaid.h', 'mermaid.h'), ('src/mermaid.cpp', 'mermaid.cpp'),
        ('LICENSE', 'LICENSE'), ('tests/mermaid_tests.cpp', 'upstream_mermaid_tests.cpp'))
] + [
    ('leethomason/tinyxml2', TINYXML2, name, f'third_party/tinyxml2/{name}')
    for name in ('tinyxml2.h', 'tinyxml2.cpp', 'LICENSE.txt')
] + [('matplotlib/matplotlib', MATPLOTLIB, 'LICENSE/LICENSE_BAKOMA', 'third_party/microtex/LICENSE_BAKOMA')]


def microtex_sources():
    """MicroTeX's math layout core and the Computer Modern fonts it measures against.

    Platform backends, samples, the Greek/Cyrillic text alphabets and MICROTEX_EXCLUDED are left out: KeepMD
    supplies its own Direct2D backend and only typesets the built-in Latin and math fonts. The file list is read from the
    pinned commit's tree, so it is as reproducible as the commit itself."""
    url = f'https://api.github.com/repos/NanoMichael/MicroTeX/git/trees/{MICROTEX}?recursive=1'
    tree = json.loads(urllib.request.urlopen(url, timeout=30).read())['tree']
    keep = lambda path: path == 'LICENSE' or (path.startswith('res/fonts/') and path not in MICROTEX_EXCLUDED) or (
        path.startswith('src/') and path.endswith(('.cpp', '.h'))
        and not path.startswith(('src/platform/', 'src/samples/')))
    return [('NanoMichael/MicroTeX', MICROTEX, item['path'], f"third_party/microtex/{item['path']}")
            for item in tree if item['type'] == 'blob' and keep(item['path'])]

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
        manifest = list(pool.map(fetch, SOURCES + microtex_sources()))
    (ROOT / 'third_party/manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(f'Fetched {len(manifest)} pinned source files.')
