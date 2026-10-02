#include "editor.h"
#include "file_io.h"
#include "prompt_window.h"
#include "settings.h"
#include "shell_integration.h"
#include "view.h"
#include <algorithm>
#include <chrono>
#include <commctrl.h>
#include <commdlg.h>
#include <fstream>
#include <imm.h>
#include <psapi.h>
#include <shellapi.h>
#include <sstream>
#include <windowsx.h>

using namespace keepmd;
namespace {
constexpr UINT WM_DOCUMENT_READY = WM_APP + 12;
enum Command {
    Open = 100,
    Exit,
    Find,
    FindNext,
    FindPrevious,
    Toc,
    Theme,
    ZoomIn,
    ZoomOut,
    ZoomReset,
    Copy,
    SelectAll,
    Reload,
    About,
    Back,
    Forward,
    Width,
    Edit,
    Save,
    SaveAs,
    New,
    Replace,
    ReplaceOne,
    ReplaceAll,
    Split,
    RegisterOpenWith,
    Prompt
};
constexpr int SearchId = 200, TocId = 201, EditorId = 202;
const auto processStart = std::chrono::steady_clock::now();
struct LoadResult {
    uint64_t generation = 0;
    std::shared_ptr<Document> doc;
    FileData file;
    std::filesystem::path path;
    std::wstring error;
    bool refresh = false, preview = false;
    size_t cursorSource = 0;
};
struct App {
    HWND hwnd = nullptr, reader = nullptr, toolbar = nullptr, status = nullptr, searchBox = nullptr,
         searchNext = nullptr, searchPrev = nullptr, toc = nullptr, replaceBox = nullptr,
         replaceOne = nullptr, replaceAll = nullptr;
    HFONT font = nullptr;
    View *view = nullptr;
    float dpi = 1;
    bool showToc = false, showSearch = false, loading = false;
    std::filesystem::path path, snapshot, report;
    FileData file;
    uint64_t generation = 0;
    std::shared_ptr<std::atomic_bool> cancel;
    std::mutex mutex;
    std::vector<LoadResult> results;
    std::unique_ptr<Worker> loader;
    std::vector<Match> matches;
    int match = -1;
    std::vector<std::pair<std::filesystem::path, size_t>> history;
    int historyIndex = -1, pendingHistoryIndex = -1;
    bool navigating = false;
    float restoreScroll = 0;
    size_t restoreBlock = 0;
    std::wstring pendingAnchor;
    bool firstPaint = false;
    double firstPaintMs = 0;
    int64_t firstPaintQpc = 0;
    int autoExitMs = 0;
    uint64_t openedAt = 0;
    std::filesystem::path settingsPath;
    ReaderSettings preferences;
    std::unique_ptr<PromptWindow> prompt;
    bool forceExit = false, skipSettingsSave = false;
    unsigned renderDpi = 0;
    HMENU recentMenu = nullptr;
    std::unique_ptr<Editor> editor;
    bool suppressEdit = false, showReplace = false, split = true;
    std::wstring title() const {
        return (editor && editor->modified() ? L"* " : L"") +
               (path.empty() ? L"KeepMD" : path.filename().wstring() + L" — KeepMD");
    }
    ~App() {
        if (cancel)
            cancel->store(true);
        loader.reset();
        if (font)
            DeleteObject(font);
    }
};
App *app_of(HWND hwnd) {
    return reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}
bool is_mmd(const std::filesystem::path &path) {
    return _wcsicmp(path.extension().c_str(), L".mmd") == 0;
}
void status(App &app, const std::wstring &text) {
    SendMessageW(app.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}
void update_ui_font(App &app, unsigned dpi) {
    HFONT old = app.font;
    app.font =
        CreateFontW(-MulDiv(16, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND child : {app.toolbar, app.status, app.searchBox, app.searchNext, app.searchPrev, app.replaceBox,
                       app.replaceOne, app.replaceAll, app.toc})
        if (child)
            SendMessageW(child, WM_SETFONT, (WPARAM)app.font, TRUE);
    if (app.status)
        SendMessageW(app.status, SB_SETMINHEIGHT, MulDiv(20, dpi, 96), 0);
    if (old)
        DeleteObject(old);
}
void copy_text(HWND owner, const std::wstring &text) {
    if (text.empty() || !OpenClipboard(owner))
        return;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (memory) {
        void *ptr = GlobalLock(memory);
        if (ptr) {
            memcpy(ptr, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        } else
            GlobalFree(memory);
    }
    CloseClipboard();
}
std::wstring control_text(HWND hwnd) {
    int n = GetWindowTextLengthW(hwnd);
    std::wstring text(n + 1, 0);
    GetWindowTextW(hwnd, text.data(), n + 1);
    text.resize(n);
    return text;
}
void arrange(App &app) {
    RECT r{};
    GetClientRect(app.hwnd, &r);
    app.dpi = (float)GetDpiForWindow(app.hwnd) / 96.f;
    int top = (int)(42 * app.dpi), bottom = (int)(24 * app.dpi),
        side = app.showToc ? (int)(220 * app.dpi) : 0;
    SendMessageW(app.status, WM_SIZE, 0, 0);
    MoveWindow(app.toolbar, 0, 0, r.right, top, TRUE);
    int searchHeight = app.showSearch ? (int)((app.showReplace ? 76 : 38) * app.dpi) : 0;
    ShowWindow(app.searchBox, app.showSearch ? SW_SHOW : SW_HIDE);
    ShowWindow(app.searchNext, app.showSearch ? SW_SHOW : SW_HIDE);
    ShowWindow(app.searchPrev, app.showSearch ? SW_SHOW : SW_HIDE);
    MoveWindow(app.searchBox, (int)(12 * app.dpi), top + (int)(5 * app.dpi),
               std::max<int>(150, r.right - (int)(240 * app.dpi)), (int)(27 * app.dpi), TRUE);
    MoveWindow(app.searchPrev, r.right - (int)(215 * app.dpi), top + (int)(4 * app.dpi), (int)(98 * app.dpi),
               (int)(29 * app.dpi), TRUE);
    MoveWindow(app.searchNext, r.right - (int)(112 * app.dpi), top + (int)(4 * app.dpi), (int)(98 * app.dpi),
               (int)(29 * app.dpi), TRUE);
    for (HWND child : {app.replaceBox, app.replaceOne, app.replaceAll})
        ShowWindow(child, app.showSearch && app.showReplace ? SW_SHOW : SW_HIDE);
    MoveWindow(app.replaceBox, (int)(12 * app.dpi), top + (int)(43 * app.dpi),
               std::max<int>(150, r.right - (int)(240 * app.dpi)), (int)(27 * app.dpi), TRUE);
    MoveWindow(app.replaceOne, r.right - (int)(215 * app.dpi), top + (int)(42 * app.dpi), (int)(98 * app.dpi),
               (int)(29 * app.dpi), TRUE);
    MoveWindow(app.replaceAll, r.right - (int)(112 * app.dpi), top + (int)(42 * app.dpi), (int)(98 * app.dpi),
               (int)(29 * app.dpi), TRUE);
    if (app.toc) {
        ShowWindow(app.toc, app.showToc ? SW_SHOW : SW_HIDE);
        MoveWindow(app.toc, 0, top + searchHeight, side,
                   std::max<int>(1, r.bottom - top - searchHeight - bottom), TRUE);
        ListView_SetColumnWidth(app.toc, 0, std::max(1, side - (int)(20 * app.dpi)));
    }
    int editorWidth =
        app.editor ? (app.split ? std::max<int>(280, (r.right - side) / 2) : r.right - side) : 0;
    if (app.editor)
        MoveWindow(app.editor->hwnd(), side, top + searchHeight, editorWidth,
                   std::max<int>(1, r.bottom - top - searchHeight - bottom), TRUE);
    ShowWindow(app.reader, !app.editor || app.split ? SW_SHOW : SW_HIDE);
    MoveWindow(app.reader, side + editorWidth, top + searchHeight,
               std::max<int>(1, r.right - side - editorWidth),
               std::max<int>(1, r.bottom - top - searchHeight - bottom), TRUE);
}
bool save_editor(App &app, bool saveAs = false);
void update_recent(App &app) {
    while (GetMenuItemCount(app.recentMenu) > 0)
        DeleteMenu(app.recentMenu, 0, MF_BYPOSITION);
    for (size_t i = 0; i < app.preferences.recent.size(); ++i) {
        auto title = L"&" + std::to_wstring(i + 1) + L" " + app.preferences.recent[i].path.wstring();
        AppendMenuW(app.recentMenu, MF_STRING, 300 + (UINT)i, title.c_str());
    }
    if (app.preferences.recent.empty())
        AppendMenuW(app.recentMenu, MF_STRING | MF_GRAYED, 0, L"暂无最近文件");
}
void update_controls(App &app) {
    SendMessageW(app.toolbar, TB_ENABLEBUTTON, Save, MAKELONG(app.editor != nullptr, 0));
    SendMessageW(app.toolbar, TB_ENABLEBUTTON, Split, MAKELONG(app.editor != nullptr, 0));
    TBBUTTONINFOW info{sizeof(info), TBIF_TEXT};
    info.pszText = const_cast<wchar_t *>(app.editor ? L"阅读" : L"编辑");
    SendMessageW(app.toolbar, TB_SETBUTTONINFOW, Edit, reinterpret_cast<LPARAM>(&info));
}
bool allow_navigation(App &app) {
    bool discarded = false;
    if (app.editor && app.editor->modified()) {
        int choice =
            MessageBoxW(app.hwnd, L"保存当前 Markdown 更改？", L"KeepMD", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL)
            return false;
        if (choice == IDYES && !save_editor(app))
            return false;
        discarded = choice == IDNO;
    }
    if (app.editor) {
        KillTimer(app.hwnd, 3);
        app.editor.reset();
        SetFocus(app.reader);
        if (discarded && app.path.empty()) {
            if (app.cancel)
                app.cancel->store(true);
            ++app.generation;
            app.view->set_document(parse_document(""), {});
        }
        app.showReplace = false;
        arrange(app);
        update_controls(app);
        SetWindowTextW(app.hwnd, app.title().c_str());
    }
    return true;
}
void queue_preview(App &app) {
    if (!app.editor)
        return;
    auto text = app.editor->text(false);
    size_t cursorSource =
        utf8(std::wstring_view(text).substr(0, std::min(text.size(), app.editor->caret()))).size();
    auto source = utf8(text);
    if (app.cancel)
        app.cancel->store(true);
    if (!app.loader)
        app.loader = std::make_unique<Worker>();
    app.loader->clear();
    app.cancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = app.cancel;
    auto version = ++app.generation;
    auto path = app.path;
    app.loader->push([&app, source = std::move(source), cancel, version, path, cursorSource]() mutable {
        LoadResult result;
        result.preview = true;
        result.generation = version;
        result.path = path;
        result.cursorSource = cursorSource + (is_mmd(path) ? 11 : 0);
        try {
            if (is_mmd(path))
                source = "```mermaid\n" + source + "\n```\n";
            result.doc = parse_document(std::move(source), cancel.get());
            result.error = result.doc->error;
        } catch (...) {
            result.error = L"预览更新失败";
        }
        if (cancel->load())
            return;
        {
            std::lock_guard lock(app.mutex);
            app.results.push_back(std::move(result));
        }
        PostMessageW(app.hwnd, WM_DOCUMENT_READY, 0, 0);
    });
}
void toggle_editor(App &app) {
    if (app.loading) {
        status(app, L"文件仍在读取，完成后可进入编辑。");
        return;
    }
    if (app.editor) {
        if (!allow_navigation(app))
            return;
        if (!app.path.empty()) {
            app.navigating = true;
            app.restoreScroll = app.view->scroll_y();
        }
        return;
    }
    std::string source;
    auto doc = app.view->document();
    if (doc)
        source = is_mmd(app.path) ? app.file.utf8 : doc->source;
    if (source.size() > 8 * 1024 * 1024) {
        MessageBoxW(app.hwnd, L"当前轻量编辑模式支持 8 MiB 以内的源码。此文件仍可继续阅读。", L"KeepMD",
                    MB_OK);
        return;
    }
    if (source.find('\0') != std::string::npos) {
        status(app, L"文件包含 NUL 字符，保留阅读模式以避免编辑截断。");
        return;
    }
    app.suppressEdit = true;
    app.editor = std::make_unique<Editor>(app.hwnd, EditorId);
    if (!app.editor->valid()) {
        app.editor.reset();
        app.suppressEdit = false;
        status(app, L"无法创建 Windows 原生编辑控件");
        return;
    }
    app.editor->load(wide(source));
    app.editor->theme(app.view->dark());
    app.suppressEdit = false;
    arrange(app);
    update_controls(app);
    SetFocus(app.editor->hwnd());
    status(app, L"源码编辑 · Ctrl+S 保存 · F6 返回阅读 · Ctrl+H 替换");
}
bool save_editor(App &app, bool saveAs) {
    if (!app.editor)
        return true;
    auto target = app.path;
    if (saveAs || target.empty()) {
        wchar_t filename[32768]{};
        if (!target.empty())
            wcsncpy_s(filename, target.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = app.hwnd;
        dialog.lpstrFilter = L"Markdown 文件\0*.md\0所有文件\0*.*\0";
        dialog.lpstrDefExt = L"md";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = 32768;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog))
            return false;
        target = filename;
    }
    if (!saveAs && !app.path.empty()) {
        std::error_code ec;
        auto stamp = std::filesystem::last_write_time(app.path, ec);
        if (!ec && stamp != app.file.stamp) {
            if (MessageBoxW(app.hwnd, L"磁盘文件已被其他程序修改。是否用当前编辑内容覆盖？", L"文件冲突",
                            MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES)
                return false;
        }
    }
    if (!app.editor->modified() && target == app.path) {
        status(app, L"没有未保存更改");
        return true;
    }
    auto text = app.editor->text_for_save(app.file.crlf);
    std::wstring error;
    if (!save_file(target, text, app.file.encoding, error)) {
        MessageBoxW(app.hwnd, error.c_str(), L"保存失败", MB_OK | MB_ICONERROR);
        return false;
    }
    if (target != app.path) {
        if (app.historyIndex >= 0 && !app.path.empty() && app.history[app.historyIndex].first == app.path)
            app.history[app.historyIndex].second = app.view->anchor_block();
        app.history.resize(app.historyIndex + 1);
        app.history.emplace_back(target, app.view->anchor_block());
        if (app.history.size() > 50)
            app.history.erase(app.history.begin());
        app.historyIndex = (int)app.history.size() - 1;
    }
    app.path = target;
    std::error_code ec;
    app.file.stamp = std::filesystem::last_write_time(target, ec);
    if (is_mmd(target))
        app.file.utf8 = utf8(text);
    app.editor->saved(text);
    app.preferences.remember(target, app.view->anchor_block());
    update_recent(app);
    SetWindowTextW(app.hwnd, app.title().c_str());
    queue_preview(app);
    status(app, L"已保存 · " + target.filename().wstring());
    return true;
}
void update_toc(App &app) {
    if (!app.toc)
        return;
    auto doc = app.view->document();
    ListView_SetItemState(app.toc, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(app.toc, doc ? (int)doc->headings.size() : 0, LVSICF_NOSCROLL);
    InvalidateRect(app.toc, nullptr, FALSE);
}
void ensure_toc(App &app) {
    if (app.toc)
        return;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    app.toc =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                        WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL |
                            LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS,
                        0, 0, 0, 0, app.hwnd, reinterpret_cast<HMENU>((INT_PTR)TocId), nullptr, nullptr);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = 200;
    ListView_InsertColumn(app.toc, 0, &column);
    ListView_SetExtendedListViewStyle(app.toc, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(app.toc, WM_SETFONT, (WPARAM)app.font, TRUE);
    update_toc(app);
}
void ensure_search(App &app) {
    if (app.searchBox)
        return;
    app.searchBox =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
                        app.hwnd, reinterpret_cast<HMENU>((INT_PTR)SearchId), nullptr, nullptr);
    app.searchPrev = CreateWindowExW(0, L"BUTTON", L"上一个", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, app.hwnd,
                                     (HMENU)FindPrevious, nullptr, nullptr);
    app.searchNext = CreateWindowExW(0, L"BUTTON", L"下一个", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, app.hwnd,
                                     (HMENU)FindNext, nullptr, nullptr);
    app.replaceBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                     0, 0, 0, 0, app.hwnd, (HMENU)203, nullptr, nullptr);
    app.replaceOne = CreateWindowExW(0, L"BUTTON", L"替换一次", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, app.hwnd,
                                     (HMENU)ReplaceOne, nullptr, nullptr);
    app.replaceAll = CreateWindowExW(0, L"BUTTON", L"全部替换", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, app.hwnd,
                                     (HMENU)ReplaceAll, nullptr, nullptr);
    for (HWND child :
         {app.searchBox, app.searchPrev, app.searchNext, app.replaceBox, app.replaceOne, app.replaceAll})
        SendMessageW(child, WM_SETFONT, (WPARAM)app.font, TRUE);
}
void begin_load(App &app, const std::filesystem::path &path, bool refresh = false) {
    // Preserve the current editor until the new document has loaded successfully.
    if (app.editor) {
        if (app.editor->modified()) {
            int choice = MessageBoxW(app.hwnd, L"保存当前 Markdown 更改？", L"KeepMD",
                                     MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL || (choice == IDYES && !save_editor(app)))
                return;
        }
        KillTimer(app.hwnd, 3);
        EnableWindow(app.editor->hwnd(), FALSE);
    }
    if (!app.path.empty())
        app.preferences.remember(app.path, app.view->anchor_block());
    if (app.cancel)
        app.cancel->store(true);
    if (!app.loader)
        app.loader = std::make_unique<Worker>();
    app.loader->clear();
    app.cancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = app.cancel;
    auto version = ++app.generation;
    app.loading = true;
    status(app, L"正在读取 · " + path.filename().wstring());
    app.loader->push([&app, path, cancel, version, refresh] {
        LoadResult result;
        result.generation = version;
        result.path = path;
        result.refresh = refresh;
        try {
            if (load_file(path, result.file, result.error) && !cancel->load()) {
                auto source = result.file.utf8;
                if (is_mmd(path))
                    source = "```mermaid\n" + source + "\n```\n";
                result.doc = parse_document(std::move(source), cancel.get());
                if (!result.doc->error.empty())
                    result.error = result.doc->error;
            }
        } catch (const std::exception &) {
            result.error = L"读取或解析失败。";
        }
        if (cancel->load())
            return;
        {
            std::lock_guard lock(app.mutex);
            app.results.push_back(std::move(result));
        }
        PostMessageW(app.hwnd, WM_DOCUMENT_READY, 0, 0);
    });
}
void open_dialog(App &app) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = app.hwnd;
    dialog.lpstrFilter = L"Markdown 文件\0*.md;*.markdown;*.mmd\0所有文件\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
        app.navigating = false;
        begin_load(app, path);
    }
}
void find(App &app, bool next = false, bool previous = false) {
    auto doc = app.view->document();
    if (!doc)
        return;
    auto query = control_text(app.searchBox);
    if (app.editor) {
        bool found = app.editor->find(query, previous, !next && !previous);
        status(app, found ? L"已定位源码中的匹配" : L"源码中未找到匹配");
        return;
    }
    if (!next && !previous) {
        app.matches = search(*doc, query);
        app.match = app.matches.empty() ? -1 : 0;
    } else if (!app.matches.empty())
        app.match = (app.match + (previous ? -1 : 1) + (int)app.matches.size()) % (int)app.matches.size();
    app.view->set_matches(app.matches, app.match);
    status(app, query.empty() ? L"输入要查找的文字"
                              : L"查找：" + std::to_wstring(app.match < 0 ? 0 : app.match + 1) + L" / " +
                                    std::to_wstring(app.matches.size()));
}
void navigate(App &app, int delta) {
    if (app.historyIndex < 0)
        return;
    int target = app.historyIndex + delta;
    if (target < 0 || target >= (int)app.history.size())
        return;
    if (!allow_navigation(app))
        return;
    app.history[app.historyIndex].second = app.view->anchor_block();
    app.pendingHistoryIndex = target;
    app.restoreBlock = app.history[target].second;
    app.navigating = true;
    begin_load(app, app.history[target].first);
}
void link(App &app, const std::wstring &target) {
    if (target.empty())
        return;
    if (target.starts_with(L"http://") || target.starts_with(L"https://") || target.starts_with(L"mailto:")) {
        ShellExecuteW(app.hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    auto hash = target.find(L'#');
    std::wstring pathname = target.substr(0, hash);
    app.pendingAnchor = hash == std::wstring::npos ? L"" : target.substr(hash + 1);
    if (pathname.empty()) {
        if (auto doc = app.view->document())
            for (const auto &h : doc->headings)
                if (h.anchor == app.pendingAnchor) {
                    app.view->goto_block(h.block);
                    break;
                }
        app.pendingAnchor.clear();
        return;
    }
    if (pathname.starts_with(L"file:///"))
        pathname.erase(0, 8);
    bool drivePath = pathname.size() > 2 && iswalpha(pathname[0]) && pathname[1] == L':' &&
                     (pathname[2] == L'/' || pathname[2] == L'\\');
    if (pathname.find(L":") != std::wstring::npos && !drivePath) {
        status(app, L"该链接类型暂不支持");
        return;
    }
    // URI percent decoding while preserving ordinary Unicode paths.
    std::string encoded = utf8(pathname), decoded;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size() && hex(encoded[i + 1]) >= 0 &&
            hex(encoded[i + 2]) >= 0) {
            decoded += (char)(hex(encoded[i + 1]) * 16 + hex(encoded[i + 2]));
            i += 2;
        } else
            decoded += encoded[i];
    }
    auto path = app.path.parent_path() / wide(decoded);
    auto ext = path.extension().wstring();
    for (auto &c : ext)
        c = (wchar_t)towlower(c);
    if (ext == L".md" || ext == L".markdown" || ext == L".mmd") {
        app.navigating = false;
        begin_load(app, path);
    } else
        status(app, L"当前只在阅读器中打开 Markdown 链接");
}
void command(App &app, int id) {
    if (id >= 300 && id < 310) {
        size_t index = id - 300;
        if (index < app.preferences.recent.size()) {
            auto path = app.preferences.recent[index].path;
            app.navigating = false;
            begin_load(app, path);
        }
        return;
    }
    switch (id) {
    case Open:
        open_dialog(app);
        break;
    case Exit:
        app.forceExit = true;
        SendMessageW(app.hwnd, WM_CLOSE, 0, 0);
        break;
    case Prompt:
        app.prompt->show();
        break;
    case Find:
        app.showSearch = !app.showSearch;
        if (app.showSearch)
            ensure_search(app);
        arrange(app);
        SetFocus(app.showSearch ? app.searchBox : app.editor ? app.editor->hwnd() : app.reader);
        break;
    case FindNext:
        find(app, true);
        break;
    case FindPrevious:
        find(app, false, true);
        break;
    case Toc:
        app.showToc = !app.showToc;
        if (app.showToc)
            ensure_toc(app);
        arrange(app);
        if (app.showToc)
            SetFocus(app.toc);
        break;
    case Theme:
        app.view->set_dark(!app.view->dark());
        app.prompt->theme(app.view->dark());
        if (app.editor) {
            app.suppressEdit = true;
            app.editor->theme(app.view->dark());
            app.suppressEdit = false;
        }
        break;
    case ZoomIn:
        app.view->set_zoom(app.view->zoom() * 1.1f);
        break;
    case ZoomOut:
        app.view->set_zoom(app.view->zoom() / 1.1f);
        break;
    case ZoomReset:
        app.view->set_zoom(1);
        break;
    case Copy:
        copy_text(app.hwnd, app.view->selection());
        break;
    case SelectAll:
        app.view->select_all();
        break;
    case Reload:
        if (!app.path.empty())
            begin_load(app, app.path, true);
        break;
    case Back:
        navigate(app, -1);
        break;
    case Forward:
        navigate(app, 1);
        break;
    case Width: {
        app.view->set_reading_width(app.view->reading_width() > 800 ? 700.f : 1100.f);
        break;
    }
    case Edit: {
        bool wasEditing = bool(app.editor);
        toggle_editor(app);
        if (wasEditing && !app.editor && !app.path.empty())
            begin_load(app, app.path, true);
        break;
    }
    case Save:
        save_editor(app);
        break;
    case SaveAs:
        if (!app.editor)
            toggle_editor(app);
        save_editor(app, true);
        break;
    case New:
        if (allow_navigation(app)) {
            app.preferences.remember(app.path, app.view->anchor_block());
            if (app.cancel)
                app.cancel->store(true);
            ++app.generation;
            app.loading = false;
            app.path.clear();
            app.file = {};
            app.view->set_document(parse_document(""), {});
            update_toc(app);
            toggle_editor(app);
            SetWindowTextW(app.hwnd, app.title().c_str());
        }
        break;
    case Replace:
        if (!app.editor)
            toggle_editor(app);
        if (app.editor) {
            ensure_search(app);
            app.showSearch = true;
            app.showReplace = !app.showReplace;
            arrange(app);
            SetFocus(app.searchBox);
        }
        break;
    case ReplaceOne:
    case ReplaceAll:
        if (app.editor) {
            auto count = app.editor->replace(control_text(app.searchBox), control_text(app.replaceBox),
                                             id == ReplaceAll);
            status(app, L"已替换 " + std::to_wstring(count) + L" 处");
        }
        break;
    case Split:
        if (app.editor) {
            app.split = !app.split;
            arrange(app);
        }
        break;
    case RegisterOpenWith: {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        std::wstring error;
        if (apply_registration(registration_plan(executable), error))
            MessageBoxW(app.hwnd, L"已添加。可在文件右键的“打开方式”或 Windows 设置中选择 KeepMD。",
                        L"KeepMD", MB_OK);
        else
            MessageBoxW(app.hwnd, error.c_str(), L"KeepMD", MB_OK | MB_ICONERROR);
        break;
    }
    case About:
        MessageBoxW(app.hwnd,
                    L"KeepMD\nWindows 原生 Markdown 阅读器\n\nDirectWrite · Direct2D · MD4C\nCtrl+O 打开 · "
                    L"Ctrl+F 查找 · F9 目录\nCtrl+D 深浅主题 · Ctrl+滚轮缩放",
                    L"关于 KeepMD", MB_OK);
        break;
    }
}
void write_report(App &app) {
    if (app.report.empty())
        return;
    PROCESS_MEMORY_COUNTERS_EX memory{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                         sizeof(memory));
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    std::ofstream out(app.report);
    auto doc = app.view->document();
    out << "{\n  \"first_paint_ms\": " << app.firstPaintMs
        << ",\n  \"first_paint_qpc\": " << app.firstPaintQpc
        << ",\n  \"qpc_frequency\": " << frequency.QuadPart
        << ",\n  \"parse_ms\": " << (doc ? doc->parse_ms : 0)
        << ",\n  \"blocks\": " << (doc ? doc->blocks.size() : 0)
        << ",\n  \"private_bytes\": " << memory.PrivateUsage
        << ",\n  \"working_set\": " << memory.WorkingSetSize
        << ",\n  \"layouts\": " << app.view->layout_count()
        << ",\n  \"reused_layouts\": " << app.view->reused_layouts()
        << ",\n  \"reused_diagrams\": " << app.view->reused_diagrams()
        << ",\n  \"anchor_block\": " << app.view->anchor_block() << ",\n  \"zoom\": " << app.view->zoom()
        << ",\n  \"render_dpi\": " << (unsigned)(app.view->dpi_scale() * 96)
        << ",\n  \"editing\": " << (app.editor ? "true" : "false")
        << ",\n  \"asset_bytes\": " << app.view->asset_bytes()
        << ",\n  \"paint_count\": " << app.view->paint_count()
        << ",\n  \"last_paint_ms\": " << app.view->last_paint_ms() << ",\n  \"scroll_samples\": [";
    bool first = true;
    for (const auto &sample : app.view->scroll_samples()) {
        if (!first)
            out << ",";
        first = false;
        out << "{\"draw_ms\":" << sample.draw_ms << ",\"dispatch_ms\":" << sample.dispatch_ms << "}";
    }
    out << "]\n}\n";
}
LRESULT CALLBACK reader_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto *app = app_of(hwnd);
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app || !app->view)
        return DefWindowProcW(hwnd, message, w, l);
    auto &view = *app->view;
    float dpi = view.dpi_scale();
    switch (message) {
    case WM_PAINT:
        view.paint();
        if (!app->firstPaint && !app->loading && view.document()) {
            app->firstPaint = true;
            app->firstPaintMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - processStart)
                    .count();
            LARGE_INTEGER stamp{};
            QueryPerformanceCounter(&stamp);
            app->firstPaintQpc = stamp.QuadPart;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        if (!view.link_at(point.x / dpi, point.y / dpi).empty()) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_SIZE:
        view.resize();
        return 0;
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL)
            view.set_zoom(view.zoom() * (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1.1f : 1 / 1.1f));
        else
            view.scroll(-GET_WHEEL_DELTA_WPARAM(w) / 120.f * 80.f);
        return 0;
    case WM_MOUSEHWHEEL:
        view.hscroll(GET_WHEEL_DELTA_WPARAM(w) / 120.f * 60);
        return 0;
    case WM_VSCROLL:
    case WM_HSCROLL: {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd, message == WM_VSCROLL ? SB_VERT : SB_HORZ, &si);
        float pos = message == WM_VSCROLL ? view.scroll_y() : view.scroll_x();
        switch (LOWORD(w)) {
        case SB_LINEUP:
            pos -= 36;
            break;
        case SB_LINEDOWN:
            pos += 36;
            break;
        case SB_PAGEUP:
            pos -= si.nPage * .9f;
            break;
        case SB_PAGEDOWN:
            pos += si.nPage * .9f;
            break;
        case SB_THUMBTRACK:
            pos = (float)si.nTrackPos;
            break;
        case SB_TOP:
            pos = 0;
            break;
        case SB_BOTTOM:
            pos = (float)si.nMax;
            break;
        }
        if (message == WM_VSCROLL)
            view.scroll_to(pos);
        else
            view.hscroll(pos - view.scroll_x());
        return 0;
    }
    case WM_LBUTTONDOWN:
        view.mouse_down(GET_X_LPARAM(l) / dpi, GET_Y_LPARAM(l) / dpi, (w & MK_SHIFT) != 0);
        return 0;
    case WM_MOUSEMOVE:
        view.mouse_move(GET_X_LPARAM(l) / dpi, GET_Y_LPARAM(l) / dpi);
        return 0;
    case WM_LBUTTONUP:
        view.mouse_up(GET_X_LPARAM(l) / dpi, GET_Y_LPARAM(l) / dpi);
        return 0;
    case WM_CAPTURECHANGED:
        view.cancel_drag();
        return 0;
    case WM_LBUTTONDBLCLK:
        view.toggle_diagram(GET_X_LPARAM(l) / dpi, GET_Y_LPARAM(l) / dpi);
        return 0;
    case WM_KEYDOWN:
        switch (w) {
        case VK_DOWN:
            view.scroll(40);
            break;
        case VK_UP:
            view.scroll(-40);
            break;
        case VK_NEXT:
            view.scroll(view.page_height() * .9f);
            break;
        case VK_PRIOR:
            view.scroll(-view.page_height() * .9f);
            break;
        case VK_HOME:
            view.scroll_to(0);
            break;
        case VK_END:
            view.scroll_to(view.total_height());
            break;
        case VK_LEFT:
            view.hscroll(-40);
            break;
        case VK_RIGHT:
            view.hscroll(40);
            break;
        default:
            return DefWindowProcW(hwnd, message, w, l);
        }
        return 0;
    case WM_ASSET_READY:
        view.collect_assets();
        return 0;
    case WM_CONTEXTMENU: {
        HMENU menu = CreatePopupMenu();
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (p.x == -1)
            GetCursorPos(&p);
        POINT client = p;
        ScreenToClient(hwnd, &client);
        auto source = view.source_at(client.x / dpi, client.y / dpi);
        AppendMenuW(menu, MF_STRING, Copy, L"复制\tCtrl+C");
        AppendMenuW(menu, MF_STRING, SelectAll, L"全选\tCtrl+A");
        if (!source.empty())
            AppendMenuW(menu, MF_STRING, 900, L"复制代码 / 图表源码");
        AppendMenuW(menu, MF_STRING, 901, L"切换图表适应宽度 / 原始大小");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Theme, L"切换深浅主题");
        int id = TrackPopupMenu(menu, TPM_RETURNCMD, p.x, p.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (id == 900)
            copy_text(hwnd, source);
        else if (id == 901)
            view.toggle_diagram(client.x / dpi, client.y / dpi);
        else if (id)
            command(*app, id);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, w, l);
}
LRESULT CALLBACK main_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto *app = app_of(hwnd);
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        app->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app)
        return DefWindowProcW(hwnd, message, w, l);
    switch (message) {
    case WM_CREATE: {
        app->font =
            CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        app->toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                                       WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | CCS_NORESIZE |
                                           CCS_NOPARENTALIGN,
                                       0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(app->toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(app->toolbar, WM_SETFONT, (WPARAM)app->font, TRUE);
        const struct {
            int id;
            const wchar_t *label;
        } buttons[] = {{Open, L"打开"},  {Back, L"←"},         {Forward, L"→"},      {Toc, L"目录"},
                       {Find, L"查找"},  {ZoomOut, L"−"},      {ZoomReset, L"100%"}, {ZoomIn, L"＋"},
                       {Theme, L"主题"}, {Width, L"阅读宽度"}, {Edit, L"编辑"},      {Save, L"保存"},
                       {Split, L"双栏"}, {Prompt, L"提示词"}};
        for (const auto &button : buttons) {
            TBBUTTON b{};
            b.iBitmap = I_IMAGENONE;
            b.idCommand = button.id;
            b.fsState = TBSTATE_ENABLED;
            b.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
            b.iString = reinterpret_cast<INT_PTR>(button.label);
            SendMessageW(app->toolbar, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&b));
        }
        app->status = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0,
                                      0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(app->status, WM_SETFONT, (WPARAM)app->font, TRUE);
        app->reader = CreateWindowExW(0, L"KeepMD.Reader", nullptr,
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0, 0,
                                      0, hwnd, nullptr, nullptr, app);
        app->view = new View(app->reader);
        // The reader is not a text input field; editing/search controls keep
        // their own native IME contexts when they are created on demand.
        ImmAssociateContext(app->reader, nullptr);
        if (app->renderDpi)
            app->view->set_render_dpi(app->renderDpi);
        app->view->on_link = [app](const auto &target) { link(*app, target); };
        if (app->settingsPath.empty()) {
            wchar_t executablePath[32768]{};
            GetModuleFileNameW(nullptr, executablePath, 32768);
            app->settingsPath = std::filesystem::path(executablePath).parent_path() / L"keepmd.ini";
        }
        app->preferences = load_settings(app->settingsPath);
        app->prompt = std::make_unique<PromptWindow>(hwnd, app->settingsPath, app->preferences.dark);
        app->view->set_dark(app->preferences.dark);
        app->view->set_zoom(app->preferences.zoom);
        app->view->set_reading_width(app->preferences.reading_width);
        update_recent(*app);
        DragAcceptFiles(hwnd, TRUE);
        update_ui_font(*app, GetDpiForWindow(hwnd));
        arrange(*app);
        update_controls(*app);
        SetFocus(app->reader);
        SetTimer(hwnd, 1, 1000, nullptr);
        return 0;
    }
    case WM_SIZE:
        arrange(*app);
        return 0;
    case WM_DPICHANGED: {
        update_ui_font(*app, LOWORD(w));
        const auto *r = reinterpret_cast<RECT *>(l);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        arrange(*app);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(l);
        info->ptMinTrackSize = {640, 420};
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == EditorId && HIWORD(w) == EN_MAXTEXT) {
            status(*app, L"编辑内容已达到缓冲上限，后续输入未加入。请先保存当前内容。");
            return 0;
        }
        if (LOWORD(w) == EditorId && HIWORD(w) == EN_CHANGE) {
            if (!app->suppressEdit && app->editor) {
                SetTimer(hwnd, 3, 350, nullptr);
                SetWindowTextW(hwnd, app->title().c_str());
            }
            return 0;
        }
        if (LOWORD(w) == SearchId && HIWORD(w) == EN_CHANGE) {
            find(*app);
            return 0;
        }
        command(*app, LOWORD(w));
        return 0;
    case WM_NOTIFY: {
        auto *notice = reinterpret_cast<NMHDR *>(l);
        if (notice->idFrom != TocId || !app->view)
            break;
        auto doc = app->view->document();
        if (!doc)
            break;
        if (notice->code == LVN_GETDISPINFOW) {
            auto *data = reinterpret_cast<NMLVDISPINFOW *>(l);
            if ((data->item.mask & LVIF_TEXT) && data->item.pszText && data->item.cchTextMax > 0 &&
                data->item.iItem >= 0 && (size_t)data->item.iItem < doc->headings.size()) {
                const auto &heading = doc->headings[data->item.iItem];
                auto label = std::wstring((heading.level - 1) * 2, L' ') + heading.text.substr(0, 512);
                wcsncpy_s(data->item.pszText, data->item.cchTextMax, label.c_str(), _TRUNCATE);
            }
            return 0;
        }
        if (notice->code == LVN_ITEMCHANGED) {
            auto *data = reinterpret_cast<NMLISTVIEW *>(l);
            if ((data->uChanged & LVIF_STATE) && ((data->uNewState ^ data->uOldState) & LVIS_SELECTED) &&
                (data->uNewState & LVIS_SELECTED) && data->iItem >= 0 &&
                (size_t)data->iItem < doc->headings.size())
                app->view->goto_block(doc->headings[data->iItem].block);
            return 0;
        }
        if (notice->code == LVN_ODFINDITEMW) {
            auto *request = reinterpret_cast<NMLVFINDITEMW *>(l);
            if (!(request->lvfi.flags & LVFI_STRING) || !request->lvfi.psz || doc->headings.empty())
                return -1;
            size_t length = wcslen(request->lvfi.psz), start = std::max(0, request->iStart);
            for (size_t n = 0; n < doc->headings.size(); ++n) {
                size_t index = (start + n) % doc->headings.size();
                if (_wcsnicmp(doc->headings[index].text.c_str(), request->lvfi.psz, length) == 0)
                    return (LRESULT)index;
            }
            return -1;
        }
        break;
    }
    case WM_DOCUMENT_READY: {
        std::vector<LoadResult> results;
        {
            std::lock_guard lock(app->mutex);
            results.swap(app->results);
        }
        for (auto &result : results) {
            if (result.generation != app->generation)
                continue;
            app->loading = false;
            if (!result.error.empty()) {
                status(*app, result.error);
                if (!result.preview) {
                    app->navigating = false;
                    app->pendingHistoryIndex = -1;
                    if (app->editor)
                        EnableWindow(app->editor->hwnd(), TRUE);
                }
                continue;
            }
            if (result.preview) {
                float scroll = app->view->scroll_y();
                size_t cursorBlock = 0, bestSource = 0;
                for (size_t i = 0; i < result.doc->blocks.size(); ++i) {
                    size_t source = result.doc->blocks[i].source;
                    if (source <= result.cursorSource && source > bestSource) {
                        bestSource = source;
                        cursorBlock = i;
                    }
                }
                app->view->set_document(std::move(result.doc), app->path);
                if (app->editor && GetFocus() == app->editor->hwnd())
                    app->view->goto_block(cursorBlock);
                else
                    app->view->scroll_to(scroll);
                update_toc(*app);
                continue;
            }
            if (app->editor) {
                app->editor.reset();
                app->showReplace = false;
                arrange(*app);
                update_controls(*app);
                SetFocus(app->reader);
            }
            float scroll = result.refresh ? app->view->scroll_y() : app->navigating ? app->restoreScroll : 0;
            if (app->navigating && app->pendingHistoryIndex >= 0) {
                app->historyIndex = app->pendingHistoryIndex;
                app->pendingHistoryIndex = -1;
            }
            if (!result.refresh && !app->navigating) {
                if (app->historyIndex >= 0 && app->historyIndex < (int)app->history.size())
                    app->history[app->historyIndex].second = app->view->anchor_block();
                app->history.resize(app->historyIndex + 1);
                app->history.emplace_back(result.path, 0);
                if (app->history.size() > 50)
                    app->history.erase(app->history.begin());
                app->historyIndex = (int)app->history.size() - 1;
            }
            app->path = result.path;
            app->file = std::move(result.file);
            if (!is_mmd(app->path))
                std::string{}.swap(app->file.utf8);
            app->view->set_document(std::move(result.doc), app->path);
            if (result.refresh)
                app->view->scroll_to(scroll);
            else {
                auto index = app->navigating ? app->restoreBlock : app->preferences.position(app->path);
                if (auto loaded = app->view->document(); loaded && !loaded->blocks.empty())
                    app->view->goto_block(std::min(index, loaded->blocks.size() - 1));
            }
            app->preferences.remember(app->path, app->view->anchor_block());
            update_recent(*app);
            app->navigating = false;
            SetWindowTextW(hwnd, app->title().c_str());
            update_toc(*app);
            if (!app->pendingAnchor.empty()) {
                auto doc = app->view->document();
                for (const auto &h : doc->headings)
                    if (h.anchor == app->pendingAnchor) {
                        app->view->goto_block(h.block);
                        break;
                    }
                app->pendingAnchor.clear();
            }
            auto doc = app->view->document();
            const wchar_t *encoding = app->file.encoding == Encoding::Utf8      ? L"UTF-8"
                                      : app->file.encoding == Encoding::Utf8Bom ? L"UTF-8 BOM"
                                      : app->file.encoding == Encoding::Utf16LE ? L"UTF-16 LE"
                                                                                : L"UTF-16 BE";
            status(*app, L"阅读 · " + std::wstring(encoding) + L" · " + app->path.filename().wstring());
            if (app->showSearch)
                find(*app);
            app->openedAt = GetTickCount64();
            if (!app->snapshot.empty() || app->autoExitMs)
                SetTimer(hwnd, 2, 100, nullptr);
        }
        return 0;
    }
    case WM_TIMER:
        if (w == 3) {
            KillTimer(hwnd, 3);
            queue_preview(*app);
            return 0;
        }
        if (w == 1 && !app->loading && !app->path.empty()) {
            std::error_code ec;
            auto stamp = std::filesystem::last_write_time(app->path, ec);
            if (!ec && stamp != app->file.stamp) {
                if (app->editor)
                    status(*app, L"磁盘文件已变化 · F5 可重新加载；保存时将检查冲突");
                else
                    begin_load(*app, app->path, true);
            }
        }
        if (w == 2 && app->firstPaint && !app->view->assets_pending() &&
            GetTickCount64() - app->openedAt > (uint64_t)std::max(app->autoExitMs, 400)) {
            if (!app->snapshot.empty())
                app->view->snapshot(app->snapshot);
            write_report(*app);
            KillTimer(hwnd, 2);
            if (app->autoExitMs)
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_DROPFILES: {
        auto drop = (HDROP)w;
        UINT size = DragQueryFileW(drop, 0, nullptr, 0);
        std::wstring path(size + 1, 0);
        DragQueryFileW(drop, 0, path.data(), size + 1);
        path.resize(size);
        DragFinish(drop);
        app->navigating = false;
        begin_load(*app, path);
        return 0;
    }
    case WM_APP + 100:
        write_report(*app);
        return 0; // Opt-in diagnostic report path only.
    case WM_APP + 101:
        if (!app->report.empty())
            app->view->reset_device();
        return 0;
    case WM_PROMPT_OWNER:
        if (w == 1) {
            ShowWindow(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
            SetFocus(app->reader);
            SetTimer(hwnd, 1, 1000, nullptr);
        } else if (w == 2) {
            app->forceExit = true;
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_QUERYENDSESSION:
        return app->prompt->flush() && allow_navigation(*app);
    case WM_CLOSE:
        if (app->prompt->flush() && allow_navigation(*app)) {
            if (!app->forceExit && app->prompt->resident()) {
                ShowWindow(hwnd, SW_HIDE);
                KillTimer(hwnd, 1);
            } else
                DestroyWindow(hwnd);
        }
        app->forceExit = false;
        return 0;
    case WM_DESTROY:
        app->prompt.reset();
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        if (app->cancel)
            app->cancel->store(true);
        app->loader.reset();
        write_report(*app);
        app->preferences.dark = app->view->dark();
        app->preferences.zoom = app->view->zoom();
        app->preferences.reading_width = app->view->reading_width();
        app->preferences.remember(app->path, app->view->anchor_block());
        if (!app->skipSettingsSave)
            store_settings(app->settingsPath, app->preferences);
        delete app->view;
        app->view = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    cls.hIconSm =
        static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    cls.lpszClassName = L"KeepMD.Window";
    cls.lpfnWndProc = main_proc;
    RegisterClassExW(&cls);
    cls.lpszClassName = L"KeepMD.Reader";
    cls.lpfnWndProc = reader_proc;
    cls.hbrBackground = nullptr;
    cls.style = CS_DBLCLKS;
    RegisterClassExW(&cls);
    App app;
    bool residentStart = false, promptStart = false;
    std::filesystem::path initial;
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--resident")
            residentStart = true;
        else if (arg == L"--prompt")
            promptStart = true;
        else if (arg == L"--snapshot" && i + 1 < argc)
            app.snapshot = argv[++i];
        else if (arg == L"--report" && i + 1 < argc)
            app.report = argv[++i];
        else if (arg == L"--config" && i + 1 < argc)
            app.settingsPath = std::filesystem::absolute(argv[++i]);
        else if (arg == L"--render-dpi" && i + 1 < argc)
            app.renderDpi = std::clamp(_wtoi(argv[++i]), 96, 288);
        else if (arg == L"--exit-after" && i + 1 < argc)
            app.autoExitMs = _wtoi(argv[++i]);
        else if (!arg.starts_with(L"--"))
            initial = arg;
    }
    LocalFree(argv);
    HMENU menu = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu();
    app.recentMenu = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, New, L"新建\tCtrl+N");
    AppendMenuW(file, MF_STRING, Open, L"打开…\tCtrl+O");
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(app.recentMenu), L"最近文件");
    AppendMenuW(file, MF_STRING, Save, L"保存\tCtrl+S");
    AppendMenuW(file, MF_STRING, SaveAs, L"另存为…\tCtrl+Shift+S");
    AppendMenuW(file, MF_STRING, Reload, L"重新加载\tF5");
    AppendMenuW(file, MF_STRING, RegisterOpenWith, L"添加到打开方式");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, Exit, L"退出");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"文件");
    AppendMenuW(view, MF_STRING, Edit, L"编辑 / 阅读\tF6");
    AppendMenuW(view, MF_STRING, Split, L"切换双栏预览");
    AppendMenuW(view, MF_STRING, Find, L"查找\tCtrl+F");
    AppendMenuW(view, MF_STRING, Replace, L"替换\tCtrl+H");
    AppendMenuW(view, MF_STRING, Toc, L"目录\tF9");
    AppendMenuW(view, MF_STRING, Theme, L"深浅主题\tCtrl+D");
    AppendMenuW(view, MF_STRING, ZoomReset, L"重置缩放\tCtrl+0");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)view, L"视图");
    AppendMenuW(menu, MF_STRING, Prompt, L"提示词");
    AppendMenuW(menu, MF_STRING, About, L"关于");
    auto hwnd = CreateWindowExW(0, L"KeepMD.Window", L"KeepMD", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1060, 820, nullptr, menu, instance, &app);
    if (!hwnd)
        return 1;
    bool promptOwner = app.prompt->start(residentStart || promptStart, promptStart);
    if ((residentStart || promptStart) && !promptOwner && initial.empty()) {
        app.skipSettingsSave = true;
        app.forceExit = true;
        DestroyWindow(hwnd);
        CoUninitialize();
        return 0;
    }
    ShowWindow(hwnd, (residentStart || promptStart) && initial.empty() ? SW_HIDE : show);
    if ((residentStart || promptStart) && initial.empty())
        KillTimer(hwnd, 1);
    UpdateWindow(hwnd);
    if (!initial.empty()) {
        std::error_code ec;
        initial = std::filesystem::absolute(initial, ec);
        begin_load(app, initial);
    } else
        app.view->set_document(nullptr, {});
    ACCEL keys[] = {{FVIRTKEY | FCONTROL, 'O', Open},
                    {FVIRTKEY | FCONTROL, 'N', New},
                    {FVIRTKEY | FCONTROL, 'S', Save},
                    {FVIRTKEY | FCONTROL | FSHIFT, 'S', SaveAs},
                    {FVIRTKEY, VK_F6, Edit},
                    {FVIRTKEY | FCONTROL, 'H', Replace},
                    {FVIRTKEY | FCONTROL, 'F', Find},
                    {FVIRTKEY, VK_F3, FindNext},
                    {FVIRTKEY | FSHIFT, VK_F3, FindPrevious},
                    {FVIRTKEY, VK_F9, Toc},
                    {FVIRTKEY | FCONTROL, 'D', Theme},
                    {FVIRTKEY | FCONTROL, '0', ZoomReset},
                    {FVIRTKEY | FCONTROL, VK_OEM_PLUS, ZoomIn},
                    {FVIRTKEY | FCONTROL, VK_OEM_MINUS, ZoomOut},
                    {FVIRTKEY | FCONTROL, 'C', Copy},
                    {FVIRTKEY | FCONTROL, 'A', SelectAll},
                    {FVIRTKEY, VK_F5, Reload},
                    {FVIRTKEY | FALT, VK_LEFT, Back},
                    {FVIRTKEY | FALT, VK_RIGHT, Forward}};
    auto accel = CreateAcceleratorTableW(keys, (int)std::size(keys));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (app.prompt && app.prompt->translate(message))
            continue;
        bool inSearch = (app.searchBox && GetFocus() == app.searchBox) ||
                        (app.replaceBox && GetFocus() == app.replaceBox);
        bool inEditor = app.editor && GetFocus() == app.editor->hwnd();
        if (inSearch && message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            find(app, true);
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && app.showSearch) {
            command(app, Find);
            continue;
        }
        if (!inEditor && message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
            auto next = GetNextDlgTabItem(hwnd, GetFocus(), GetKeyState(VK_SHIFT) < 0);
            if (next) {
                SetFocus(next);
                continue;
            }
        }
        if ((inSearch || inEditor) && message.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) &&
            (message.wParam == 'A' || message.wParam == 'C')) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        if (!TranslateAcceleratorW(hwnd, accel, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DestroyAcceleratorTable(accel);
    CoUninitialize();
    return (int)message.wParam;
}
