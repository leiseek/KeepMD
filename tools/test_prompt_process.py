"""Verify KeepMD and KeepPrompt are independent programs: neither starts, summons nor configures the other."""
import ctypes as C
import hashlib
import json
import shutil
import subprocess
import time
from ctypes import wintypes as W

from test_gui import ROOT, OUT, CALLBACK, children, clipboard, name, u, wait

u.GetDlgItem.argtypes = [W.HWND, C.c_int]
u.GetDlgItem.restype = W.HWND
u.IsWindow.argtypes = [W.HWND]

exe = ROOT / "build/release/keepmd.exe"
prompt_exe = ROOT / "build/release/KeepPrompt.exe"
reader_config = OUT / "independent-reader.ini"
prompt_config = OUT / "independent-prompt.ini"
draft = prompt_config.with_suffix(".draft.md")
reader_config.write_text("[Reader]\nDark=0\n", encoding="utf-8")
prompt_config.write_text("[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=1\n", encoding="utf-8")
for stale in (draft, reader_config.with_suffix(".prompt.ini"), reader_config.with_suffix(".prompt.md")):
    if stale.exists():
        stale.unlink()


def top_windows(pid, prefix):
    found = []

    def collect(hwnd, _):
        value = W.DWORD()
        u.GetWindowThreadProcessId(hwnd, C.byref(value))
        if value.value == pid and name(hwnd, True).startswith(prefix):
            found.append(hwnd)
        return True

    callback = CALLBACK(collect)
    u.EnumWindows(callback, 0)
    return found


def first(pid, prefix):
    return (top_windows(pid, prefix) or [None])[0]


def keepprompt_processes():
    output = subprocess.run(["tasklist", "/FI", "IMAGENAME eq KeepPrompt.exe", "/FO", "CSV", "/NH"],
                            capture_output=True, text=True).stdout
    return {line.split('","')[1] for line in output.splitlines() if line.startswith('"KeepPrompt.exe"')}


def set_text(hwnd, value):
    buffer = C.create_unicode_buffer(value)
    u.SendMessageW(hwnd, 0x000C, 0, C.cast(buffer, C.c_void_p).value)


