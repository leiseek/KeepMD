#include "prompt_window.h"
#include "editor.h"
#include "file_io.h"
#include "prompt_model.h"
#include "view.h"
#include <array>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <imm.h>
#include <richedit.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

namespace keepmd {
namespace {
constexpr UINT Summon = WM_APP + 31, Tray = WM_APP + 32, PreviewReady = WM_APP + 33;
constexpr int EditId = 500, StatusId = 501, HotkeyId = 502;
enum Action {
    CopyHide = 510,
    CopyOnly,
    Clear,
    Preview,
    Options,
    Apply,
    DoubleCtrl,
    DefaultKey,
    Resident,
    Top,
    Startup,
    ImportMd,
    ExportMd,
    ImportLegacy,
    Reader,
    Quit,
    Reposition,
    OpacityBase = 600
};
std::wstring text_of(HWND hwnd) {
    std::wstring s(GetWindowTextLengthW(hwnd) + 1, 0);
    s.resize(GetWindowTextW(hwnd, s.data(), (int)s.size()));
    return s;
}
bool clipboard_text(HWND hwnd, const std::wstring &text) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!h)
        return false;
    auto p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return false;
    }
    memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    if (!OpenClipboard(hwnd)) {
        GlobalFree(h);
        return false;
    }
    bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
    if (!ok)
        GlobalFree(h);
    return ok;
}
bool composing(HWND edit) {
    auto context = ImmGetContext(edit);
    bool active = context && ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0;
    if (context)
        ImmReleaseContext(edit, context);
    return active;
}
} // namespace
struct PromptWindow::Impl {
    HWND owner = nullptr, hwnd = nullptr, previous = nullptr, reader = nullptr, status = nullptr,
         hotkeyEdit = nullptr, hotkeyLabel = nullptr;
    DWORD previousPid = 0;
    HANDLE mutex = nullptr;
    HFONT font = nullptr;
    HMENU menu = nullptr, prefsMenu = nullptr, opacityMenu = nullptr;
    std::vector<std::pair<int, HWND>> buttons;
    std::unique_ptr<Editor> editor;
    std::unique_ptr<View> view;
    std::filesystem::path readerConfig, config, draftPath;
    std::wstring className, startupValue, startupCommand, draft, hotkeyError;
    PromptSettings settings;
    PromptHotkey activeHotkey;
    CtrlTap taps;
    std::array<bool, 256> otherKeys{};
    unsigned otherHeld = 0;
    int hotkeyToken = 0;
    bool raw = false, dark = false, options = false, dirty = false, loading = false, tray = false;
    bool draftLoaded = false, draftBlocked = false, modal = false;
    bool draftExists = false;
    std::filesystem::file_time_type draftStamp{};
    float dpi = 1;
    UINT taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    std::unique_ptr<Worker> worker;
    std::shared_ptr<std::atomic_bool> cancel;
    std::mutex resultMutex;
    std::shared_ptr<Document> result;
    uint64_t generation = 0, resultVersion = 0;
    std::wstring previewSource;
    Impl(HWND host, const std::filesystem::path &path, bool theme)
        : owner(host), readerConfig(path), dark(theme) {
        config = path;
        config.replace_extension(L".prompt.ini");
        draftPath = path;
        draftPath.replace_extension(L".prompt.md");
        auto id = prompt_profile_id(config);
        className = L"KeepMD.Prompt." + id;
        startupValue = L"KeepMD.Prompt." + id;
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        startupCommand = prompt_startup_command(exe, path);
        settings = load_prompt_settings(config);
    }
    ~Impl() {
        if (cancel)
            cancel->store(true);
        worker.reset();
        if (hwnd) {
            stop_shortcut();
            tray_icon(false);
            view.reset();
            editor.reset();
            DestroyWindow(hwnd);
        }
        if (font)
            DeleteObject(font);
        if (mutex)
            CloseHandle(mutex);
    }
    void say(const std::wstring &s) {
        if (status)
            SetWindowTextW(status, s.c_str());
    }
    bool persist_settings() {
        std::wstring error;
        if (save_prompt_settings(config, settings, error))
            return true;
        say(L"设置未保存：" + error);
        return false;
    }
    bool load_draft() {
        if (draftLoaded)
            return !draftBlocked;
        draftLoaded = true;
        std::error_code ec;
        if (!std::filesystem::exists(draftPath, ec) && !ec)
            return true;
        FileData data;
        std::wstring error;
        auto size = std::filesystem::file_size(draftPath, ec);
        if (ec || size > 4 * PromptLimit || !load_file(draftPath, data, error) ||
            wide(data.utf8).size() > PromptLimit || data.utf8.find('\0') != std::string::npos) {
            draftBlocked = true;
            say(L"无法读取草稿，原文件已保留。请检查 " + draftPath.wstring());
            return false;
        }
        draft = wide(data.utf8);
        draftExists = true;
        draftStamp = data.stamp;
        return true;
    }
    bool flush() {
        if (!dirty)
            return true;
        if (draftBlocked) {
            say(L"原草稿无法读取；请先另存为或排除草稿文件错误。");
            return false;
        }
        std::wstring error;
        auto text = editor ? editor->text(false) : draft;
        if (text == draft) {
            dirty = false;
            if (editor)
                editor->saved();
            KillTimer(hwnd, 2);
            return true;
        }
        std::error_code ec;
        bool exists = std::filesystem::exists(draftPath, ec);
        bool changed = ec || exists != draftExists;
        if (exists && !changed)
            changed = std::filesystem::last_write_time(draftPath, ec) != draftStamp || ec;
        if (changed) {
            say(L"草稿文件在外部发生变化，已停止自动覆盖。请用“导出 "
                L"Markdown”保存当前输入，再重启载入磁盘草稿。");
            return false;
        }
        if (!save_file(draftPath, text, Encoding::Utf8, error)) {
            say(L"草稿未保存：" + error);
            return false;
        }
        draft = std::move(text);
        dirty = false;
        draftExists = true;
        draftStamp = std::filesystem::last_write_time(draftPath, ec);
        if (editor)
            editor->saved();
        KillTimer(hwnd, 2);
        return true;
    }
    bool prepare_close() {
        if (editor && composing(editor->hwnd())) {
            show();
            say(L"请先完成或取消输入法组词，再关闭窗口。");
            return false;
        }
        if (flush())
            return true;
        show();
        modal = true;
        int choice = MessageBoxW(hwnd,
                                 L"提示词草稿无法保存，是否先导出当前输入？\n\n是：选择另一个 Markdown "
                                 L"文件保存后关闭。\n否：放弃当前未保存输入。\n取消：继续编辑。",
                                 L"KeepMD · 草稿未保存", MB_YESNOCANCEL | MB_ICONWARNING);
        modal = false;
        if (choice == IDCANCEL)
            return false;
        if (choice == IDYES) {
            auto path = dialog(true, false);
            if (path.empty())
                return false;
            std::wstring error;
            if (!save_file(path, editor->text(false), Encoding::Utf8, error)) {
                say(error);
                return false;
            }
        }
        draftLoaded = draftBlocked = draftExists = false;
        draft.clear();
        load_draft();
        loading = true;
        editor->load(draft);
        loading = false;
        EnableWindow(editor->hwnd(), !draftBlocked);
        dirty = false;
        KillTimer(hwnd, 2);
        refresh_preview();
        return true;
    }
    void stop_shortcut() {
        if (hotkeyToken)
            UnregisterHotKey(hwnd, hotkeyToken);
        hotkeyToken = 0;
        if (raw) {
            RAWINPUTDEVICE device{1, 6, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&device, 1, sizeof(device));
            raw = false;
        }
        taps = {};
        otherKeys = {};
        otherHeld = 0;
    }
    bool shortcut(const std::wstring &text) {
        PromptHotkey next;
        std::wstring error;
        if (!parse_prompt_hotkey(text, next, error)) {
            hotkeyError = error;
            say(error);
            return false;
        }
        if (next.label == activeHotkey.label && (hotkeyToken || raw))
            return true;
        if (next.doubleCtrl) {
            if (FindWindowW(L"PromptFlow_MSVC_Class", nullptr)) {
                hotkeyError = L"旧 Prompt Flow 正在运行；请退出旧程序后启用双击左 Ctrl，或使用组合快捷键。";
                say(hotkeyError);
                return false;
            }
            RAWINPUTDEVICE device{1, 6, RIDEV_INPUTSINK, hwnd};
            if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
                hotkeyError = L"无法启用双击左 Ctrl，原快捷键保留。";
                say(hotkeyError);
                return false;
            }
            if (hotkeyToken)
                UnregisterHotKey(hwnd, hotkeyToken);
            hotkeyToken = 0;
            raw = true;
            taps = {};
        } else {
            int token = hotkeyToken == 1 ? 2 : 1;
            if (!RegisterHotKey(hwnd, token, next.modifiers | MOD_NOREPEAT, next.key)) {
                hotkeyError =
                    L"快捷键 " + next.label + L" 注册失败（可能被占用）。原快捷键保留；可在此修改。";
                say(hotkeyError);
                return false;
            }
            stop_shortcut();
            hotkeyToken = token;
        }
        activeHotkey = next;
        settings.hotkey = next.label;
        hotkeyError.clear();
        if (hotkeyEdit)
            SetWindowTextW(hotkeyEdit, next.label.c_str());
        if (tray)
            tray_icon(true);
        return true;
    }
    void tray_icon(bool add) {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = hwnd;
        data.uID = 1;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        data.uCallbackMessage = Tray;
        data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        std::wstring tip = L"KeepMD 提示词 · " + settings.hotkey;
        wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
        if (add) {
            bool ok = Shell_NotifyIconW(tray ? NIM_MODIFY : NIM_ADD, &data) != FALSE;
            if (!ok)
                ok = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
            tray = ok;
        } else {
            Shell_NotifyIconW(NIM_DELETE, &data);
            tray = false;
        }
    }
    bool start(bool forceResident, bool showNow) {
        if (hwnd) {
            if (forceResident) {
                settings.resident = true;
                tray_icon(true);
                persist_settings();
            }
            if (showNow)
                show();
            return true;
        }
        mutex = CreateMutexW(nullptr, FALSE, (L"Local\\" + className).c_str());
        if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (mutex) {
                CloseHandle(mutex);
                mutex = nullptr;
            }
            if (auto other = FindWindowW(className.c_str(), nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                if (showNow || forceResident)
                    PostMessageW(other, Summon, (showNow ? 1 : 0) | (forceResident ? 2 : 0), 0);
            } else
                MessageBoxW(owner, L"另一个 KeepMD 提示词实例正在启动，请稍后重试。", L"KeepMD", MB_OK);
            return false;
        }
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = proc;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(101));
        cls.lpszClassName = className.c_str();
        RegisterClassExW(&cls);
        cls.lpszClassName = L"KeepMD.PromptPreview";
        cls.lpfnWndProc = preview_proc;
        cls.hbrBackground = nullptr;
        cls.style = CS_DBLCLKS;
        RegisterClassExW(&cls);
        if (forceResident)
            settings.resident = true;
        hwnd =
            CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, className.c_str(), L"KeepMD · Markdown 提示词",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 940, 580,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd) {
            CloseHandle(mutex);
            mutex = nullptr;
            return false;
        }
        apply_appearance();
        bool ok = shortcut(settings.hotkey);
        if (settings.resident)
            tray_icon(true);
        if (forceResident)
            persist_settings();
        if (showNow || (settings.resident && (!ok || !tray))) {
            show();
            if (!ok) {
                options = true;
                layout();
                say(hotkeyError);
            } else if (settings.resident && !tray)
                say(L"托盘图标创建失败；提示词窗口保持可见。");
        }
        return true;
    }
    HWND child(const wchar_t *cls, const wchar_t *text, int id, DWORD extra = 0) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | extra, 0, 0, 1, 1, hwnd,
                               (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    }
    void ensure_ui() {
        if (editor)
            return;
        menu = CreateMenu();
        auto file = CreatePopupMenu();
        prefsMenu = CreatePopupMenu();
        opacityMenu = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, ImportMd, L"载入 Markdown…");
        AppendMenuW(file, MF_STRING, ExportMd, L"导出 Markdown…\tCtrl+S");
        AppendMenuW(file, MF_STRING, ImportLegacy, L"导入 Prompt Flow 配置…");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, Reader, L"打开阅读器");
        AppendMenuW(file, MF_STRING, Quit, L"退出 KeepMD");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"文件");
        AppendMenuW(prefsMenu, MF_STRING, Options, L"设置全局快捷键");
        AppendMenuW(prefsMenu, MF_STRING, Resident, L"关闭阅读器后常驻托盘");
        AppendMenuW(prefsMenu, MF_STRING, Startup, L"登录 Windows 时启动");
        AppendMenuW(prefsMenu, MF_STRING, Top, L"窗口置顶");
        for (int i : {100, 90, 80, 70, 60, 50, 40, 30}) {
            auto label = std::to_wstring(i) + L"%";
            AppendMenuW(opacityMenu, MF_STRING, OpacityBase + i, label.c_str());
        }
        AppendMenuW(prefsMenu, MF_POPUP, (UINT_PTR)opacityMenu, L"不透明度");
        AppendMenuW(prefsMenu, MF_STRING, Reposition, L"移到当前屏幕右侧");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)prefsMenu, L"设置");
        SetMenu(hwnd, menu);
        for (const auto &[id, label] :
             std::vector<std::pair<int, const wchar_t *>>{{CopyHide, L"复制并收起"},
                                                          {CopyOnly, L"复制全文"},
                                                          {Preview, L"预览"},
                                                          {Clear, L"清空"},
                                                          {Options, L"快捷键"},
                                                          {Apply, L"应用"},
                                                          {DoubleCtrl, L"双击左 Ctrl"},
                                                          {DefaultKey, L"默认"}})
            buttons.emplace_back(id, child(L"BUTTON", label, id, WS_TABSTOP));
        hotkeyLabel = child(L"STATIC", L"全局快捷键", 0);
        hotkeyEdit =
            child(L"EDIT", settings.hotkey.c_str(), HotkeyId, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
        SendMessageW(hotkeyEdit, EM_SETLIMITTEXT, 100, 0);
        status = child(L"STATIC", L"", StatusId, SS_LEFTNOWORDWRAP);
        editor = std::make_unique<Editor>(hwnd, EditId);
        if (!editor->valid()) {
            say(L"无法加载 Windows 文本编辑控件。");
            return;
        }
        SendMessageW(editor->hwnd(), EM_EXLIMITTEXT, 0, PromptLimit);
        // Soft wrapping keeps prose convenient; the source retains its exact whitespace.
        SendMessageW(editor->hwnd(), EM_SETTARGETDEVICE, 0, 0);
        load_draft();
        loading = true;
        editor->load(draft);
        loading = false;
        reader = CreateWindowExW(0, L"KeepMD.PromptPreview", nullptr,
                                 WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0, 1, 1, hwnd, nullptr,
                                 GetModuleHandleW(nullptr), this);
        ImmAssociateContext(reader, nullptr);
        update_font();
        apply_appearance();
        if (draftBlocked)
            EnableWindow(editor->hwnd(), FALSE);
    }
    void update_font() {
        auto old = font;
        dpi = (float)GetDpiForWindow(hwnd) / 96;
        font = CreateFontW(-(int)(15 * dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                           L"Segoe UI");
        for (const auto &[id, h] : buttons)
            SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        for (HWND h : {status, hotkeyEdit, hotkeyLabel})
            if (h)
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        if (old)
            DeleteObject(old);
    }
    void apply_appearance() {
        if (!hwnd)
            return;
        SetWindowPos(hwnd, settings.top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetLayeredWindowAttributes(hwnd, 0, (BYTE)(settings.opacity * 255 / 100), LWA_ALPHA);
        BOOL value = dark;
        DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value));
        if (editor) {
            bool wasLoading = loading;
            loading = true;
            editor->theme(dark);
            loading = wasLoading;
        }
        if (view)
            view->set_dark(dark);
    }
    void layout() {
        if (!editor)
            return;
        RECT r{};
        GetClientRect(hwnd, &r);
        auto px = [&](int n) { return (int)(n * dpi); };
        int x = px(10);
        for (const auto &[id, h] : buttons) {
            bool setting = id == Apply || id == DoubleCtrl || id == DefaultKey;
            ShowWindow(h, setting && !options ? SW_HIDE : SW_SHOW);
            if (!setting) {
                int width = px(id == CopyHide ? 120 : id == CopyOnly ? 100 : 78);
                MoveWindow(h, x, px(8), width, px(30), TRUE);
                x += width + px(8);
            }
        }
        ShowWindow(hotkeyEdit, options ? SW_SHOW : SW_HIDE);
        ShowWindow(hotkeyLabel, options ? SW_SHOW : SW_HIDE);
        MoveWindow(hotkeyLabel, px(10), px(54), px(95), px(24), TRUE);
        MoveWindow(hotkeyEdit, px(108), px(49), px(230), px(29), TRUE);
        x = px(347);
        for (const auto &[id, h] : buttons)
            if (id == Apply || id == DoubleCtrl || id == DefaultKey) {
                int width = px(id == DoubleCtrl ? 110 : 70);
                MoveWindow(h, x, px(48), width, px(30), TRUE);
                x += width + px(8);
            }
        int top = px(options ? 86 : 46), height = std::max<int>(px(40), r.bottom - top - px(35));
        int width = settings.preview ? (r.right - px(24)) / 2 : r.right - px(20);
        MoveWindow(editor->hwnd(), px(10), top, width, height, TRUE);
        ShowWindow(reader, settings.preview ? SW_SHOW : SW_HIDE);
        MoveWindow(reader, px(14) + width, top, std::max<int>(1, r.right - width - px(24)), height, TRUE);
        MoveWindow(status, px(10), r.bottom - px(27), r.right - px(20), px(23), TRUE);
        if (view)
            view->resize();
    }
    void place() {
        auto monitor = MonitorFromWindow(previous ? previous : owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        auto r = info.rcWork;
        int w = std::min<int>((int)(980 * dpi), r.right - r.left),
            h = std::min<int>((int)(600 * dpi), r.bottom - r.top);
        SetWindowPos(hwnd, nullptr, r.right - w - std::min<int>(20, (r.right - r.left - w) / 2),
                     r.top + (r.bottom - r.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void show() {
        if (!hwnd) {
            start(false, true);
            return;
        }
        auto foreground = GetForegroundWindow();
        if (foreground && foreground != hwnd && GetAncestor(foreground, GA_ROOTOWNER) != hwnd) {
            previous = foreground;
            GetWindowThreadProcessId(previous, &previousPid);
        }
        ensure_ui();
        place();
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        if (editor)
            SetFocus(editor->hwnd());
        layout();
        refresh_preview();
        if (draftBlocked)
            say(L"草稿无法读取，原文件已保留。请修复草稿文件后重启：" + draftPath.wstring());
        if (!hotkeyError.empty())
            say(hotkeyError);
    }
    bool hide(bool copy) {
        if (modal)
            return false;
        if (editor && composing(editor->hwnd())) {
            say(L"请先完成或取消输入法组词，再复制或收起。");
            return false;
        }
        if (copy && !copy_prompt())
            return false;
        if (!flush())
            return false;
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        if (cancel)
            cancel->store(true);
        if (worker)
            worker->clear();
        ++generation;
        view.reset();
        previewSource.clear();
        {
            std::lock_guard lock(resultMutex);
            result.reset();
        }
        bool restore = GetForegroundWindow() == hwnd;
        ShowWindow(hwnd, SW_HIDE);
        DWORD pid = 0;
        if (restore && IsWindow(previous) && GetWindowThreadProcessId(previous, &pid) && pid == previousPid)
            SetForegroundWindow(previous);
        return true;
    }
    bool copy_prompt() {
        if (!editor)
            ensure_ui();
        auto text = editor->text(false); // Preserve indentation, trailing newline, and blank lines.
        if (text.empty()) {
            say(L"草稿为空，未更改剪贴板。");
            return true;
        }
        if (!clipboard_text(hwnd, text)) {
            say(L"剪贴板正忙，内容尚未复制。请重试。");
            return false;
        }
        say(L"已复制 Markdown 源码 · " + std::to_wstring(text.size()) + L" UTF-16 字符");
        return true;
    }
    void refresh_preview() {
        if (!editor || !IsWindowVisible(hwnd))
            return;
        auto text = editor->text(false);
        auto lines = text.empty() ? 0 : 1 + std::count(text.begin(), text.end(), L'\n');
        say(std::to_wstring(text.size()) + L" UTF-16 字符 · " + std::to_wstring(lines) +
            L" 行 · Esc / Ctrl+Enter：复制并收起" + (dirty ? L" · 草稿待保存" : L""));
        if (!settings.preview)
            return;
        if (!view) {
            view = std::make_unique<View>(reader);
            view->set_dark(dark);
            view->set_empty_message(L"Markdown 提示词",
                                    L"在左侧输入提示词，这里显示预览。\n\n支持标题、列表、代码与 Mermaid "
                                    L"流程图。\n\nCtrl+Enter：复制源码并收起。\nF6：切换预览。");
        }
        if (previewSource == text && view->document())
            return;
        previewSource = text;
        if (cancel)
            cancel->store(true);
        cancel = std::make_shared<std::atomic_bool>(false);
        auto cancellation = cancel;
        auto version = ++generation;
        if (!worker)
            worker = std::make_unique<Worker>();
        worker->clear();
        worker->push([this, source = utf8(text), cancellation, version]() mutable {
            std::shared_ptr<Document> doc;
            try {
                doc = parse_document(std::move(source), cancellation.get());
            } catch (...) {
                return;
            }
            if (cancellation->load())
                return;
            {
                std::lock_guard lock(resultMutex);
                result = std::move(doc);
                resultVersion = version;
            }
            PostMessageW(hwnd, PreviewReady, 0, 0);
        });
    }
    void update_menu() {
        if (!prefsMenu)
            return;
        for (auto [id, checked] : {std::pair{Resident, settings.resident},
                                   {Top, settings.top},
                                   {Startup, prompt_startup_enabled(startupValue, startupCommand)}})
            CheckMenuItem(prefsMenu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuRadioItem(opacityMenu, OpacityBase + 30, OpacityBase + 100, OpacityBase + settings.opacity,
                           MF_BYCOMMAND);
    }
    std::filesystem::path dialog(bool save, bool legacy) {
        wchar_t path[32768]{};
        std::wstring initial;
        if (legacy) {
            PWSTR roaming = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
                initial = std::wstring(roaming) + L"\\PromptFlow\\PromptFlow\\config";
                CoTaskMemFree(roaming);
            }
        } else if (save)
            wcscpy_s(path, L"提示词.md");
        OPENFILENAMEW of{sizeof(of)};
        of.hwndOwner = hwnd;
        of.lpstrFile = path;
        of.nMaxFile = 32768;
        of.lpstrFilter =
            legacy ? L"Prompt Flow 配置\0*.json\0\0" : L"Markdown\0*.md;*.markdown\0所有文件\0*.*\0\0";
        of.lpstrDefExt = legacy ? L"json" : L"md";
        of.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
        of.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        modal = true;
        bool ok = (save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of)) != FALSE;
        modal = false;
        return ok ? std::filesystem::path(path) : std::filesystem::path();
    }
    void import_file(bool legacy) {
        if (!flush())
            return;
        auto path = dialog(false, legacy);
        if (path.empty())
            return;
        FileData data;
        std::wstring error;
        std::error_code ec;
        if (std::filesystem::file_size(path, ec) > (legacy ? 8 : 4) * PromptLimit || ec ||
            !load_file(path, data, error)) {
            say(error.empty() ? L"导入文件过大或不可读。" : error);
            return;
        }
        auto text = wide(data.utf8);
        auto next = settings;
        if (legacy && !import_prompt_flow(data.utf8, next, text, error)) {
            say(error);
            return;
        }
        if (text.size() > PromptLimit || text.find(L'\0') != std::wstring::npos) {
            say(L"导入内容过大或包含 NUL 字符。");
            return;
        }
        if (!editor->text(false).empty()) {
            modal = true;
            int choice = MessageBoxW(hwnd, L"用导入内容替换当前提示词草稿？", L"导入提示词",
                                     MB_OKCANCEL | MB_ICONQUESTION);
            modal = false;
            if (choice != IDOK)
                return;
        }
        // Import is one undoable edit. Original Prompt Flow file is never modified.
        SendMessageW(editor->hwnd(), EM_SETSEL, 0, -1);
        SendMessageW(editor->hwnd(), EM_REPLACESEL, TRUE, (LPARAM)text.c_str());
        if (legacy) {
            bool keyOk = shortcut(next.hotkey);
            settings.top = next.top;
            settings.opacity = next.opacity;
            apply_appearance();
            persist_settings();
            if (!keyOk)
                say(hotkeyError);
        }
        dirty = true;
        if (!flush())
            return;
        refresh_preview();
        say(legacy ? L"已导入草稿、置顶与透明度；自启动请在设置中选择。"
                   : L"已载入 Markdown，Ctrl+Z 可撤销替换。");
        if (!hotkeyError.empty())
            say(hotkeyError);
    }
    void action(int id) {
        if (id >= OpacityBase + 30 && id <= OpacityBase + 100) {
            settings.opacity = id - OpacityBase;
            apply_appearance();
            persist_settings();
            return;
        }
        switch (id) {
        case CopyHide:
            hide(true);
            break;
        case CopyOnly:
            copy_prompt();
            break;
        case Clear:
            SendMessageW(editor->hwnd(), EM_SETSEL, 0, -1);
            SendMessageW(editor->hwnd(), EM_REPLACESEL, TRUE, (LPARAM)L"");
            SetFocus(editor->hwnd());
            break;
        case Preview:
            settings.preview = !settings.preview;
            if (!settings.preview) {
                if (cancel)
                    cancel->store(true);
                ++generation;
                view.reset();
                previewSource.clear();
            }
            layout();
            refresh_preview();
            persist_settings();
            break;
        case Options:
            options = !options;
            layout();
            SetFocus(options ? hotkeyEdit : editor->hwnd());
            if (!hotkeyError.empty())
                say(hotkeyError);
            break;
        case Apply:
        case DoubleCtrl:
        case DefaultKey:
            if (shortcut(id == DoubleCtrl   ? L"LCtrl x2"
                         : id == DefaultKey ? L"Ctrl+Alt+Space"
                                            : text_of(hotkeyEdit))) {
                if (persist_settings())
                    say(L"全局快捷键已设置为 " + settings.hotkey);
            }
            break;
        case Resident:
            if (!settings.resident) {
                settings.resident = true;
                tray_icon(true);
            } else {
                std::wstring error;
                if (!set_prompt_startup(startupValue, startupCommand, false, error)) {
                    say(error);
                    break;
                }
                settings.resident = false;
                tray_icon(false);
                PostMessageW(owner, WM_PROMPT_OWNER, 1, 0);
            }
            persist_settings();
            break;
        case Top:
            settings.top = !settings.top;
            apply_appearance();
            persist_settings();
            break;
        case Startup: {
            bool enable = !prompt_startup_enabled(startupValue, startupCommand);
            std::wstring error;
            if (!set_prompt_startup(startupValue, startupCommand, enable, error)) {
                say(error);
                break;
            }
            if (enable) {
                settings.resident = true;
                tray_icon(true);
                persist_settings();
            }
            say(enable ? L"已启用当前用户登录时启动。" : L"已关闭登录时启动。");
            break;
        }
        case ImportMd:
            import_file(false);
            break;
        case ImportLegacy:
            import_file(true);
            break;
        case ExportMd: {
            auto path = dialog(true, false);
            if (path.empty())
                break;
            std::wstring error;
            bool ok = save_file(path, editor->text(false), Encoding::Utf8, error);
            say(ok ? L"已导出 " + path.wstring() : error);
            break;
        }
        case Reader:
            PostMessageW(owner, WM_PROMPT_OWNER, 1, 0);
            break;
        case Quit:
            PostMessageW(owner, WM_PROMPT_OWNER, 2, 0);
            break;
        case Reposition:
            place();
            break;
        }
    }
    bool translate(MSG &msg) {
        if (!hwnd || modal || !(msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd)))
            return false;
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
            bool ime = editor && composing(editor->hwnd());
            if (!ime && (msg.wParam == VK_ESCAPE || (ctrl && msg.wParam == VK_RETURN))) {
                hide(true);
                return true;
            }
            if (!ime && ctrl && shift && msg.wParam == 'C') {
                copy_prompt();
                return true;
            }
            if (!ime && ctrl && msg.wParam == 'S') {
                action(ExportMd);
                return true;
            }
            if (!ime && msg.wParam == VK_F6) {
                action(Preview);
                return true;
            }
            if (msg.hwnd == reader && view && ctrl && msg.wParam == 'C') {
                clipboard_text(hwnd, view->selection());
                return true;
            }
            if (msg.hwnd == reader && view && ctrl && msg.wParam == 'A') {
                view->select_all();
                return true;
            }
            if (msg.hwnd == hotkeyEdit && msg.wParam == VK_RETURN) {
                action(Apply);
                return true;
            }
            if (msg.wParam == VK_TAB && (ctrl || msg.hwnd != editor->hwnd())) {
                auto next = GetNextDlgTabItem(hwnd, GetFocus(), shift);
                if (next)
                    SetFocus(next);
                return true;
            }
        }
        // Do not let reader accelerators consume keys intended for the prompt editor.
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        return true;
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto p = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            p = (Impl *)((CREATESTRUCTW *)l)->lpCreateParams;
            p->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
        }
        if (!p)
            return DefWindowProcW(h, m, w, l);
        if (m == p->taskbar && p->settings.resident) {
            p->tray = false;
            p->tray_icon(true);
            return 0;
        }
        switch (m) {
        case WM_COMMAND:
            if (LOWORD(w) == EditId && HIWORD(w) == EN_CHANGE && !p->loading && p->editor) {
                p->dirty = true;
                SetTimer(h, 1, 250, nullptr);
                SetTimer(h, 2, 800, nullptr);
            } else if (LOWORD(w) == EditId && HIWORD(w) == EN_MAXTEXT)
                p->say(L"提示词达到 1 Mi UTF-16 字符上限，未加入后续输入。");
            else if (HIWORD(w) == 0)
                p->action(LOWORD(w));
            return 0;
        case WM_INITMENUPOPUP:
            p->update_menu();
            return 0;
        case WM_SIZE:
            p->layout();
            return 0;
        case WM_DPICHANGED: {
            p->update_font();
            auto r = (RECT *)l;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            p->layout();
            return 0;
        }
        case WM_GETMINMAXINFO:
            ((MINMAXINFO *)l)->ptMinTrackSize = {(LONG)(680 * p->dpi), (LONG)(360 * p->dpi)};
            return 0;
        case WM_SETFOCUS:
            if (p->editor)
                SetFocus(p->editor->hwnd());
            return 0;
        case WM_TIMER:
            KillTimer(h, w);
            if (w == 1)
                p->refresh_preview();
            if (w == 2 && p->flush())
                p->refresh_preview();
            return 0;
        case PreviewReady: {
            std::shared_ptr<Document> doc;
            {
                std::lock_guard lock(p->resultMutex);
                if (p->resultVersion == p->generation)
                    doc = std::move(p->result);
                else
                    p->result.reset();
            }
            if (doc && p->view && IsWindowVisible(h)) {
                auto anchor = p->view->anchor_block();
                p->view->set_document(std::move(doc), p->draftPath);
                p->view->goto_block(anchor);
            }
            return 0;
        }
        case Summon:
        case WM_HOTKEY:
            if (p->modal)
                return 0;
            if (m == Summon && (w & 2)) {
                p->settings.resident = true;
                p->tray_icon(true);
                p->persist_settings();
                if (!(w & 1))
                    return 0;
            }
            if (m == Summon && (w & 1))
                p->show();
            else if (IsWindowVisible(h) && GetForegroundWindow() == h)
                p->hide(true);
            else
                p->show();
            return 0;
        case WM_INPUT: {
            RAWINPUT input{};
            UINT size = sizeof(input);
            if (p->raw &&
                GetRawInputData((HRAWINPUT)l, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                input.header.dwType == RIM_TYPEKEYBOARD) {
                auto &key = input.data.keyboard;
                bool left = key.VKey == VK_CONTROL && !(key.Flags & RI_KEY_E0);
                bool down = !(key.Flags & RI_KEY_BREAK);
                if (!left && key.VKey < p->otherKeys.size()) {
                    auto &held = p->otherKeys[key.VKey];
                    if (held != down) {
                        if (down)
                            ++p->otherHeld;
                        else if (p->otherHeld)
                            --p->otherHeld;
                        held = down;
                    }
                }
                if (p->taps.event(left, down, GetTickCount64(), p->otherHeld != 0))
                    PostMessageW(h, Summon, 0, 0);
            }
            // DefWindowProc performs the RIM_INPUT cleanup required by Windows.
            return DefWindowProcW(h, m, w, l);
        }
        case Tray:
            if (l == WM_LBUTTONUP || l == NIN_KEYSELECT)
                p->show();
            else if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
                auto menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"输入 Markdown 提示词");
                AppendMenuW(menu, MF_STRING, Reader, L"打开阅读器");
                AppendMenuW(menu, MF_STRING, Quit, L"退出 KeepMD");
                POINT pt{};
                GetCursorPos(&pt);
                SetForegroundWindow(h);
                int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
                DestroyMenu(menu);
                PostMessageW(h, WM_NULL, 0, 0);
                if (id == 1)
                    p->show();
                else if (id)
                    p->action(id);
            }
            return 0;
        case WM_QUERYENDSESSION:
            return p->flush();
        case WM_CLOSE:
            p->hide(false);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    static LRESULT CALLBACK preview_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto p = (Impl *)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (m == WM_NCCREATE) {
            p = (Impl *)((CREATESTRUCTW *)l)->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
        }
        if (!p || !p->view) {
            if (m == WM_PAINT) {
                PAINTSTRUCT ps;
                BeginPaint(h, &ps);
                EndPaint(h, &ps);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
        }
        auto &v = *p->view;
        float scale = v.dpi_scale();
        switch (m) {
        case WM_PAINT:
            v.paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            v.resize();
            return 0;
        case WM_ASSET_READY:
            v.collect_assets();
            return 0;
        case WM_MOUSEWHEEL:
            v.scroll(-GET_WHEEL_DELTA_WPARAM(w) / 120.f * 90);
            return 0;
        case WM_MOUSEHWHEEL:
            v.hscroll(GET_WHEEL_DELTA_WPARAM(w) / 120.f * 90);
            return 0;
        case WM_LBUTTONDOWN:
            v.mouse_down(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale, GetKeyState(VK_SHIFT) < 0);
            return 0;
        case WM_MOUSEMOVE:
            v.mouse_move(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale);
            return 0;
        case WM_LBUTTONUP:
            v.mouse_up(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale);
            return 0;
        case WM_LBUTTONDBLCLK:
            v.toggle_diagram(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale);
            return 0;
        case WM_CAPTURECHANGED:
            v.cancel_drag();
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(h, SB_VERT, &info);
            switch (LOWORD(w)) {
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
                v.scroll_to((float)info.nTrackPos);
                break;
            case SB_LINEUP:
                v.scroll(-40);
                break;
            case SB_LINEDOWN:
                v.scroll(40);
                break;
            case SB_PAGEUP:
                v.scroll(-v.page_height());
                break;
            case SB_PAGEDOWN:
                v.scroll(v.page_height());
                break;
            case SB_TOP:
                v.scroll_to(0);
                break;
            case SB_BOTTOM:
                v.scroll_to(v.total_height());
                break;
            }
            return 0;
        }
        case WM_HSCROLL: {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(h, SB_HORZ, &info);
            if (LOWORD(w) == SB_THUMBTRACK || LOWORD(w) == SB_THUMBPOSITION)
                v.hscroll((float)info.nTrackPos - v.scroll_x());
            else
                v.hscroll(LOWORD(w) == SB_LINELEFT || LOWORD(w) == SB_PAGELEFT ? -60.f : 60.f);
            return 0;
        }
        case WM_KEYDOWN:
            switch (w) {
            case VK_UP:
                v.scroll(-40);
                break;
            case VK_DOWN:
                v.scroll(40);
                break;
            case VK_PRIOR:
                v.scroll(-v.page_height());
                break;
            case VK_NEXT:
                v.scroll(v.page_height());
                break;
            case VK_HOME:
                v.scroll_to(0);
                break;
            case VK_END:
                v.scroll_to(v.total_height());
                break;
            }
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};
PromptWindow::PromptWindow(HWND owner, const std::filesystem::path &config, bool dark)
    : p_(std::make_unique<Impl>(owner, config, dark)) {}
PromptWindow::~PromptWindow() = default;
bool PromptWindow::start(bool resident, bool show) {
    return p_->start(resident, show);
}
void PromptWindow::show() {
    p_->start(false, true);
}
bool PromptWindow::resident() const {
    return p_->settings.resident && p_->hwnd;
}
bool PromptWindow::flush() {
    return p_->prepare_close();
}
void PromptWindow::theme(bool dark) {
    p_->dark = dark;
    p_->apply_appearance();
}
bool PromptWindow::translate(MSG &message) {
    return p_->translate(message);
}
} // namespace keepmd
