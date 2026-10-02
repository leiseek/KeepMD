#include "prompt_window.h"
#include "document.h"
#include "file_io.h"
#include "prompt_model.h"
#include "ui.h"
#include "visual_editor.h"

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
constexpr UINT Summon = WM_APP + 31, Tray = WM_APP + 32;
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
    Theme,
    ToggleToolbar,
    FormatText = 540,
    FormatH1,
    FormatH2,
    FormatBold,
    FormatItalic,
    FormatBullet,
    FormatNumber,
    FormatQuote,
    FormatCode,
    FormatDiagram,
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
} // namespace
struct PromptWindow::Impl {
    HWND owner = nullptr, hwnd = nullptr, previous = nullptr, status = nullptr, hotkeyEdit = nullptr,
         hotkeyLabel = nullptr;
    DWORD previousPid = 0;
    HANDLE mutex = nullptr;
    HFONT font = nullptr, titleFont = nullptr, smallFont = nullptr;
    HBRUSH backgroundBrush = nullptr, surfaceBrush = nullptr;
    RECT sourceCard{};
    std::vector<POINT> formatSeparators;
    bool placed = false;
    HMENU menu = nullptr, prefsMenu = nullptr, opacityMenu = nullptr;
    std::vector<std::pair<int, HWND>> buttons;
    std::unique_ptr<VisualEditor> editor;
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
        if (hwnd) {
            stop_shortcut();
            tray_icon(false);
            editor.reset();
            DestroyWindow(hwnd);
        }
        if (font)
            DeleteObject(font);
        if (titleFont)
            DeleteObject(titleFont);
        if (smallFont)
            DeleteObject(smallFont);
        if (backgroundBrush)
            DeleteObject(backgroundBrush);
        if (surfaceBrush)
            DeleteObject(surfaceBrush);
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
            wide(data.utf8).size() > PromptLimit ||
            (data.utf8.find('\0') != std::string::npos ||
             data.utf8.find("\xef\xbf\xbc") != std::string::npos)) {
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
        if (editor && editor->composing()) {
            SetTimer(hwnd, 2, 800, nullptr);
            return false;
        }
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
        if (editor && editor->composing()) {
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
        AppendMenuW(prefsMenu, MF_STRING, ToggleToolbar, L"显示格式工具栏\tCtrl+Shift+T");
        AppendMenuW(prefsMenu, MF_STRING, Resident, L"关闭阅读器后常驻托盘");
        AppendMenuW(prefsMenu, MF_STRING, Startup, L"登录 Windows 时启动");
        AppendMenuW(prefsMenu, MF_STRING, Top, L"窗口置顶");
        for (int i : {100, 90, 80, 70, 60, 50, 40, 30}) {
            auto label = std::to_wstring(i) + L"%";
            AppendMenuW(opacityMenu, MF_STRING, OpacityBase + i, label.c_str());
        }
        AppendMenuW(prefsMenu, MF_POPUP, (UINT_PTR)opacityMenu, L"不透明度");
        AppendMenuW(prefsMenu, MF_STRING, Reposition, L"移到当前屏幕右侧");
        AppendMenuW(prefsMenu, MF_STRING, Theme, L"切换深浅主题");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)prefsMenu, L"设置");
        ui::menu_labels(menu, {L"文件", L"设置"});
        ui::caption_menu(hwnd, menu);
        for (const auto &[id, label] :
             std::vector<std::pair<int, const wchar_t *>>{{CopyHide, L"复制并收起   Ctrl+Enter"},
                                                          {CopyOnly, L"复制全文"},
                                                          {Clear, L"清空"},
                                                          {Options, L"快捷键"},
                                                          {Theme, L"深色"},
                                                          {Apply, L"应用"},
                                                          {DoubleCtrl, L"双击左 Ctrl"},
                                                          {DefaultKey, L"默认"},
                                                          {FormatText, L"正文"},
                                                          {FormatH1, L"标题 1"},
                                                          {FormatH2, L"标题 2"},
                                                          {FormatBold, L"加粗"},
                                                          {FormatItalic, L"斜体"},
                                                          {FormatBullet, L"列表"},
                                                          {FormatNumber, L"编号"},
                                                          {FormatQuote, L"引用"},
                                                          {FormatCode, L"代码"},
                                                          {FormatDiagram, L"流程图"}}) {
            auto h = child(L"BUTTON", label, id, WS_TABSTOP);
            ui::style_button(h, id == CopyHide || id == Apply);
            using ui::Icon;
            switch (id) {
            case FormatText:
                ui::icon_button(h, Icon::Text, L"正文 · 将当前段落设为正文");
                break;
            case FormatH1:
                ui::icon_button(h, Icon::H1, L"一级标题");
                break;
            case FormatH2:
                ui::icon_button(h, Icon::H2, L"二级标题");
                break;
            case FormatBold:
                ui::icon_button(h, Icon::Bold, L"加粗 · Ctrl+B");
                break;
            case FormatItalic:
                ui::icon_button(h, Icon::Italic, L"斜体 · Ctrl+I");
                break;
            case FormatBullet:
                ui::icon_button(h, Icon::Bullet, L"无序列表");
                break;
            case FormatNumber:
                ui::icon_button(h, Icon::Numbered, L"有序列表");
                break;
            case FormatQuote:
                ui::icon_button(h, Icon::Quote, L"引用");
                break;
            case FormatCode:
                ui::icon_button(h, Icon::Code, L"行内代码");
                break;
            case FormatDiagram:
                ui::icon_button(h, Icon::Diagram, L"插入或修改 Mermaid 流程图");
                break;
            case Theme:
                ui::icon_button(h, dark ? Icon::Sun : Icon::Moon, L"切换深浅主题");
                break;
            case Options:
                ui::icon_button(h, Icon::Keyboard, L"全局快捷键设置");
                break;
            case Clear:
                ui::icon_button(h, Icon::Trash, L"清空提示词 · Ctrl+Z 可撤销");
                break;
            case CopyOnly:
                ui::icon_button(h, Icon::Copy, L"复制完整 Markdown · Ctrl+Shift+C");
                break;
            case CopyHide:
                ui::icon_button(h, Icon::CopyHide, L"复制 Markdown 并收起 · Ctrl+Enter / Esc", true);
                SetWindowTextW(h, L"复制并收起");
                break;
            case Apply:
                ui::icon_button(h, Icon::Check, L"应用全局快捷键");
                break;
            case DefaultKey:
                ui::icon_button(h, Icon::Reset, L"恢复默认快捷键 Ctrl+Alt+Space");
                break;
            }
            buttons.emplace_back(id, h);
        }
        hotkeyLabel = child(L"STATIC", L"全局快捷键", 0);
        hotkeyEdit =
            child(L"EDIT", settings.hotkey.c_str(), HotkeyId, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
        SendMessageW(hotkeyEdit, EM_SETLIMITTEXT, 100, 0);
        status = child(L"STATIC", L"", StatusId, SS_LEFTNOWORDWRAP);
        editor = std::make_unique<VisualEditor>(hwnd, EditId);
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
        update_font();
        apply_appearance();
        if (draftBlocked)
            EnableWindow(editor->hwnd(), FALSE);
    }
    void update_font() {
        auto old = font;
        dpi = (float)GetDpiForWindow(hwnd) / 96;
        font = ui::font(GetDpiForWindow(hwnd));
        if (titleFont)
            DeleteObject(titleFont);
        if (smallFont)
            DeleteObject(smallFont);
        titleFont = ui::font(GetDpiForWindow(hwnd), 25, FW_SEMIBOLD);
        smallFont = ui::font(GetDpiForWindow(hwnd), 13);
        for (const auto &[id, h] : buttons)
            SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        for (HWND h : {hotkeyEdit, hotkeyLabel})
            if (h)
                SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        if (status)
            SendMessageW(status, WM_SETFONT, (WPARAM)smallFont, TRUE);
        if (old)
            DeleteObject(old);
    }
    void apply_appearance() {
        if (!hwnd)
            return;
        SetWindowPos(hwnd, settings.top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetLayeredWindowAttributes(hwnd, 0, (BYTE)(settings.opacity * 255 / 100), LWA_ALPHA);
        auto colors = ui::palette(dark);
        if (backgroundBrush)
            DeleteObject(backgroundBrush);
        if (surfaceBrush)
            DeleteObject(surfaceBrush);
        backgroundBrush = CreateSolidBrush(colors.background);
        surfaceBrush = CreateSolidBrush(colors.surface);
        for (const auto &[id, h] : buttons) {
            ui::style_button(h, id == CopyHide || id == Apply,
                             id == Preview   ? settings.preview
                             : id == Options ? options
                                             : false);
            if (id == Theme) {
                SetWindowTextW(h, dark ? L"浅色" : L"深色");
                ui::set_button_icon(h, dark ? ui::Icon::Sun : ui::Icon::Moon);
            }
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        for (HWND h : {status, hotkeyEdit, hotkeyLabel})
            if (h)
                InvalidateRect(h, nullptr, TRUE);
        ui::titlebar(hwnd, dark);
        ui::caption_toolbar(hwnd, settings.toolbar);
        if (menu) {
            MENUINFO mi{sizeof(mi)};
            mi.fMask = MIM_BACKGROUND;
            mi.hbrBack = backgroundBrush;
            SetMenuInfo(menu, &mi);
            DrawMenuBar(hwnd);
        }
        if (editor) {
            bool wasLoading = loading;
            loading = true;
            editor->theme(dark);
            loading = wasLoading;
        }
    }
    HWND button(int id) const {
        for (const auto &[key, h] : buttons)
            if (key == id)
                return h;
        return nullptr;
    }
    void layout() {
        if (!editor)
            return;
        RECT r{};
        GetClientRect(hwnd, &r);
        auto px = [&](int n) { return (int)(n * dpi); };
        const int margin = px(24), w = r.right, caption = ui::caption_height(hwnd);
        auto move = [&](int id, int x, int y, int width, int height = 36) {
            MoveWindow(button(id), x, y, width, px(height), TRUE);
        };
        move(Theme, w - margin - px(38), caption + px(18), px(38));
        move(Options, w - margin - px(84), caption + px(18), px(38));
        for (int id : {Apply, DoubleCtrl, DefaultKey})
            ShowWindow(button(id), options ? SW_SHOW : SW_HIDE);
        ShowWindow(hotkeyEdit, options ? SW_SHOW : SW_HIDE);
        ShowWindow(hotkeyLabel, options ? SW_SHOW : SW_HIDE);
        int fieldWidth = std::max(px(110), w - margin * 2 - px(404));
        MoveWindow(hotkeyLabel, margin, caption + px(80), px(92), px(24), TRUE);
        MoveWindow(hotkeyEdit, margin + px(96), caption + px(74), fieldWidth, px(33), TRUE);
        int x = margin + px(104) + fieldWidth;
        move(Apply, x, caption + px(73), px(64));
        move(DoubleCtrl, x + px(72), caption + px(73), px(120));
        move(DefaultKey, x + px(200), caption + px(73), px(76));
        int formatY = caption + px(options ? 127 : 68);
        int xFormat = margin, row = 0;
        formatSeparators.clear();
        for (int id : {FormatText, FormatH1, FormatH2, FormatBold, FormatItalic, FormatBullet, FormatNumber,
                       FormatQuote, FormatCode, FormatDiagram}) {
            ShowWindow(button(id), settings.toolbar ? SW_SHOWNA : SW_HIDE);
            if (!settings.toolbar)
                continue;
            int width = px(38);
            if (xFormat + width > w - margin) {
                xFormat = margin;
                ++row;
            }
            move(id, xFormat, formatY + row * px(40), width, 36);
            xFormat += width + px(4);
            if (id == FormatH2 || id == FormatItalic || id == FormatQuote) {
                formatSeparators.push_back({xFormat + px(4), formatY + row * px(40)});
                xFormat += px(12);
            }
        }
        int top = formatY + (settings.toolbar ? (row + 1) * px(40) : 0) + px(8),
            bottom = std::max<int>(top + px(80), r.bottom - px(106));
        sourceCard = {margin, top, w - margin, bottom};
        MoveWindow(editor->hwnd(), margin + 1, top + 1, w - margin * 2 - 2, bottom - top - 2, TRUE);
        editor->inset(GetDpiForWindow(hwnd));
        MoveWindow(status, margin, r.bottom - px(92), w - margin * 2, px(25), TRUE);
        move(Clear, margin, r.bottom - px(54), px(38));
        move(CopyOnly, w - margin - px(224), r.bottom - px(54), px(38));
        move(CopyHide, w - margin - px(172), r.bottom - px(54), px(172));
        ui::style_button(button(Options), false, options);
        ui::caption_toolbar(hwnd, settings.toolbar);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void paint() {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(hwnd, &ps);
        auto colors = ui::palette(dark);
        RECT r{};
        GetClientRect(hwnd, &r);
        ui::fill(dc, r, colors.background);
        auto px = [&](int n) { return (int)(n * dpi); };
        int caption = ui::caption_height(hwnd);
        for (auto point : formatSeparators)
            ui::line(dc, point.x, point.y + px(9), point.x, point.y + px(27), colors.border);
        ui::text(dc, titleFont, L"提示词", {px(24), caption + px(15), r.right - px(310), caption + px(48)},
                 colors.text);
        if (sourceCard.bottom > sourceCard.top)
            ui::rounded(dc, sourceCard, colors.surface, colors.border, px(12));
        if (options)
            ui::line(dc, px(24), caption + px(119), r.right - px(24), caption + px(119), colors.border);
        EndPaint(hwnd, &ps);
    }
    void place() {
        auto monitor = MonitorFromWindow(previous ? previous : owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        auto r = info.rcWork;
        int w = std::min<int>((int)(1040 * dpi), r.right - r.left),
            h = std::min<int>((int)(700 * dpi), r.bottom - r.top);
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
        if (!placed) {
            place();
            placed = true;
        }
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
        if (editor && editor->composing()) {
            say(L"请先完成或取消输入法组词，再复制或收起。");
            return false;
        }
        if (copy && !copy_prompt())
            return false;
        if (!flush())
            return false;
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
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
        say(L"已复制 Markdown · " + std::to_wstring(text.size()) + L" 字符");
        return true;
    }
    void refresh_preview() {
        if (!editor || !IsWindowVisible(hwnd))
            return;
        editor->refresh();
        auto text = editor->text(false);
        auto lines = text.empty() ? 0 : 1 + std::count(text.begin(), text.end(), L'\n');
        say(std::to_wstring(text.size()) + L" 字符 · " + std::to_wstring(lines) + L" 行    ·    " +
            (dirty ? L"正在保存…" : L"草稿已保存"));
    }
    void update_menu() {
        if (!prefsMenu)
            return;
        for (auto [id, checked] : {std::pair{Resident, settings.resident},
                                   {ToggleToolbar, settings.toolbar},
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
        if (text.size() > PromptLimit ||
            (text.find(L'\0') != std::wstring::npos || text.find(L'\ufffc') != std::wstring::npos)) {
            say(L"导入内容过大或包含保留控制字符（NUL / 对象占位符），未替换当前草稿。");
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
        editor->replace_document(text);
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
        say(legacy ? L"已导入草稿、置顶与透明度；自启动请在设置中选择。" : L"已载入 Markdown。");
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
        case ToggleToolbar:
            settings.toolbar = !settings.toolbar;
            layout();
            persist_settings();
            SetFocus(editor->hwnd());
            break;
        case CopyHide:
            hide(true);
            break;
        case CopyOnly:
            copy_prompt();
            break;
        case Clear:
            editor->clear();
            break;
        case FormatText:
            editor->command(VisualCommand::Paragraph);
            break;
        case FormatH1:
            editor->command(VisualCommand::H1);
            break;
        case FormatH2:
            editor->command(VisualCommand::H2);
            break;
        case FormatBold:
            editor->command(VisualCommand::Bold);
            break;
        case FormatItalic:
            editor->command(VisualCommand::Italic);
            break;
        case FormatBullet:
            editor->command(VisualCommand::Bullet);
            break;
        case FormatNumber:
            editor->command(VisualCommand::Numbered);
            break;
        case FormatQuote:
            editor->command(VisualCommand::Quote);
            break;
        case FormatCode:
            editor->command(VisualCommand::Code);
            break;
        case FormatDiagram:
            editor->command(VisualCommand::Diagram);
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
        case Theme:
            PostMessageW(owner, WM_PROMPT_OWNER, 3, !dark);
            break;
        }
    }
    bool translate(MSG &msg) {
        if (!hwnd || modal || !(msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd)))
            return false;
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
            bool ime = editor && editor->composing();
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
        case ui::WM_TOGGLE_TOOLBAR:
            if (p->editor && !p->modal)
                p->action(ToggleToolbar);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            p->paint();
            return 0;
        case WM_DRAWITEM:
            if (((DRAWITEMSTRUCT *)l)->CtlType == ODT_BUTTON) {
                ui::draw_button(*(DRAWITEMSTRUCT *)l, p->dark);
                return TRUE;
            }
            if (((DRAWITEMSTRUCT *)l)->CtlType == ODT_MENU) {
                ui::draw_menu(*(DRAWITEMSTRUCT *)l, p->font, p->dark);
                return TRUE;
            }
            break;
        case WM_MEASUREITEM:
            if (((MEASUREITEMSTRUCT *)l)->CtlType == ODT_MENU) {
                ui::measure_menu(*(MEASUREITEMSTRUCT *)l, h, p->font);
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            auto colors = ui::palette(p->dark);
            auto dc = (HDC)w;
            bool input = (HWND)l == p->hotkeyEdit;
            SetTextColor(dc, input ? colors.text : colors.muted);
            SetBkColor(dc, input ? colors.surface : colors.background);
            return (LRESULT)(input ? p->surfaceBrush : p->backgroundBrush);
        }
        case WM_SETTINGCHANGE:
            p->apply_appearance();
            return 0;
        case WM_COMMAND:
            if (LOWORD(w) == EditId && HIWORD(w) == EN_CHANGE && !p->loading && p->editor &&
                !p->editor->internal()) {
                p->dirty = true;
                p->editor->changed();
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
            ((MINMAXINFO *)l)->ptMinTrackSize = {(LONG)(720 * p->dpi), (LONG)(480 * p->dpi)};
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