checks = []
reader = prompt = None
try:
    before = keepprompt_processes()
    reader = subprocess.Popen([str(exe), str(OUT / "html-width.md"), "--config", str(reader_config), "--prompt"],
                              cwd=ROOT)
    main = wait(lambda: first(reader.pid, "KeepMD.Window"))
    wait(lambda: "html-width.md" in name(main))
    time.sleep(.6)
    assert keepprompt_processes() == before, "the reader started KeepPrompt"
    caption = next(h for h in children(main) if name(h, True) == "Keep.Caption")
    labels = [name(h) for h in children(caption)]
    assert "提示词" not in labels and "文件" in labels, labels
    checks.append("KeepMD runs alone: no prompt menu, and a legacy --prompt flag starts no other program")

    prompt = subprocess.Popen([str(prompt_exe), "--resident", "--config", str(prompt_config)], cwd=ROOT)
    panel = wait(lambda: first(prompt.pid, "KeepPrompt."))
    assert not u.IsWindowVisible(panel) and not first(prompt.pid, "KeepMD.")
    checks.append("KeepPrompt runs alone as a hidden resident process with its own window class")

    again = subprocess.Popen([str(prompt_exe), "--show", "--config", str(prompt_config)], cwd=ROOT)
    again.wait(timeout=8)
    assert again.returncode == 0
    wait(lambda: u.IsWindowVisible(panel))
    assert len(top_windows(prompt.pid, "KeepPrompt.")) == 1
    editor = wait(lambda: u.GetDlgItem(panel, 500))
    sample = "# 独立提示词\n\n**KeepPrompt** 😀\n"
    set_text(editor, sample)
    wait(lambda: draft.exists() and draft.read_text(encoding="utf-8") == sample)
    u.SendMessageW(panel, 0x0111, 511, 0)
    wait(lambda: clipboard() == sample)
    checks.append("a second KeepPrompt launch summons the first; the draft autosaves beside its own profile")

    u.PostMessageW(main, 0x0010, 0, 0)
    reader.wait(timeout=8)
    assert reader.returncode == 0 and prompt.poll() is None and u.IsWindow(panel)
    assert "[Reader]" in reader_config.read_text(encoding="utf-8")
    assert "[Prompt]" in prompt_config.read_text(encoding="utf-8")
    assert not reader_config.with_suffix(".prompt.ini").exists() and not reader_config.with_suffix(".prompt.md").exists()
    checks.append("closing KeepMD leaves KeepPrompt running; each program writes only its own settings")

    u.SendMessageW(panel, 0x0111, 525, 0)
    prompt.wait(timeout=8)
    assert prompt.returncode == 0
    checks.append("KeepPrompt Quit ends only its own process")

    # Upgrades from KeepMD 0.4.x, which kept prompts beside the reader profile as keepmd.prompt.ini/.md.
    folder = OUT / "pre-split-folder"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir()
    shutil.copy2(prompt_exe, folder / "KeepPrompt.exe")
    legacy_settings, legacy_draft = folder / "keepmd.prompt.ini", folder / "keepmd.prompt.md"
    legacy_settings.write_text("[Prompt]\nHotkey=Ctrl+Alt+Space\nResident=0\n", encoding="utf-8")
    legacy_draft.write_text("# 升级前的草稿\n", encoding="utf-8")
    prompt = subprocess.Popen([str(folder / "KeepPrompt.exe"), "--show"], cwd=folder)
    panel = wait(lambda: first(prompt.pid, "KeepPrompt."))
    editor = wait(lambda: u.GetDlgItem(panel, 500))
    wait(lambda: "升级前的草稿" in name(editor))
    u.SendMessageW(panel, 0x0111, 525, 0)
    prompt.wait(timeout=8)
    assert legacy_draft.read_text(encoding="utf-8") == "# 升级前的草稿\n"
    assert not (folder / "KeepPrompt.ini").exists() and not (folder / "KeepPrompt.draft.md").exists()
    checks.append("a pre-split portable folder keeps opening its existing prompt draft in place")

    # A pre-split startup entry passes the KeepMD reader profile; it must never be rewritten as prompt settings.
    reader_profile = folder / "keepmd.ini"
    reader_profile.write_text("[Reader]\nDark=1\nZoom=1000\n", encoding="utf-8")
    original = reader_profile.read_bytes()
    prompt = subprocess.Popen([str(folder / "KeepPrompt.exe"), "--resident", "--config", str(reader_profile)],
                              cwd=folder)
    panel = wait(lambda: first(prompt.pid, "KeepPrompt."))
    wait(lambda: "Resident=1" in legacy_settings.read_text(encoding="utf-8"))
    u.SendMessageW(panel, 0x0111, 525, 0)
    prompt.wait(timeout=8)
    assert reader_profile.read_bytes() == original, "KeepPrompt rewrote the KeepMD reader profile"
    assert not (folder / "keepmd.draft.md").exists()
    checks.append("a pre-split startup entry uses the old prompt files and leaves the KeepMD reader profile untouched")

    # KeepMD 0.4.5 startup entries ran the reader with --resident for its retired background service.
    started = time.perf_counter()
    legacy = subprocess.run([str(exe), "--resident", "--config", str(reader_profile)], cwd=ROOT, timeout=10)
    assert legacy.returncode == 0 and time.perf_counter() - started < 5
    assert reader_profile.read_bytes() == original
    checks.append("a KeepMD 0.4.5 --resident startup entry exits without opening a window or touching settings")
finally:
    for process in (reader, prompt):
        if process and process.poll() is None:
            process.terminate()
            process.wait(timeout=5)

result = {
    "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
    "prompt_exe_sha256": hashlib.sha256(prompt_exe.read_bytes()).hexdigest(),
    "checks": checks,
}
(ROOT / "bench/results/prompt-process-e2e.json").write_text(
    json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)
print(json.dumps(result, ensure_ascii=False, indent=2))
