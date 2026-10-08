"""Verify KeepPrompt survives independently of the reader process."""
import ctypes as C
import hashlib
import json
import subprocess
from ctypes import wintypes as W

from test_gui import ROOT, OUT, CALLBACK, clipboard, name, u, wait

u.GetDlgItem.argtypes = [W.HWND, C.c_int]
u.GetDlgItem.restype = W.HWND
u.IsWindow.argtypes = [W.HWND]

exe = ROOT / "build/release/keepmd.exe"
prompt_exe = ROOT / "build/release/KeepPrompt.exe"
config = OUT / "independent-prompt.ini"
prefs = config.with_suffix(".prompt.ini")
draft = config.with_suffix(".prompt.md")
config.write_text("[Reader]\nDark=0\n", encoding="utf-8")
prefs.write_text("[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=1\n", encoding="utf-8")
if draft.exists():
    draft.unlink()


def first(pid, cls):
    found = []

    def collect(hwnd, _):
        value = W.DWORD()
        u.GetWindowThreadProcessId(hwnd, C.byref(value))
        if value.value == pid and name(hwnd, True).startswith(cls):
            found.append(hwnd)
        return True

    callback = CALLBACK(collect)
    u.EnumWindows(callback, 0)
    return found[0] if found else None


def launch(path, *args):
    return subprocess.Popen([str(path), "--config", str(config), *args], cwd=ROOT)


def set_text(hwnd, value):
    buffer = C.create_unicode_buffer(value)
    u.SendMessageW(hwnd, 0x000C, 0, C.cast(buffer, C.c_void_p).value)


def command(hwnd, value):
    u.SendMessageW(hwnd, 0x0111, value, 0)


checks = []
resident = launch(prompt_exe, "--resident")
reader = None
try:
    panel = wait(lambda: first(resident.pid, "KeepMD.Prompt."))
    assert not u.IsWindowVisible(panel)
    assert not first(resident.pid, "KeepMD.Window")
    checks.append("KeepPrompt resident starts as a separate hidden process without a reader window")

    broker = launch(exe, "--prompt")
    broker.wait(timeout=8)
    assert broker.returncode == 0
    wait(lambda: u.IsWindowVisible(panel))
    editor = wait(lambda: u.GetDlgItem(panel, 500))
    sample = "# 独立提示词\n\n**KeepPrompt** 😀\n"
    set_text(editor, sample)
    wait(lambda: draft.exists() and draft.read_text(encoding="utf-8") == sample)
    command(panel, 511)
    wait(lambda: clipboard() == sample)
    checks.append("KeepMD broker summons KeepPrompt; visual editor autosaves and copies Markdown")

    reader = launch(exe, str(OUT / "html-width.md"))
    main = wait(lambda: first(reader.pid, "KeepMD.Window"))
    u.PostMessageW(main, 0x0010, 0, 0)
    reader.wait(timeout=8)
    assert reader.returncode == 0 and resident.poll() is None and u.IsWindow(panel)
    checks.append("closing KeepMD leaves the KeepPrompt process and prompt window alive")

    command(panel, 525)
    resident.wait(timeout=8)
    assert resident.returncode == 0
    checks.append("KeepPrompt explicit Quit exits its own process")
finally:
    if reader and reader.poll() is None:
        reader.terminate()
        reader.wait(timeout=5)
    if resident.poll() is None:
        resident.terminate()
        resident.wait(timeout=5)

result = {
    "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
    "prompt_exe_sha256": hashlib.sha256(prompt_exe.read_bytes()).hexdigest(),
    "checks": checks,
}
(ROOT / "bench/results/prompt-process-e2e.json").write_text(
    json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)
print(json.dumps(result, ensure_ascii=False, indent=2))
