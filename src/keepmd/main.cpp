#include "editor.h"
#include "file_io.h"
#include "paths.h"
#include "settings.h"
#include "shell_integration.h"
#include "ui.h"
#include "view.h"
#include <algorithm>
#include <chrono>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
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
    SetDefault,
    ToggleToolbar = 128, // Command IDs stay stable for automation; 127 is retired.
    EditMode,            // Switch the open editor between visual and Markdown source.
    ModeRead,
    ModeVisual,
    ModeSource,
    RemoteImages,
    Fullscreen,
    FormatH1 = 140,
    FormatH2,
    FormatBold,
    FormatItalic,
    FormatBullet,
    FormatNumbered,
    FormatQuote,
    FormatCode,
    FormatDiagram,
    EditSeparator = 160 // Toolbar separator shown only with the visual formatting buttons.
};
constexpr int FormatFirst = FormatH1, FormatLast = FormatDiagram;
keep::VisualCommand format_command(int id) {
    using keep::VisualCommand;
    static const VisualCommand commands[] = {VisualCommand::H1,     VisualCommand::H2,       VisualCommand::Bold,
                                             VisualCommand::Italic, VisualCommand::Bullet,   VisualCommand::Numbered,
                                             VisualCommand::Quote,  VisualCommand::Code,     VisualCommand::Diagram};
    return commands[id - FormatFirst];
}
ui::Icon toolbar_icon(int id, bool editing, bool visual, bool dark) {
    using ui::Icon;
    switch (id) {
    case EditMode:
        return visual ? Icon::Markdown : Icon::Text;
    case FormatH1:
        return Icon::H1;
    case FormatH2:
        return Icon::H2;
    case FormatBold:
        return Icon::Bold;
    case FormatItalic:
        return Icon::Italic;
    case FormatBullet:
        return Icon::Bullet;
    case FormatNumbered:
        return Icon::Numbered;
    case FormatQuote:
        return Icon::Quote;
    case FormatCode:
        return Icon::Code;
    case FormatDiagram:
        return Icon::Diagram;
    case Open:
        return Icon::Folder;
    case Back:
        return Icon::Back;
    case Forward:
        return Icon::Forward;
    case Toc:
        return Icon::Outline;
    case Find:
        return Icon::Search;
    case ZoomOut:
        return Icon::Minus;
    case ZoomIn:
        return Icon::Plus;
    case Theme:
        return dark ? Icon::Sun : Icon::Moon;
    case Width:
        return Icon::Width;
    case Edit:
        return editing ? Icon::Book : Icon::Edit;
    case Save:
        return Icon::Save;
    case Split:
        return Icon::Split;
    default:
        return Icon::None;
    }
}
const wchar_t *toolbar_tip(int id, bool editing, bool visual) {
    switch (id) {
    case EditMode:
        return visual ? L"切换为 Markdown 源码编辑 · Ctrl+E" : L"切换为可视化编辑 · Ctrl+E";
    case FormatH1:
        return L"一级标题";
    case FormatH2:
        return L"二级标题";
    case FormatBold:
        return L"加粗 · Ctrl+B";
    case FormatItalic:
        return L"斜体 · Ctrl+I";
    case FormatBullet:
        return L"无序列表";
    case FormatNumbered:
        return L"有序列表";
    case FormatQuote:
        return L"引用";
    case FormatCode:
        return L"行内代码";
    case FormatDiagram:
        return L"插入或修改 Mermaid 流程图";
    case Open:
        return L"打开 Markdown · Ctrl+O";
    case Back:
        return L"返回 · Alt+←";
    case Forward:
        return L"前进 · Alt+→";
    case Toc:
        return L"目录 · F9";
    case Find:
        return L"查找 · Ctrl+F";
    case ZoomOut:
        return L"缩小 · Ctrl+−";
    case ZoomIn:
        return L"放大 · Ctrl++";
    case ZoomReset:
        return L"重置缩放 · Ctrl+0";
    case Theme:
        return L"切换深浅主题 · Ctrl+D";
    case Width:
        return L"切换阅读宽度";
    case Edit:
        return editing ? L"返回阅读 · F6" : L"编辑 · F6";
    case Save:
        return L"保存 · Ctrl+S";
    case Split:
        return L"切换源码 / 双栏预览";
    default:
        return L"";
    }
}
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
         replaceOne = nullptr, replaceAll = nullptr, remoteImages = nullptr;
    HFONT font = nullptr;
    HBRUSH surfaceBrush = nullptr;
    RECT searchField{}, replaceField{}, pageCard{};
    View *view = nullptr;
    float dpi = 1;
    bool showToc = false, showSearch = false, loading = false;
    bool fullscreen = false, cursorHidden = false; // The pointer hides after idling in full screen.
    POINT cursorAt{};
    std::filesystem::path path, snapshot, report, imageCache;
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
    bool skipSettingsSave = false;
    unsigned renderDpi = 0;
    HMENU recentMenu = nullptr;
    std::unique_ptr<Editor> editor;
    bool suppressEdit = false, showReplace = false, split = true, readingDraft = false;
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
        if (surfaceBrush)
            DeleteObject(surfaceBrush);
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
    app.font = ui::font(dpi);
    for (HWND child : {app.toolbar, app.status, app.searchBox, app.searchNext, app.searchPrev, app.replaceBox,
                       app.replaceOne, app.replaceAll, app.toc, app.remoteImages})
        if (child)
            SendMessageW(child, WM_SETFONT, (WPARAM)app.font, TRUE);
    if (app.status)
        SendMessageW(app.status, SB_SETMINHEIGHT, MulDiv(26, dpi, 96), 0);
    SendMessageW(app.toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(MulDiv(34, dpi, 96), MulDiv(32, dpi, 96)));
    for (int i = 0; i < SendMessageW(app.toolbar, TB_BUTTONCOUNT, 0, 0); ++i) {
        TBBUTTON b{};
        SendMessageW(app.toolbar, TB_GETBUTTON, i, (LPARAM)&b);
        if (b.fsStyle & BTNS_SEP)
            continue;
        TBBUTTONINFOW info{sizeof(info), TBIF_SIZE};
        int width = b.idCommand == ZoomReset ? 58 : b.idCommand == Toc || b.idCommand == Find ||
                    b.idCommand == Split ? 76 : 36;
        info.cx = (WORD)MulDiv(width, dpi, 96);
        SendMessageW(app.toolbar, TB_SETBUTTONINFOW, b.idCommand, (LPARAM)&info);
    }
    if (old)
        DeleteObject(old);
}
void update_remote_control(App &app) {
    bool enabled = app.view && app.view->remote_images_enabled();
    if (app.remoteImages) {
        ui::style_button(app.remoteImages, false, enabled);
        ui::icon_button(app.remoteImages, ui::Icon::Image,
                        enabled ? L"网络图片已开启 · 点击停止下载；缓存图片仍可显示"
                                : L"网络图片 · 开启本文全部网络图片加载；优先使用本地缓存");
        SetWindowTextW(app.remoteImages, enabled ? L"网络图片（已开启）" : L"网络图片（已关闭）");
    }
    if (auto menu = ui::window_menu(app.hwnd))
        CheckMenuItem(GetSubMenu(menu, 1), RemoteImages, MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
}
void chrome_theme(App &app) {
    bool dark = app.view && app.view->dark();
    auto p = ui::palette(dark);
    if (app.surfaceBrush)
        DeleteObject(app.surfaceBrush);
    app.surfaceBrush = CreateSolidBrush(p.surface);
    ui::titlebar(app.hwnd, dark);
    update_remote_control(app);
    MENUINFO mi{sizeof(mi)};
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = app.surfaceBrush;
    SetMenuInfo(ui::window_menu(app.hwnd), &mi);
    DrawMenuBar(app.hwnd);
    if (app.toc) {
        ListView_SetBkColor(app.toc, p.background);
        ListView_SetTextBkColor(app.toc, p.background);
        ListView_SetTextColor(app.toc, p.text);
        ui::scroll_theme(app.toc, dark);
    }
    RedrawWindow(app.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
LRESULT CALLBACK status_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto &app = *(App *)data;
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        auto colors = ui::palette(app.view && app.view->dark());
        RECT r{};
        GetClientRect(h, &r);
        ui::fill(dc, r, colors.background);
        ui::line(dc, 0, 0, r.right, 0, colors.border);
        int size = std::min(4096, (int)LOWORD(SendMessageW(h, SB_GETTEXTLENGTHW, 0, 0)));
        // Status text is controlled by this process; allocate the actual reported length.
        std::wstring label((size_t)LOWORD(SendMessageW(h, SB_GETTEXTLENGTHW, 0, 0)) + 1, 0);
        SendMessageW(h, SB_GETTEXTW, 0, (LPARAM)label.data());
        label.resize(size);
        r.left += (int)(16 * app.dpi);
        r.right -= (int)(16 * app.dpi);
        ui::text(dc, app.font, label, r, colors.muted);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, status_proc, id);
    return DefSubclassProc(h, m, w, l);
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
    int caption = ui::caption_height(app.hwnd);
    auto px = [&](int n) { return (int)(n * app.dpi); };
    // Full screen keeps only the document, plus the find bar or outline when the reader opens them.
    bool chrome = !app.fullscreen, toolbar = chrome && app.preferences.toolbar;
    int bottom = chrome ? px(30) : 0, margin = px(18), side = app.showToc ? px(226) : 0;
    int toolsTop = caption + px(6);
    ShowWindow(app.status, chrome ? SW_SHOWNA : SW_HIDE);
    SendMessageW(app.status, WM_SIZE, 0, 0);
    MoveWindow(app.toolbar, (int)(12 * app.dpi), toolsTop,
               std::max(1L, r.right - (int)(68 * app.dpi)), (int)(64 * app.dpi), TRUE);
    SendMessageW(app.toolbar, TB_AUTOSIZE, 0, 0);
    int toolbarHeight = px(32);
    for (int i = 0; i < SendMessageW(app.toolbar, TB_BUTTONCOUNT, 0, 0); ++i) {
        TBBUTTON b{};
        SendMessageW(app.toolbar, TB_GETBUTTON, i, (LPARAM)&b);
        if ((b.fsStyle & BTNS_SEP) || (b.fsState & TBSTATE_HIDDEN))
            continue;
        RECT br{};
        SendMessageW(app.toolbar, TB_GETITEMRECT, i, (LPARAM)&br);
        toolbarHeight = std::max(toolbarHeight, (int)br.bottom);
    }
    int top = chrome ? toolsTop + (toolbar ? toolbarHeight + px(10) : 0) : 0;
    ShowWindow(app.toolbar, toolbar ? SW_SHOWNA : SW_HIDE);
    ShowWindow(app.remoteImages, toolbar ? SW_SHOWNA : SW_HIDE);
    MoveWindow(app.remoteImages, r.right - px(44), toolsTop, px(32), px(32), TRUE);
    ui::caption_toolbar(app.hwnd, app.preferences.toolbar);
    if (auto menu = ui::window_menu(app.hwnd))
        CheckMenuItem(GetSubMenu(menu, 1), ToggleToolbar,
                      MF_BYCOMMAND | (app.preferences.toolbar ? MF_CHECKED : MF_UNCHECKED));
    MoveWindow(app.toolbar, (int)(12 * app.dpi), toolsTop,
               std::max(1L, r.right - (int)(68 * app.dpi)), toolbarHeight, TRUE);
    int searchHeight = app.showSearch ? (int)((app.showReplace ? 88 : 44) * app.dpi) : 0;
    ShowWindow(app.searchBox, app.showSearch ? SW_SHOW : SW_HIDE);
    ShowWindow(app.searchNext, app.showSearch ? SW_SHOW : SW_HIDE);
    ShowWindow(app.searchPrev, app.showSearch ? SW_SHOW : SW_HIDE);
    auto place_field = [&](HWND edit, RECT &field, int y) {
        field = {(int)(12 * app.dpi), y, (int)(12 * app.dpi) + std::max<int>(150, r.right - (int)(240 * app.dpi)),
                 y + (int)(34 * app.dpi)};
        int height = (int)(22 * app.dpi);
        MoveWindow(edit, field.left + (int)(11 * app.dpi), field.top + (field.bottom - field.top - height) / 2,
                   field.right - field.left - (int)(22 * app.dpi), height, TRUE);
    };
    place_field(app.searchBox, app.searchField, top + (int)(4 * app.dpi));
    MoveWindow(app.searchPrev, r.right - (int)(215 * app.dpi), top + (int)(4 * app.dpi), (int)(98 * app.dpi),
               (int)(34 * app.dpi), TRUE);
    MoveWindow(app.searchNext, r.right - (int)(112 * app.dpi), top + (int)(4 * app.dpi), (int)(98 * app.dpi),
               (int)(34 * app.dpi), TRUE);
    for (HWND child : {app.replaceBox, app.replaceOne, app.replaceAll})
        ShowWindow(child, app.showSearch && app.showReplace ? SW_SHOW : SW_HIDE);
    place_field(app.replaceBox, app.replaceField, top + (int)(48 * app.dpi));
    MoveWindow(app.replaceOne, r.right - (int)(215 * app.dpi), top + (int)(48 * app.dpi), (int)(98 * app.dpi),
               (int)(34 * app.dpi), TRUE);
    MoveWindow(app.replaceAll, r.right - (int)(112 * app.dpi), top + (int)(48 * app.dpi), (int)(98 * app.dpi),
               (int)(34 * app.dpi), TRUE);
    if (app.toc) {
        ShowWindow(app.toc, app.showToc ? SW_SHOW : SW_HIDE);
        MoveWindow(app.toc, (int)(12 * app.dpi), top + searchHeight + (int)(38 * app.dpi),
                   std::max(1, side - (int)(24 * app.dpi)),
                   std::max<int>(1, r.bottom - top - searchHeight - bottom - (int)(46 * app.dpi)), TRUE);
        ListView_SetColumnWidth(app.toc, 0, std::max(1, side - (int)(44 * app.dpi)));
    }
    // The visual editor is its own preview; the source editor can share the width with the reader.
    bool preview = app.editor && !app.readingDraft && !app.editor->visual() && app.split;
    int contentLeft = side + margin + px(8), contentRight = r.right - margin - px(8);
    app.pageCard = {side + margin, top + searchHeight, r.right - margin, r.bottom - bottom - px(10)};
    int contentTop = app.pageCard.top + px(38), contentBottom = app.pageCard.bottom - px(8);
    if (!chrome) {
        // No page card: the document fills everything below the find bar and beside the outline.
        app.pageCard = {};
        contentLeft = side;
        contentRight = r.right;
        contentTop = top + searchHeight;
        contentBottom = r.bottom;
    }
    int available = std::max(px(80), contentRight - contentLeft);
    int editorWidth = app.editor && !app.readingDraft ? (preview ? available / 2 : available) : 0;
    if (app.editor)
        ShowWindow(app.editor->hwnd(), app.readingDraft ? SW_HIDE : SW_SHOW);
    if (app.editor && !app.readingDraft)
        MoveWindow(app.editor->hwnd(), contentLeft, contentTop, editorWidth,
                   std::max(1, contentBottom - contentTop), TRUE);
    if (app.editor && !app.readingDraft)
        app.editor->inset(GetDpiForWindow(app.hwnd), (int)app.view->reading_width());
    ShowWindow(app.reader, !app.editor || app.readingDraft || preview ? SW_SHOW : SW_HIDE);
    MoveWindow(app.reader, contentLeft + editorWidth + (preview ? px(8) : 0), contentTop,
               std::max(1, available - editorWidth - (preview ? px(8) : 0)),
               std::max(1, contentBottom - contentTop), TRUE);
    ui::sync_scrollbars(app.reader);
    if (app.editor)
        ui::sync_scrollbars(app.editor->hwnd());
    InvalidateRect(app.hwnd, nullptr, FALSE);
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
    update_remote_control(app);
    int mode = !app.editor || app.readingDraft ? ModeRead : app.editor->visual() ? ModeVisual : ModeSource;
    ui::caption_select(app.hwnd, mode);
    if (auto menu = ui::window_menu(app.hwnd)) {
        CheckMenuRadioItem(GetSubMenu(menu, 1), ModeRead, ModeSource, mode, MF_BYCOMMAND);
        EnableMenuItem(GetSubMenu(menu, 0), Save, MF_BYCOMMAND | (app.editor ? MF_ENABLED : MF_GRAYED));
    }
    SendMessageW(app.toolbar, TB_ENABLEBUTTON, Save, MAKELONG(app.editor != nullptr, 0));
    SendMessageW(app.toolbar, TB_ENABLEBUTTON, Split, MAKELONG(app.editor != nullptr, 0));
    TBBUTTONINFOW info{sizeof(info), TBIF_TEXT};
    info.pszText = const_cast<wchar_t *>(app.editor ? L"阅读" : L"编辑");
    SendMessageW(app.toolbar, TB_SETBUTTONINFOW, Edit, reinterpret_cast<LPARAM>(&info));
    bool visual = app.editor && !app.readingDraft && app.editor->visual();
    SendMessageW(app.toolbar, TB_HIDEBUTTON, Save, MAKELONG(!app.editor, 0));
    SendMessageW(app.toolbar, TB_HIDEBUTTON, EditMode, MAKELONG(!app.editor, 0));
    SendMessageW(app.toolbar, TB_HIDEBUTTON, Split, MAKELONG(!app.editor || app.readingDraft || visual, 0));
    for (int id : {Split, FormatH2, FormatItalic, FormatQuote})
        SendMessageW(app.toolbar, TB_HIDEBUTTON, EditSeparator + id - Split, MAKELONG(!visual, 0));
    for (int id = FormatFirst; id <= FormatLast; ++id)
        SendMessageW(app.toolbar, TB_HIDEBUTTON, id, MAKELONG(!visual, 0));
    for (int id : {Open, Edit, EditMode, Save})
        SendMessageW(app.toolbar, TB_HIDEBUTTON, id, MAKELONG(TRUE, 0));
    for (int id : {Back, Forward, ZoomOut, ZoomReset, ZoomIn, Width})
        SendMessageW(app.toolbar, TB_HIDEBUTTON, id, MAKELONG(visual, 0));
    for (int id : {Forward, ZoomIn, Width})
        SendMessageW(app.toolbar, TB_HIDEBUTTON, 400 + id, MAKELONG(visual, 0));
    InvalidateRect(app.toolbar, nullptr, TRUE);
    arrange(app);
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
        app.readingDraft = false;
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
constexpr wchar_t SourceOnly[] = L"文档较大（超过约 26 万字符），已使用 Markdown 源码编辑以保持输入流畅";
// Creates the preferred editor for `source`, falling back to Markdown source for large or unusual documents.
std::unique_ptr<Editor> make_editor(App &app, std::wstring_view source, bool visual) {
    auto editor = visual && visual_editable(source) ? visual_editor(app.hwnd, EditorId)
                                                    : source_editor(app.hwnd, EditorId);
    if (!editor->valid())
        return nullptr;
    return editor;
}
// Names the active mode, and why a visual request opened the source editor instead.
void editor_status(App &app, bool requestedVisual) {
    status(app, app.editor->visual() ? L"可视化编辑" : requestedVisual ? SourceOnly : L"Markdown 源码编辑");
}
void show_editor(App &app) {
    app.readingDraft = false;
    app.editor->theme(app.view->dark());
    arrange(app);
    update_controls(app);
    SetFocus(app.editor->hwnd());
}
void switch_edit_mode(App &app) {
    if (!app.editor || app.loading)
        return;
    auto text = app.editor->text(false);
    if (!app.editor->visual() && !visual_editable(text)) {
        status(app, SourceOnly);
        return;
    }
    auto baseline = app.editor->baseline();
    app.suppressEdit = true;
    auto next = make_editor(app, text, !app.editor->visual());
    if (!next) {
        app.suppressEdit = false;
        return;
    }
    // The old control goes first: both use the same child-window id.
    app.editor.reset();
    app.editor = std::move(next);
    app.editor->adopt(text, std::move(baseline));
    app.preferences.visual = app.editor->visual();
    show_editor(app);
    app.suppressEdit = false;
    SetWindowTextW(app.hwnd, app.title().c_str());
    editor_status(app, false);
}
void toggle_editor(App &app, bool visual) {
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
    // RichEdit stores U+FFFC as an embedded-object placeholder and turns it into a space, so neither editor
    // could save such a file unchanged.
    if (source.find("\xEF\xBF\xBC") != std::string::npos) {
        status(app, L"文件包含对象占位符 U+FFFC，保留阅读模式以避免保存时被替换。");
        return;
    }
    app.suppressEdit = true;
    auto text = wide(source);
    app.editor = make_editor(app, text, visual);
    if (!app.editor) {
        app.suppressEdit = false;
        status(app, L"无法创建 Windows 原生编辑控件");
        return;
    }
    app.editor->load(text);
    show_editor(app);
    app.suppressEdit = false;
    editor_status(app, visual);
}
void toggle_editor(App &app) {
    toggle_editor(app, app.preferences.visual);
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
        CreateWindowExW(0, WC_LISTVIEWW, nullptr,
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
    ui::attach_scrollbars(app.toc, ui::ScrollKind::List);
    chrome_theme(app);
}
void ensure_search(App &app) {
    if (app.searchBox)
        return;
    app.searchBox =
        CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, app.hwnd,
                        reinterpret_cast<HMENU>((INT_PTR)SearchId), nullptr, nullptr);
    app.searchPrev = CreateWindowExW(0, L"BUTTON", L"上一个", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, app.hwnd,
                                     (HMENU)FindPrevious, nullptr, nullptr);
    app.searchNext = CreateWindowExW(0, L"BUTTON", L"下一个", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, app.hwnd,
                                     (HMENU)FindNext, nullptr, nullptr);
    app.replaceBox = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                     app.hwnd, (HMENU)203, nullptr, nullptr);
    app.replaceOne = CreateWindowExW(0, L"BUTTON", L"替换一次", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, app.hwnd,
                                     (HMENU)ReplaceOne, nullptr, nullptr);
    app.replaceAll = CreateWindowExW(0, L"BUTTON", L"全部替换", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, app.hwnd,
                                     (HMENU)ReplaceAll, nullptr, nullptr);
    for (HWND child :
         {app.searchBox, app.searchPrev, app.searchNext, app.replaceBox, app.replaceOne, app.replaceAll})
        SendMessageW(child, WM_SETFONT, (WPARAM)app.font, TRUE);
    for (HWND h : {app.searchPrev, app.searchNext, app.replaceOne, app.replaceAll})
        ui::style_button(h);
    SendMessageW(app.searchBox, EM_SETCUEBANNER, FALSE, (LPARAM)L"查找文档中的文字…");
    SendMessageW(app.replaceBox, EM_SETCUEBANNER, FALSE, (LPARAM)L"替换为…");
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
    if (app.editor && !app.readingDraft) {
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
    status(app, query.empty() ? L"查找"
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
    auto path = linked_path(app.path.parent_path(), wide(decoded));
    auto ext = path.extension().wstring();
    for (auto &c : ext)
        c = (wchar_t)towlower(c);
    if (ext == L".md" || ext == L".markdown" || ext == L".mmd") {
        app.navigating = false;
        begin_load(app, path);
    } else
        status(app, L"当前只在阅读器中打开 Markdown 链接");
}
void set_fullscreen(App &app, bool on) {
    app.fullscreen = on; // Layout during the window change already follows the new state.
    app.fullscreen = ui::caption_fullscreen(app.hwnd, on);
    app.cursorHidden = false;
    if (app.fullscreen) {
        GetCursorPos(&app.cursorAt);
        SetTimer(app.hwnd, 4, 2500, nullptr);
    } else
        KillTimer(app.hwnd, 4);
    arrange(app);
    if (auto menu = ui::window_menu(app.hwnd))
        CheckMenuItem(GetSubMenu(menu, 1), Fullscreen, MF_BYCOMMAND | (app.fullscreen ? MF_CHECKED : MF_UNCHECKED));
    SetFocus(app.editor && !app.readingDraft ? app.editor->hwnd() : app.reader);
}
// Real pointer movement in full screen shows the pointer again and restarts the idle countdown. Windows also
// sends move messages without movement, which must not keep the pointer visible.
void wake_cursor(App &app, HWND reader) {
    POINT point{};
    GetCursorPos(&point);
    if (point.x == app.cursorAt.x && point.y == app.cursorAt.y)
        return;
    app.cursorAt = point;
    SetTimer(app.hwnd, 4, 2500, nullptr);
    if (app.cursorHidden) {
        app.cursorHidden = false;
        SendMessageW(reader, WM_SETCURSOR, (WPARAM)reader, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
}
void command(App &app, int id) {
    if (id >= FormatFirst && id <= FormatLast) {
        if (app.editor) {
            app.editor->format(format_command(id));
            SetFocus(app.editor->hwnd());
        }
        return;
    }
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
    case Fullscreen:
        set_fullscreen(app, !app.fullscreen);
        break;
    case ToggleToolbar:
        app.preferences.toolbar = !app.preferences.toolbar;
        arrange(app);
        CheckMenuItem(GetSubMenu(ui::window_menu(app.hwnd), 1), ToggleToolbar,
                      MF_BYCOMMAND | (app.preferences.toolbar ? MF_CHECKED : MF_UNCHECKED));
        SetFocus(app.editor && !app.readingDraft ? app.editor->hwnd() : app.reader);
        break;
    case Open:
        open_dialog(app);
        break;
    case Exit:
        SendMessageW(app.hwnd, WM_CLOSE, 0, 0);
        break;
    case Find:
        app.showSearch = !app.showSearch;
        if (app.showSearch)
            ensure_search(app);
        arrange(app);
        SetFocus(app.showSearch ? app.searchBox : app.editor && !app.readingDraft ? app.editor->hwnd() : app.reader);
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
        if (app.editor) {
            app.suppressEdit = true;
            app.editor->theme(app.view->dark());
            app.suppressEdit = false;
        }
        chrome_theme(app);
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
    case RemoteImages:
        app.view->set_remote_images_enabled(!app.view->remote_images_enabled());
        update_remote_control(app);
        status(app, app.view->remote_images_enabled() ? L"网络图片已开启 · 优先使用本地缓存"
                                                     : L"网络图片下载已关闭 · 缓存图片仍可显示");
        break;
    case ModeRead:
        if (app.editor && !app.readingDraft) {
            app.readingDraft = true;
            app.showReplace = false;
            queue_preview(app);
            update_controls(app);
            SetFocus(app.reader);
            status(app, app.editor->modified() ? L"阅读预览 · 更改尚未保存，Ctrl+S 保存" : L"阅读预览");
        }
        break;
    case ModeVisual:
    case ModeSource:
        if (!app.editor)
            toggle_editor(app, id == ModeVisual);
        else {
            app.readingDraft = false;
            if (app.editor->visual() != (id == ModeVisual))
                switch_edit_mode(app);
            else
                editor_status(app, false);
            update_controls(app);
            SetFocus(app.editor->hwnd());
        }
        break;
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
            toggle_editor(app, true);
            SetWindowTextW(app.hwnd, app.title().c_str());
        }
        break;
    case Replace:
        app.readingDraft = false;
        if (!app.editor)
            toggle_editor(app, false);
        else if (app.editor->visual())
            switch_edit_mode(app);
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
        if (app.editor && !app.editor->visual()) {
            app.split = !app.split;
            arrange(app);
        }
        break;
    case EditMode:
        switch_edit_mode(app);
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
    case SetDefault: {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        std::wstring error;
        if (!apply_registration(registration_plan(executable), error)) {
            MessageBoxW(app.hwnd, error.c_str(), L"KeepMD", MB_OK | MB_ICONERROR);
            break;
        }
        if (!open_default_apps_settings(error)) {
            MessageBoxW(app.hwnd, error.c_str(), L"KeepMD", MB_OK | MB_ICONERROR);
            break;
        }
        MessageBoxW(app.hwnd,
                    L"KeepMD 已加入当前用户的 Markdown 应用列表。\n\n请在 Windows 默认应用设置中搜索并选择 "
                    L"KeepMD，然后为 .md、.markdown 和 .mmd 确认关联。",
                    L"设置默认打开程序", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case About:
        MessageBoxW(app.hwnd, L"KeepMD\n原生 Markdown 阅读与编辑", L"关于 KeepMD", MB_OK);
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
        << ",\n  \"formulas_typeset\": " << app.view->formula_counts().first
        << ",\n  \"formulas_source\": " << app.view->formula_counts().second
        << ",\n  \"reused_layouts\": " << app.view->reused_layouts()
        << ",\n  \"reused_diagrams\": " << app.view->reused_diagrams()
        << ",\n  \"anchor_block\": " << app.view->anchor_block() << ",\n  \"zoom\": " << app.view->zoom()
        << ",\n  \"scroll_y\": " << app.view->scroll_y()
        << ",\n  \"fullscreen\": " << (app.fullscreen ? "true" : "false")
        << ",\n  \"render_dpi\": " << (unsigned)(app.view->dpi_scale() * 96)
        << ",\n  \"editing\": " << (app.editor ? "true" : "false")
        << ",\n  \"asset_bytes\": " << app.view->asset_bytes()
        << ",\n  \"assets_pending\": " << (app.view->assets_pending() ? "true" : "false")
        << ",\n  \"remote_images_enabled\": " << (app.view->remote_images_enabled() ? "true" : "false")
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
    case ui::WM_SCROLL_TO:
        if (w == SB_VERT)
            view.scroll_to((float)l);
        else
            view.hscroll((float)l - view.scroll_x());
        return 0;
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
        if (app->cursorHidden && LOWORD(l) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
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
        if (app->fullscreen)
            wake_cursor(*app, hwnd);
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
        case VK_SPACE:
            view.scroll((GetKeyState(VK_SHIFT) < 0 ? -1 : 1) * view.page_height() * .9f);
            break;
        case VK_HOME:
            view.scroll_to(0);
            break;
        case VK_END:
            view.scroll_to(view.total_height());
            break;
        // In full screen Left/Right page like a presenter remote; otherwise they scroll wide content.
        case VK_LEFT:
            if (app->fullscreen)
                view.scroll(-view.page_height() * .9f);
            else
                view.hscroll(-40);
            break;
        case VK_RIGHT:
            if (app->fullscreen)
                view.scroll(view.page_height() * .9f);
            else
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
        app->toolbar =
            CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                            WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_WRAPABLE |
                                TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NODIVIDER | CCS_NOPARENTALIGN,
                            0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(app->toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(app->toolbar, WM_SETFONT, (WPARAM)app->font, TRUE);
        const struct {
            int id;
            const wchar_t *label;
        } buttons[] = {{Open, L"打开"},  {Back, L"←"},         {Forward, L"→"},      {Toc, L"目录"},
                       {Find, L"查找"},  {ZoomOut, L"−"},      {ZoomReset, L"100%"}, {ZoomIn, L"＋"},
                       {Theme, L"主题"}, {Width, L"阅读宽度"}, {Edit, L"编辑"},      {EditMode, L"模式"},
                       {Save, L"保存"},  {Split, L"双栏"},     {FormatH1, L"标题 1"}, {FormatH2, L"标题 2"},
                       {FormatBold, L"加粗"}, {FormatItalic, L"斜体"}, {FormatBullet, L"列表"},
                       {FormatNumbered, L"编号"}, {FormatQuote, L"引用"}, {FormatCode, L"代码"},
                       {FormatDiagram, L"流程图"}};
        for (const auto &button : buttons) {
            TBBUTTON b{};
            b.iBitmap = I_IMAGENONE;
            b.idCommand = button.id;
            b.fsState = TBSTATE_ENABLED;
            b.fsStyle = BTNS_BUTTON | BTNS_SHOWTEXT;
            b.iString = reinterpret_cast<INT_PTR>(button.label);
            SendMessageW(app->toolbar, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&b));
            bool group = button.id == Forward || button.id == Find || button.id == ZoomIn || button.id == Width;
            if (group || button.id == Split || button.id == FormatH2 || button.id == FormatItalic ||
                button.id == FormatQuote) {
                TBBUTTON sep{};
                sep.fsStyle = BTNS_SEP;
                sep.iBitmap = 12;
                // Separators around the formatting buttons hide together with them.
                sep.idCommand = group ? 400 + button.id : EditSeparator + button.id - Split;
                SendMessageW(app->toolbar, TB_ADDBUTTONSW, 1, (LPARAM)&sep);
            }
        }
        app->status = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0,
                                      0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(app->status, WM_SETFONT, (WPARAM)app->font, TRUE);
        SetWindowSubclass(app->status, status_proc, 1, (DWORD_PTR)app);
        app->reader = CreateWindowExW(0, L"KeepMD.Reader", nullptr,
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0, 0,
                                      0, hwnd, nullptr, nullptr, app);
        app->remoteImages = CreateWindowExW(0, L"BUTTON", L"网络图片（已关闭）",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1, hwnd, (HMENU)RemoteImages, nullptr, nullptr);
        app->view = new View(app->reader, app->imageCache);
        ui::attach_scrollbars(app->reader, ui::ScrollKind::Reader);
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
        app->view->set_dark(app->preferences.dark);
        app->view->set_zoom(app->preferences.zoom);
        app->view->set_reading_width(app->preferences.reading_width);
        chrome_theme(*app);
        ui::caption_actions(hwnd, {{ModeRead, ui::Icon::Book, L"阅读", L"阅读 · 预览当前文档"},
                                   {ModeVisual, ui::Icon::Edit, L"可视化编辑", L"可视化编辑 · 直接编辑排版后的内容"},
                                   {ModeSource, ui::Icon::Markdown, L"源码编辑", L"源码编辑 · 编辑 Markdown"}});
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
    case ui::WM_TOGGLE_TOOLBAR:
        command(*app, ToggleToolbar);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        auto p = ui::palette(app->view && app->view->dark());
        ui::fill(dc, r, p.background);
        auto px = [&](int n) { return (int)(n * app->dpi); };
        if (app->pageCard.bottom > app->pageCard.top) {
            RECT shadow = app->pageCard;
            OffsetRect(&shadow, 0, px(2));
            ui::rounded(dc, shadow, p.border, p.border, px(12));
            ui::rounded(dc, app->pageCard, p.surface, p.border, px(12));
            RECT label{app->pageCard.left + px(18), app->pageCard.top + px(4),
                       app->pageCard.right - px(130), app->pageCard.top + px(34)};
            auto name = app->path.empty() ? (app->editor ? L"未命名文档" : L"文档工作区") : app->path.filename().wstring();
            ui::text(dc, app->font, name, label, p.muted);
            ui::text(dc, app->font, app->editor && !app->readingDraft ? (app->editor->visual() ? L"可视化编辑" : L"Markdown") : L"阅读视图",
                     {app->pageCard.right - px(122), label.top, app->pageCard.right - px(16), label.bottom},
                     p.accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ui::line(dc, app->pageCard.left + px(16), label.bottom, app->pageCard.right - px(16), label.bottom, p.border);
        }
        if (app->showSearch) {
            ui::field(dc, app->searchField, p, GetFocus() == app->searchBox, app->dpi);
            if (app->showReplace)
                ui::field(dc, app->replaceField, p, GetFocus() == app->replaceBox, app->dpi);
        }
        if (app->showToc && app->toc) {
            RECT tr{};
            GetWindowRect(app->toc, &tr);
            MapWindowPoints(nullptr, hwnd, (POINT *)&tr, 2);
            RECT label{(LONG)(18 * app->dpi), tr.top - (LONG)(36 * app->dpi), tr.right,
                       tr.top - (LONG)(4 * app->dpi)};
            ui::text(dc, app->font, L"目录", label, p.muted);
            int x = (int)(220 * app->dpi) - 1;
            ui::line(dc, x, label.top, x, r.bottom - (int)(30 * app->dpi), p.border);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)l)->CtlType == ODT_BUTTON) {
            ui::draw_button(*(DRAWITEMSTRUCT *)l, app->view && app->view->dark());
            return TRUE;
        }
        if (((DRAWITEMSTRUCT *)l)->CtlType == ODT_MENU) {
            ui::draw_menu(*(DRAWITEMSTRUCT *)l, app->font, app->view && app->view->dark());
            return TRUE;
        }
        break;
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)l)->CtlType == ODT_MENU) {
            ui::measure_menu(*(MEASUREITEMSTRUCT *)l, hwnd, app->font);
            return TRUE;
        }
        break;
    case WM_CTLCOLOREDIT: {
        auto p = ui::palette(app->view && app->view->dark());
        SetTextColor((HDC)w, p.text);
        SetBkColor((HDC)w, p.surface);
        return (LRESULT)app->surfaceBrush;
    }
    case WM_SETTINGCHANGE:
        chrome_theme(*app);
        return 0;
    case WM_DPICHANGED: {
        update_ui_font(*app, LOWORD(w));
        if (app->fullscreen)
            ui::caption_fullscreen(hwnd, true); // Keep covering the monitor instead of the suggested rect.
        else {
            const auto *r = reinterpret_cast<RECT *>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        arrange(*app);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(l);
        info->ptMinTrackSize = {(LONG)(640 * app->dpi), (LONG)(420 * app->dpi)};
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == EditorId && HIWORD(w) == EN_MAXTEXT) {
            status(*app, L"编辑内容已达到缓冲上限，后续输入未加入。请先保存当前内容。");
            return 0;
        }
        if (LOWORD(w) == EditorId && HIWORD(w) == EN_CHANGE) {
            if (!app->suppressEdit && app->editor && !app->editor->internal()) {
                app->editor->changed();
                SetTimer(hwnd, 3, 350, nullptr);
                SetWindowTextW(hwnd, app->title().c_str());
            }
            return 0;
        }
        if ((LOWORD(w) == SearchId || LOWORD(w) == 203) &&
            (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS)) {
            InvalidateRect(hwnd, LOWORD(w) == SearchId ? &app->searchField : &app->replaceField, FALSE);
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
        if (notice->hwndFrom == app->toolbar && notice->code == TBN_GETINFOTIPW) {
            auto tip = (NMTBGETINFOTIPW *)l;
            wcsncpy_s(tip->pszText, tip->cchTextMax,
                      toolbar_tip(tip->iItem, app->editor != nullptr, app->editor && app->editor->visual()),
                      _TRUNCATE);
            return 0;
        }
        if (notice->hwndFrom == app->toolbar && notice->code == NM_CUSTOMDRAW) {
            auto *draw = (NMTBCUSTOMDRAW *)l;
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                ui::fill(draw->nmcd.hdc, draw->nmcd.rc,
                         ui::palette(app->view && app->view->dark()).background);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int id = (int)draw->nmcd.dwItemSpec;
                wchar_t text[128]{};
                TBBUTTONINFOW info{sizeof(info), TBIF_TEXT};
                info.pszText = text;
                info.cchText = 128;
                SendMessageW(app->toolbar, TB_GETBUTTONINFOW, id, (LPARAM)&info);
                auto r = draw->nmcd.rc;
                InflateRect(&r, -2, -2);
                bool selected = (id == Toc && app->showToc) || (id == Find && app->showSearch) ||
                                (id == Edit && app->editor);
                auto state = draw->nmcd.uItemState;
                auto p = ui::palette(app->view && app->view->dark());
                int saved = SaveDC(draw->nmcd.hdc);
                ui::icon_face(draw->nmcd.hdc, r, app->font,
                              toolbar_icon(id, app->editor != nullptr, app->editor && app->editor->visual(),
                                           app->view && app->view->dark()),
                              id == ZoomReset || id == Toc || id == Find || id == Split ? std::wstring_view(text) : std::wstring_view(), p,
                              false, selected, (state & CDIS_HOT) != 0, (state & CDIS_SELECTED) != 0,
                              (state & CDIS_DISABLED) != 0, (state & CDIS_FOCUS) != 0, app->dpi);
                RestoreDC(draw->nmcd.hdc, saved);
                return CDRF_SKIPDEFAULT;
            }
        }
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
                (size_t)data->iItem < doc->headings.size()) {
                const auto &heading = doc->headings[data->iItem];
                bool previewing = app->readingDraft || (app->editor && !app->editor->visual() && app->split);
                if (app->editor && !previewing && !is_mmd(app->path)) {
                    // The outline follows the editor when no reader preview is visible.
                    auto source = std::min(doc->blocks[heading.block].source, doc->source.size());
                    app->editor->reveal(wide(std::string_view(doc->source).substr(0, source)).size());
                } else
                    app->view->goto_block(heading.block);
            }
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
                app->readingDraft = false;
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
            update_remote_control(*app);
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
        if (w == 4) {
            KillTimer(hwnd, 4);
            if (app->fullscreen) {
                app->cursorHidden = true;
                POINT point{};
                GetCursorPos(&point);
                if (WindowFromPoint(point) == app->reader)
                    SetCursor(nullptr);
            }
            return 0;
        }
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
    case WM_QUERYENDSESSION:
        return allow_navigation(*app);
    case WM_CLOSE:
        if (allow_navigation(*app))
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
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
    ui::set_brand(L"KeepMD", ui::Accent::Blue);
    OleInitialize(nullptr);
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
    std::filesystem::path initial;
    bool background = false;
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--resident")
            background = true;
        else if (arg == L"--snapshot" && i + 1 < argc)
            app.snapshot = argv[++i];
        else if (arg == L"--report" && i + 1 < argc)
            app.report = argv[++i];
        else if (arg == L"--config" && i + 1 < argc)
            app.settingsPath = std::filesystem::absolute(argv[++i]);
        else if (arg == L"--image-cache" && i + 1 < argc)
            app.imageCache = std::filesystem::absolute(argv[++i]);
        else if (arg == L"--render-dpi" && i + 1 < argc)
            app.renderDpi = std::clamp(_wtoi(argv[++i]), 96, 288);
        else if (arg == L"--exit-after" && i + 1 < argc)
            app.autoExitMs = _wtoi(argv[++i]);
        else if (!arg.starts_with(L"--"))
            initial = arg;
    }
    LocalFree(argv);
    // KeepMD 0.4.5 and earlier started a background prompt service with --resident, and the startup entries they
    // registered still pass it. KeepMD has no background service any more, so such a start shows nothing.
    if (background && initial.empty()) {
        OleUninitialize();
        return 0;
    }
    HMENU menu = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu();
    app.recentMenu = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, New, L"新建\tCtrl+N");
    AppendMenuW(file, MF_STRING, Open, L"打开…\tCtrl+O");
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(app.recentMenu), L"最近文件");
    AppendMenuW(file, MF_STRING, Save, L"保存\tCtrl+S");
    AppendMenuW(file, MF_STRING, SaveAs, L"另存为…\tCtrl+Shift+S");
    AppendMenuW(file, MF_STRING, Reload, L"重新加载\tF5");
    AppendMenuW(file, MF_STRING, RegisterOpenWith, L"添加到打开方式");
    AppendMenuW(file, MF_STRING, SetDefault, L"设为默认打开程序…");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, Exit, L"退出");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"文件");
    AppendMenuW(view, MF_STRING, ModeRead, L"阅读");
    AppendMenuW(view, MF_STRING, ModeVisual, L"可视化编辑");
    AppendMenuW(view, MF_STRING, ModeSource, L"源码编辑");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, Edit, L"编辑 / 阅读\tF6");
    AppendMenuW(view, MF_STRING, EditMode, L"可视化 / 源码编辑\tCtrl+E");
    AppendMenuW(view, MF_STRING, Split, L"切换双栏预览");
    AppendMenuW(view, MF_STRING, Find, L"查找\tCtrl+F");
    AppendMenuW(view, MF_STRING, Replace, L"替换\tCtrl+H");
    AppendMenuW(view, MF_STRING, Toc, L"目录\tF9");
    AppendMenuW(view, MF_STRING, RemoteImages, L"加载本文网络图片");
    AppendMenuW(view, MF_STRING, Theme, L"深浅主题\tCtrl+D");
    AppendMenuW(view, MF_STRING, ZoomReset, L"重置缩放\tCtrl+0");
    AppendMenuW(view, MF_STRING | MF_CHECKED, ToggleToolbar, L"显示工具栏\tCtrl+Shift+T");
    AppendMenuW(view, MF_STRING, Fullscreen, L"全屏\tF11");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)view, L"视图");
    AppendMenuW(menu, MF_STRING, About, L"关于");
    ui::menu_labels(menu, {L"文件", L"视图", L"关于"});
    auto hwnd = CreateWindowExW(0, L"KeepMD.Window", L"KeepMD", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1060, 820, nullptr, menu, instance, &app);
    if (!hwnd)
        return 1;
    ShowWindow(hwnd, show);
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
                    {FVIRTKEY | FCONTROL, 'E', EditMode},
                    {FVIRTKEY | FCONTROL, 'F', Find},
                    {FVIRTKEY, VK_F3, FindNext},
                    {FVIRTKEY | FSHIFT, VK_F3, FindPrevious},
                    {FVIRTKEY, VK_F9, Toc},
                    {FVIRTKEY, VK_F11, Fullscreen},
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
        if (ui::caption_translate(message))
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
        // Esc leaves full screen from the reader; editors keep Esc for their own use and leave with F11.
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && app.fullscreen && !inEditor) {
            set_fullscreen(app, false);
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
    OleUninitialize();
    return (int)message.wParam;
}
