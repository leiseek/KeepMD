"""Render native HTML images and README HTML; check pixels, sizes, alignment, saved bytes and external READMEs."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess
from PIL import Image, ImageChops
from test_gui import ROOT, OUT, u, windows, children, name, wait, activate

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, default=ROOT / 'build/release/keepmd.exe')
parser.add_argument('--ghidra-readme', type=Path)
parser.add_argument('--typewords-readme', type=Path)
args = parser.parse_args()
exe = args.exe.resolve()
checks = []
color = (211, 47, 131)
media = OUT / 'html 图片 & sample.png'
Image.new('RGB', (240, 120), color).save(media)
source = 'html%20图片%20&amp;%20sample.png'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def color_box(image):
    rgb = image.convert('RGB')
    channels = rgb.split()
    mask = channels[0].point(lambda v: 255 if abs(v - color[0]) < 4 else 0)
    for channel, value in zip(channels[1:], color[1:]):
        mask = ImageChops.multiply(mask, channel.point(lambda v: 255 if abs(v - value) < 4 else 0))
    return mask.getbbox()


def render(label, path, zoom=1000, split=False, save=False, update=False):
    report = OUT / f'html-{label}.json'
    picture = OUT / f'html-{label}.png'
    config = OUT / f'html-{label}.ini'
    config.write_text(f'[Reader]\nVisual=0\nDark=0\nZoom={zoom}\n', encoding='utf-8')
    process = subprocess.Popen([str(exe), str(path), '--config', str(config), '--report', str(report),
                                '--snapshot', str(picture), '--exit-after', '1800'], cwd=ROOT)
    try:
        hwnd = wait(lambda: (windows(process.pid) or [None])[0])
        wait(lambda: path.name in name(hwnd))
        activate(hwnd)
        if split or save:
            u.SendMessageW(hwnd, 0x111, 117, 0)
            editor = wait(lambda: next((h for h in children(hwnd) if name(h, True) == 'RICHEDIT50W'), None))
            if save:
                original = path.read_bytes()
                u.SendMessageW(hwnd, 0x111, 118, 0)
                assert path.read_bytes() == original, 'Unchanged save rewrote HTML source'
            # Entering the source editor starts in split mode by default.
            if update:
                replacement = path.read_text(encoding='utf-8').replace('width=120', 'width=80')
                text = C.create_unicode_buffer(replacement)
                u.SendMessageW(editor, 0x00B1, 0, -1)
                u.SendMessageW(editor, 0x00C2, 1, C.cast(text, C.c_void_p).value)
                u.SendMessageW(hwnd, 0x111, 118, 0)
                assert path.read_text(encoding='utf-8') == replacement
        process.wait(timeout=12)
        assert process.returncode == 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
    return Image.open(picture).convert('RGB'), json.loads(report.read_text()), picture


cases = [
    ('width', f'<img src="{source}" width="120">', (120, 60), 1000),
    ('height', f"<img src='{source}' height='30'>", (60, 30), 1000),
    ('both', f'<IMG\nSRC="{source}" WIDTH=90 HEIGHT=30 />', (90, 30), 1000),
    ('zoom', f'<img src="{source}" width=120>', (180, 90), 1500),
    ('invalid-size', f'<img src="{source}" width="99999999999999">', (240, 120), 1000),
    ('inline', f'Before **bold** <img src="{source}" width=100> after *italic*.', (100, 50), 1000),
    ('markdown', f'![ordinary]({media.name.replace(" ", "%20")})', (240, 120), 1000),
    ('split', f'<img src="{source}" width=120>', (120, 60), 1000),
    ('live-size-edit', f'<img src="{source}" width=120>', (80, 40), 1000),
    ('fit', f'<img src="{source}" width=1840 height=184>', (920, 92), 1000),
    ('wrapper', f'<div><img src="{source}" width=120></div>', (120, 60), 1000),
    ('center', f'<p align="center">\n  <a href="x.md"><img src="{source}" width="120"></a>\n</p>', (120, 60), 1000),
    ('center-div', f'<div align="center">\n\n![logo]({media.name.replace(" ", "%20")})\n\n</div>', (240, 120), 1000),
]
# A leading "/" names the repository root, as on GitHub, not the drive root.
repo = OUT / 'html-repo'
(repo / '.git').mkdir(parents=True, exist_ok=True)
(repo / 'public').mkdir(exist_ok=True)
(repo / 'docs').mkdir(exist_ok=True)
Image.new('RGB', (240, 120), color).save(repo / 'public' / 'root image.png')
cases.append(('root', '<p align="right"><img src="/public/root%20image.png" width="120"></p>', (120, 60), 1000))
for label, tag, expected, zoom in cases:
    path = (repo / 'docs' if label == 'root' else OUT) / f'html-{label}.md'
    path.write_bytes((tag + '\r\n\r\n# Following heading\r\n\r\n中文正文。\r\n').encode('utf-8'))
    pixels, metrics, picture = render(label, path, zoom, split=label in ('split', 'live-size-edit'),
                                      save=label == 'split', update=label == 'live-size-edit')
    box = color_box(pixels)
    assert box and (abs(box[2] - box[0] - expected[0]) <= 1 and abs(box[3] - box[1] - expected[1]) <= 1), (label, box, expected)
    assert metrics['asset_bytes'] > 0
    check = {'case': label, 'rendered_size': [box[2] - box[0], box[3] - box[1]], 'expected': expected}
    if label.startswith('center'):
        # The reading column is centred in the view, so a centred image shares its midpoint.
        assert abs((box[0] + box[2]) / 2 - pixels.width / 2) <= 2, (label, box, pixels.width)
        check['center_offset'] = (box[0] + box[2]) / 2 - pixels.width / 2
    if label == 'root':
        column = min(pixels.width - 64, 920)
        assert abs(box[2] - (max(24, (pixels.width - column) / 2) + column)) <= 2, (box, pixels.width)
        check['right_edge'] = box[2]
    checks.append(check)

path = OUT / 'html-fallback.md'
path.write_text(f'```html\n<img src="{source}">\n```\n\n`<img src="{source}">`\n\n'
                f'<table><tr><td><img src="{source}"></td></tr></table>\n\n<img src="missing.png">\n\n'
                '<img src="https://example.invalid/test.png">\n\n<img src="//example.invalid/test.png">\n', encoding='utf-8')
pixels, metrics, _ = render('fallback', path)
assert color_box(pixels) is None and metrics['asset_bytes'] == 0
checks.append({'case': 'code/table/missing/remote fallbacks', 'asset_bytes': metrics['asset_bytes']})

# The supplied repository is read-only input. Nothing is copied to the release.
if args.ghidra_readme:
    readme = args.ghidra_readme.resolve()
    logo = readme.parent / 'Ghidra/Features/Base/src/main/resources/images/GHIDRA_3.png'
    before = [digest(readme), digest(logo)]
    pixels, metrics, picture = render('ghidra-readme', readme)
    assert [digest(readme), digest(logo)] == before
    assert metrics['asset_bytes'] >= 1164 * 265 * 4
    # Locate the first image's non-background pixels, above the following heading.
    background = Image.new('RGB', pixels.size, pixels.getpixel((0, 0)))
    top = ImageChops.difference(pixels, background).crop((0, 0, pixels.width, 120))
    box = top.getbbox()
    assert box and 390 <= box[2] - box[0] <= 402, box
    assert 85 <= box[3] - box[1] <= 93, box
    checks.append({'case': 'external Ghidra README', 'document_sha256': before[0],
                   'logo_sha256': before[1], 'source_unchanged': True, 'logo_pixel_bounds': box,
                   'snapshot': str(picture.relative_to(ROOT))})

# A README whose header is centred HTML and whose screenshots use repository-root paths.
if args.typewords_readme:
    readme = args.typewords_readme.resolve()
    shot = readme.parent / 'public/imgs/words.png'
    before = [digest(readme), digest(shot)]
    pixels, metrics, picture = render('typewords-readme', readme)
    assert [digest(readme), digest(shot)] == before
    assert metrics['asset_bytes'] > 0, 'root-relative screenshot did not load'
    # Raw HTML would be drawn as source lines on the code background.
    header = pixels.crop((0, 0, pixels.width, 220))
    code = sum(1 for value in header.getdata() if value == (0xf2, 0xf5, 0xf8))
    assert code < 50, code
    checks.append({'case': 'external TypeWords README', 'document_sha256': before[0],
                   'screenshot_sha256': before[1], 'source_unchanged': True, 'header_code_pixels': code,
                   'asset_bytes': metrics['asset_bytes'], 'snapshot': str(picture.relative_to(ROOT))})

result = {'exe_sha256': digest(exe), 'checks': checks}
(ROOT / 'bench/results/html-images-e2e.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, ensure_ascii=False, indent=2))
