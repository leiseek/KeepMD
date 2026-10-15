"""Document-wide network image switch and persistent URL cache, using a private loopback server."""
import argparse
import ctypes as C
from ctypes import wintypes as W
from collections import Counter
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import tempfile
from PIL import Image
from capture_window import capture
from test_gui import ROOT, OUT, u, windows, children, name, wait, CALLBACK

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, default=ROOT/'build/release/keepmd.exe')
args = parser.parse_args()
folder = OUT/'remote-images'
folder.mkdir(exist_ok=True)
u.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.UpdateWindow.argtypes = [W.HWND]
u.GetDlgItem.argtypes = [W.HWND, C.c_int]
u.GetDlgItem.restype = W.HWND
u.SendNotifyMessageW.argtypes = [W.HWND,W.UINT,W.WPARAM,W.LPARAM]
counts, headers, gates = Counter(), [], {}
lock = threading.Lock()
png = io.BytesIO()
Image.new('RGB', (240,120), (211,47,131)).save(png, format='PNG')
payload = png.getvalue()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        with lock:
            counts[self.path] += 1
            number = counts[self.path]
            headers.append(dict(self.headers))
        try:
            if self.path == '/redirect':
                self.send_response(302)
                self.send_header('Location', '/redirected.png')
                self.send_header('Set-Cookie', 'fixture=private')
                self.end_headers()
                return
            if self.path == '/loop':
                self.send_response(302)
                self.send_header('Location', '/loop')
                self.end_headers()
                return
            status = 404 if self.path == '/missing' or (self.path == '/retry' and number == 1) else 200
            data = b'not an image' if self.path == '/corrupt' else payload
            self.send_response(status)
            self.send_header('Content-Type', 'image/png')
            if self.path == '/stream-large':
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                part = b'x' * 16384
                for _ in range(600):
                    self.wfile.write(b'4000\r\n'+part+b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
                return
            self.send_header('Content-Length', str(9*1024*1024 if self.path == '/large' else len(data)))
            self.end_headers()
            if self.path == '/large':
                return
            if self.path.startswith('/slow') or self.path in ('/timeout','/concurrent'):
                gate = gates[self.path]
                self.wfile.write(data[:1])
                self.wfile.flush()
                gate.wait(20)
                self.wfile.write(data[1:])
            else:
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


server = ThreadingHTTPServer(('127.0.0.1',0), Handler)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
checks, processes = [], []
cache_scope = tempfile.TemporaryDirectory(prefix='cache-', dir=folder)
cache = Path(cache_scope.name).resolve()
assert cache.is_relative_to(folder.resolve())


def top(pid, cls):
    found = []
    def collect(h, _):
        owner = W.DWORD()
        u.GetWindowThreadProcessId(h, C.byref(owner))
        if owner.value == pid and name(h, True) == cls and u.IsWindowVisible(h):
            found.append(h)
        return True
    callback = CALLBACK(collect)
    u.EnumWindows(callback,0)
    return (found or [None])[0]


def count(path):
    with lock:
        return counts[path]


def open_case(label, endpoint=None, body=None, cache_dir=None):
    config = folder/(label+'.ini')
    config.write_text('[Reader]\nDark=0\nZoom=1000\n', encoding='utf-8')
    path = folder/(label+'.md')
    original = ((body if body is not None else f'![测试网络图片]({base}{endpoint})')+
                '\n\n# Next\n\n本地正文不应被改写。\n').encode('utf-8')
    path.write_bytes(original)
    report = folder/(label+'.json')
    p = subprocess.Popen([str(args.exe),str(path),'--config',str(config),'--report',str(report),
                          '--image-cache',str(cache_dir or cache)],cwd=ROOT)
    processes.append(p)
    h = wait(lambda: (windows(p.pid) or [None])[0])
    wait(lambda: path.name in name(h))
    reader = next(c for c in children(h) if name(c,True) == 'KeepMD.Reader')
    def stats():
        u.SendMessageW(h,0x8000+100,0,0)
        return json.loads(report.read_text(encoding='utf-8'))
    wait(lambda: stats()['paint_count'] > 0)
    wait(lambda: not stats()['assets_pending'])
    assert not stats()['remote_images_enabled'], 'a different document must start with network access off'
    return p,h,reader,stats,path,original


def toggle(case, enabled):
    control = u.GetDlgItem(case[1],133)
    assert control and u.IsWindowVisible(control)
    u.SendMessageW(control,0xF5,0,0)
    wait(lambda: case[3]()['remote_images_enabled'] == enabled)
    assert ('已开启' if enabled else '已关闭') in name(control)
    assert not top(case[0].pid,'#32770'), 'one toolbar switch must not open per-image confirmations'


def loaded(case):
    return case[3]()['asset_bytes'] > 0 and not case[3]()['assets_pending']


def close(case):
    p,h,_,_,path,original = case
    u.PostMessageW(h,0x10,0,0)
    p.wait(timeout=5)
    assert p.returncode == 0 and path.read_bytes() == original


def key(endpoint):
    return hashlib.sha256((base+endpoint).encode()).hexdigest()+'.kmi'


try:
    body = (f'[![测试网络图片]({base}/batch1)](#next)\n\n![相同图片]({base}/batch1)\n\n'+
            ('正文占位。\n\n'*70)+
            '\n\n'.join(f'![第 {i} 张]({base}/batch{i})' for i in range(2,9)))
    case = open_case('batch',body=body)
    p,h,reader,stats,_,_ = case
    assert not counts and stats()['asset_bytes'] == 0
    # Image placeholders themselves no longer initiate loading or open a browser/confirmation.
    u.SendMessageW(reader,0x201,1,(50<<16)|120)
    u.SendNotifyMessageW(reader,0x202,0,(50<<16)|120)
    time.sleep(.2)
    assert not counts and not top(p.pid,'#32770')
    capture(h,folder/'before-enable.png')
    checks.append('opening and clicking a placeholder make no network request or confirmation dialog')
    toggle(case,True)
    wait(lambda: all(count(f'/batch{i}') == 1 for i in range(1,9)),timeout=15)
    wait(lambda: loaded(case),timeout=10)
    assert all((cache/key(f'/batch{i}')).is_file() for i in range(1,9))
    pixels = capture(h,folder/'all-images-enabled.png')
    assert sum(n for n,color in pixels.getcolors(pixels.width*pixels.height) if color == (211,47,131)) >= 240*120
    checks.append('right toolbar switch downloads all eight unique URLs, including off-screen images; duplicates download once')
    for command in (107,106,106,112):
        u.SendMessageW(h,0x111,command,0)
        wait(lambda: loaded(case))
    u.SendMessageW(h,0x8000+101,0,0)
    wait(lambda: loaded(case))
    toggle(case,False)
    wait(lambda: loaded(case))
    toggle(case,True)
    wait(lambda: loaded(case))
    assert all(count(f'/batch{i}') == 1 for i in range(1,9))
    checks.append('zoom, theme, reload, device recreation and toggling reuse cached images')
    close(case)

    case = open_case('cached-in-other-document','/batch1')
    wait(lambda: loaded(case))
    assert count('/batch1') == 1
    capture(case[1],folder/'cache-after-restart.png')
    close(case)
    checks.append('another Markdown file in a new process displays the same URL from disk with network loading off')

    # Corruption is detected without showing stale/wrong bytes; repair requires the document switch.
    damaged = bytearray((cache/key('/batch1')).read_bytes())
    damaged[-1] ^= 255
    (cache/key('/batch1')).write_bytes(damaged)
    case = open_case('corrupt-cache','/batch1')
    assert case[3]()['asset_bytes'] == 0 and count('/batch1') == 1
    toggle(case,True)
    wait(lambda: loaded(case))
    assert count('/batch1') == 2
    close(case)
    checks.append('corrupt cache is discarded and refetched only after the document switch is enabled')

    case = open_case('redirect','/redirect')
    toggle(case,True)
    wait(lambda: loaded(case))
    assert count('/redirect') == 1 and count('/redirected.png') == 1
    with lock:
        assert all(not any(k.lower() in ('authorization','cookie','referer') for k in entry) for entry in headers)
    close(case)
    case = open_case('redirect-cached','/redirect')
    wait(lambda: loaded(case))
    assert count('/redirect') == 1 and count('/redirected.png') == 1
    close(case)
    checks.append('redirected images persist under the original URL; no cookies, credentials or document referrer are sent')

    for endpoint in ('/missing','/corrupt','/large','/stream-large','/loop','/retry'):
        case = open_case(endpoint[1:],endpoint)
        toggle(case,True)
        wait(lambda: count(endpoint) > 0)
        wait(lambda: not case[3]()['assets_pending'],timeout=18)
        assert case[3]()['asset_bytes'] == 0 and not (cache/key(endpoint)).exists()
        before = count(endpoint)
        time.sleep(.15)
        assert count(endpoint) == before, 'failed downloads must not loop automatically'
        if endpoint == '/retry':
            toggle(case,False)
            toggle(case,True)
            wait(lambda: loaded(case))
            assert count(endpoint) == 2
        if endpoint == '/loop': assert before <= 6
        close(case)
    checks.append('HTTP, decoding, size and redirect errors are not cached or auto-retried; toggling allows recovery')

    for endpoint in ('/slow-close','/slow-new','/slow-off'):
        gates[endpoint] = threading.Event()
        case = open_case(endpoint[1:],endpoint)
        toggle(case,True)
        wait(lambda: count(endpoint) == 1)
        start = time.monotonic()
        assert case[3]()['assets_pending'] and time.monotonic()-start < 1
        if endpoint == '/slow-new':
            u.SendMessageW(case[1],0x111,120,0)
            wait(lambda: u.GetDlgItem(case[1],202))
            assert not case[3]()['remote_images_enabled']
        elif endpoint == '/slow-off':
            toggle(case,False)
            wait(lambda: not case[3]()['assets_pending'])
        close(case)
        assert time.monotonic()-start < 3
        gates[endpoint].set()
        assert not (cache/key(endpoint)).exists()
    checks.append('closing, creating another document or disabling the switch cancels slow downloads promptly')

    # URL lock coalesces concurrent readers, not only duplicate blocks in a single process.
    gates['/concurrent'] = threading.Event()
    first = open_case('concurrent-a','/concurrent')
    second = open_case('concurrent-b','/concurrent')
    toggle(first,True)
    toggle(second,True)
    wait(lambda: count('/concurrent') == 1)
    time.sleep(.2)
    assert count('/concurrent') == 1
    gates['/concurrent'].set()
    wait(lambda: loaded(first) and loaded(second))
    assert count('/concurrent') == 1
    close(first); close(second)
    checks.append('two simultaneous readers share one download for the same URL')

    # A read-only/bad cache directory must not prevent an enabled image from displaying.
    blocked = cache/'not-a-directory'
    blocked.write_text('fixture',encoding='utf-8')
    case = open_case('cache-write-failure','/uncacheable',cache_dir=blocked)
    toggle(case,True)
    wait(lambda: loaded(case))
    close(case)
    checks.append('cache write failure does not prevent displaying a successfully downloaded image')

    # Drive the real cache-entry limit using tiny disposable entries and an unrelated sentinel file.
    oldest = cache/('0'*64+'.kmi')
    for i in range(1024):
        entry = cache/(f'{i:064x}.kmi')
        entry.write_bytes(b'KMDIMG01'+bytes(32)+b'x')
        os.utime(entry,(1000+i,1000+i))
    sentinel = cache/'user-note.txt';sentinel.write_text('keep',encoding='utf-8')
    case = open_case('prune','/prune')
    toggle(case,True)
    wait(lambda: loaded(case))
    assert len(list(cache.glob('*.kmi'))) <= 1024 and not oldest.exists()
    assert sentinel.read_text(encoding='utf-8') == 'keep'
    close(case)
    checks.append('cache prunes old entries within its limit without touching unrelated files')

    gates['/timeout'] = threading.Event()
    case = open_case('timeout','/timeout')
    toggle(case,True)
    wait(lambda: count('/timeout') == 1)
    wait(lambda: not case[3]()['assets_pending'],timeout=18)
    assert case[3]()['asset_bytes'] == 0
    close(case)
    checks.append('stalled responses time out and do not poison the persistent cache')
finally:
    for event in gates.values(): event.set()
    for p in processes:
        if p.poll() is None: p.terminate(); p.wait(timeout=5)
    server.shutdown()
    server.server_close()
    assert cache.resolve().is_relative_to(folder.resolve())
    cache_scope.cleanup()

result = {'checks':checks,'requests':dict(counts),'exe_sha256':hashlib.sha256(args.exe.read_bytes()).hexdigest()}
(folder/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
(ROOT/'bench/results/remote-images-e2e.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,ensure_ascii=False,indent=2))
