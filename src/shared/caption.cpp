#include "ui.h"
#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <windowsx.h>

namespace keep::ui {
namespace {
constexpr wchar_t Property[] = L"Keep.CaptionState", Hot[] = L"Keep.CaptionHot";
constexpr int Minimize = 9101, Maximize = 9102, Close = 9103, Toolbar = 9104, MenuBase = 9200;
struct Caption {
    HWND owner = nullptr, bar = nullptr;
    HMENU menu = nullptr;
    HFONT font = nullptr, brand = nullptr;
    HICON icon = nullptr; // The application's own icon resource, sized for the caption.
    bool dark = false;
    bool toolbarExpanded = true;
    // Full screen hides the bar and the frame; the window style and placement come back unchanged on exit.
    bool fullscreen = false;
    LONG_PTR windowedStyle = 0;
    WINDOWPLACEMENT windowed{sizeof(WINDOWPLACEMENT)};
    RECT windowedRect{};
    HWND lastFocus = nullptr;
    std::vector<HWND> buttons;
    std::vector<std::pair<int, Icon>> actions;
    int selectedAction = 0;
    int action_index(int command) const {
        for (size_t i = 0; i < actions.size(); ++i)
            if (actions[i].first == command)
                return (int)i;
        return -1;
    }
    int title_right(int width) const {
        return width - px(actions.empty() ? (GetDlgItem(bar, Toolbar) ? 154 : 124)
                                         : 160 + (int)actions.size() * 30);
    }
    ~Caption() {
        if (menu)
            DestroyMenu(menu);
        if (font)
            DeleteObject(font);
        if (brand)
            DeleteObject(brand);
        if (icon)
            DestroyIcon(icon);
    }
    int px(int n) const {
        return MulDiv(n, GetDpiForWindow(owner), 96);
    }
    void fonts() {
        if (font)
            DeleteObject(font);
        if (brand)
            DeleteObject(brand);
        font = ui::font(GetDpiForWindow(owner), 12);
        brand = ui::font(GetDpiForWindow(owner), 13, FW_SEMIBOLD);
        if (icon)
            DestroyIcon(icon);
        icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, px(16), px(16),
                                 LR_DEFAULTCOLOR);
    }
    int brand_left() const {
        return px(icon ? 42 : 20);
    }
    void layout() {
        if (!bar)
            return;
        RECT r{};
        GetClientRect(owner, &r);
        MoveWindow(bar, 0, 0, r.right, px(32), TRUE);
        int x = menu_left();
        bool max = (GetWindowLongPtrW(owner, GWL_STYLE) & WS_MAXIMIZEBOX) != 0;
        for (auto button : buttons) {
            int id = GetDlgCtrlID(button);
            int action = action_index(id);
            if (action >= 0) {
                MoveWindow(button, r.right - px(148 + ((int)actions.size() - action) * 30), px(4),
                           px(28), px(24), TRUE);
            } else if (id >= MenuBase) {
                MoveWindow(button, x, px(4), px(54), px(24), TRUE);
                x += px(56);
            } else {
                int index = id == Close ? 0 : id == Maximize ? 1 : id == Toolbar ? 3 : max ? 2 : 1;
                MoveWindow(button, r.right - px(12 + 32 * (index + 1)), px(4), px(30), px(24), TRUE);
                if (id == Maximize)
                    SetWindowTextW(button, IsZoomed(owner) ? L"还原窗口" : L"最大化窗口");
            }
            SendMessageW(button, WM_SETFONT, (WPARAM)font, FALSE);
        }
        RedrawWindow(bar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    int brand_width() const {
        auto dc = GetDC(owner);
        auto old = SelectObject(dc, brand);
        SIZE size{};
        GetTextExtentPoint32W(dc, app_name().c_str(), (int)app_name().size(), &size);
        SelectObject(dc, old);
        ReleaseDC(owner, dc);
        return size.cx;
    }
    // Menu labels follow the product name, whose width differs between applications.
    int menu_left() const {
        return brand_left() + brand_width() + px(14);
    }
    void menu_action(int index) {
        if (!menu)
            return;
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STATE;
        if (!GetMenuItemInfoW(menu, index, TRUE, &item) || (item.fState & MFS_DISABLED))
            return;
        if (item.hSubMenu) {
            RECT r{};
            GetWindowRect(GetDlgItem(bar, MenuBase + index), &r);
            SendMessageW(owner, WM_INITMENU, (WPARAM)menu, 0);
            SendMessageW(owner, WM_INITMENUPOPUP, (WPARAM)item.hSubMenu, MAKELPARAM(index, FALSE));
            auto cmd = TrackPopupMenu(item.hSubMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left,
                                      r.bottom, 0, owner, nullptr);
            if (cmd)
                PostMessageW(owner, WM_COMMAND, cmd, 0);
        } else
            PostMessageW(owner, WM_COMMAND, item.wID, 0);
        if (IsWindow(lastFocus) && IsChild(owner, lastFocus))
            SetFocus(lastFocus);
    }
    void system_menu(POINT point) {
        auto sys = GetSystemMenu(owner, FALSE);
        bool zoomed = IsZoomed(owner) != FALSE;
        EnableMenuItem(sys, SC_RESTORE, MF_BYCOMMAND | (zoomed || IsIconic(owner) ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(
            sys, SC_MAXIMIZE,
            MF_BYCOMMAND |
                (!zoomed && (GetWindowLongPtrW(owner, GWL_STYLE) & WS_MAXIMIZEBOX) ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(
            sys, SC_SIZE,
            MF_BYCOMMAND |
                (!zoomed && (GetWindowLongPtrW(owner, GWL_STYLE) & WS_THICKFRAME) ? MF_ENABLED : MF_GRAYED));
        auto cmd = TrackPopupMenu(sys, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, owner, nullptr);
        if (cmd)
            PostMessageW(owner, WM_SYSCOMMAND, cmd, 0);
    }
};
Caption *get(HWND h) {
    return (Caption *)GetPropW(h, Property);
}
LRESULT CALLBACK button_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto p = (Caption *)data;
    if (m == WM_MOUSEMOVE && !GetPropW(h, Hot)) {
        SetPropW(h, Hot, (HANDLE)1);
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        RemovePropW(h, Hot);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_SETFOCUS) {
        if ((HWND)w && GetParent((HWND)w) != p->bar)
            p->lastFocus = (HWND)w;
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_KILLFOCUS) {
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_NCHITTEST) {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT r{};
        GetWindowRect(p->owner, &r);
        if (!IsZoomed(p->owner) && pt.y < r.top + p->px(6))
            return HTTRANSPARENT;
    } else if (m == WM_NCDESTROY) {
        RemovePropW(h, Hot);
        RemoveWindowSubclass(h, button_proc, id);
    }
    return DefSubclassProc(h, m, w, l);
}
void draw(Caption &p, const DRAWITEMSTRUCT &d) {
    int saved = SaveDC(d.hDC);
    auto c = palette(p.dark);
    auto r = d.rcItem;
    fill(d.hDC, r, c.background);
    bool hot = GetPropW(d.hwndItem, Hot) != nullptr, pressed = (d.itemState & ODS_SELECTED) != 0;
    int action = p.action_index(d.CtlID);
    if (action >= 0) {
        ui::icon_face(d.hDC, r, p.font, p.actions[action].second, {}, c, false,
                      (int)d.CtlID == p.selectedAction, hot, pressed, (d.itemState & ODS_DISABLED) != 0,
                      (d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT),
                      GetDpiForWindow(p.owner) / 96.f);
        RestoreDC(d.hDC, saved);
        return;
    }
    bool close = d.CtlID == Close;
    auto fg = close && (hot || pressed) ? RGB(255, 255, 255) : c.muted;
    if (hot || pressed) {
        auto bg = close ? c.danger : pressed ? c.pressed : c.hover;
        rounded(d.hDC, r, bg, bg, p.px(8));
    }
    if (d.CtlID >= MenuBase) {
        wchar_t label[80]{};
        GetWindowTextW(d.hwndItem, label, 80);
        text(d.hDC, p.font, label, r, c.text, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        int x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2, s = p.px(5);
        auto pen = CreatePen(PS_SOLID, std::max(1, p.px(1)), fg);
        auto old = SelectObject(d.hDC, pen);
        auto segment = [&](int a, int b, int z, int q) {
            MoveToEx(d.hDC, a, b, nullptr);
            LineTo(d.hDC, z, q);
        };
        if (close) {
            segment(x - s, y - s, x + s + 1, y + s + 1);
            segment(x + s, y - s, x - s - 1, y + s + 1);
        } else if (d.CtlID == Toolbar) {
            int sign = p.toolbarExpanded ? 1 : -1;
            segment(x - s, y + sign * 2, x, y - sign * 3);
            segment(x, y - sign * 3, x + s + 1, y + sign * 2);
        } else if (d.CtlID == Minimize)
            segment(x - s, y + 2, x + s + 1, y + 2);
        else {
            auto brush = SelectObject(d.hDC, GetStockObject(NULL_BRUSH));
            if (IsZoomed(p.owner)) {
                Rectangle(d.hDC, x - s + 3, y - s - 2, x + s + 3, y + s - 2);
                fill(d.hDC, {x - s - 1, y - s + 1, x + s + 1, y + s + 3}, hot ? c.hover : c.background);
            }
            Rectangle(d.hDC, x - s, y - s, x + s + 1, y + s + 1);
            SelectObject(d.hDC, brush);
        }
        SelectObject(d.hDC, old);
        DeleteObject(pen);
    }
    if ((d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(d.hDC, &r);
    }
    RestoreDC(d.hDC, saved);
}
LRESULT CALLBACK bar_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto p = (Caption *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        p = (Caption *)((CREATESTRUCTW *)l)->lpCreateParams;
        p->bar = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
    }
    if (!p)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        auto c = palette(p->dark);
        fill(dc, r, c.background);
        if (p->icon)
            DrawIconEx(dc, p->px(18), (r.bottom - p->px(16)) / 2, p->icon, p->px(16), p->px(16), 0, nullptr, DI_NORMAL);
        text(dc, p->brand, app_name(), {p->brand_left(), 0, p->brand_left() + p->brand_width(), r.bottom},
             c.text);
        int count = p->menu ? GetMenuItemCount(p->menu) : 0;
        wchar_t title[1024]{};
        GetWindowTextW(p->owner, title, 1024);
        std::wstring name = title;
        auto suffix = name.find(L" — " + app_name());
        if (suffix != std::wstring::npos)
            name.resize(suffix);
        if (name == app_name() || name.starts_with(app_name() + L" ·"))
            name.clear();
        text(dc, p->font, name,
             {p->menu_left() + p->px(12 + count * 56), 0, p->title_right(r.right),
              r.bottom},
             c.muted);
        line(dc, 0, r.bottom - 1, r.right, r.bottom - 1, c.border);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        draw(*p, *(DRAWITEMSTRUCT *)l);
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (p->action_index(id) >= 0)
            SendMessageW(p->owner, WM_COMMAND, id, 0);
        else if (id >= MenuBase)
            p->menu_action(id - MenuBase);
        else if (id == Toolbar)
            PostMessageW(p->owner, WM_TOGGLE_TOOLBAR, 0, 0);
        else
            PostMessageW(p->owner, WM_SYSCOMMAND,
                         id == Close          ? SC_CLOSE
                         : id == Minimize     ? SC_MINIMIZE
                         : IsZoomed(p->owner) ? SC_RESTORE
                                              : SC_MAXIMIZE,
                         0);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK frame_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto p = (Caption *)data;
    switch (m) {
    case WM_NCCALCSIZE: {
        auto r = w ? &((NCCALCSIZE_PARAMS *)l)->rgrc[0] : (RECT *)l;
        if (p->fullscreen)
            return 0; // The whole window is client area.
        if (IsZoomed(h)) {
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &info);
            *r = info.rcWork;
        } else {
            int border = p->px((GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME) ? 4 : 1);
            InflateRect(r, -border, -border);
        }
        return 0;
    }
    case WM_NCPAINT: {
        auto dc = GetWindowDC(h);
        if (!dc)
            return 0;
        RECT window{}, client{};
        GetWindowRect(h, &window);
        GetClientRect(h, &client);
        MapWindowPoints(h, nullptr, (POINT *)&client, 2);
        OffsetRect(&client, -window.left, -window.top);
        ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
        auto colors = palette(p->dark);
        RECT r{0, 0, window.right - window.left, window.bottom - window.top};
        fill(dc, r, colors.background);
        auto pen = SelectObject(dc, GetStockObject(DC_PEN));
        auto brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetDCPenColor(dc, colors.border);
        Rectangle(dc, 0, 0, r.right, r.bottom);
        SelectObject(dc, pen);
        SelectObject(dc, brush);
        ReleaseDC(h, dc);
        return 0;
    }
    case WM_NCACTIVATE:
        InvalidateRect(p->bar, nullptr, FALSE);
        return TRUE;
    case WM_NCHITTEST: {
        if (p->fullscreen)
            return HTCLIENT;
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT r{};
        GetWindowRect(h, &r);
        int x = point.x - r.left, y = point.y - r.top, b = p->px(6);
        if (!IsZoomed(h) && (GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME)) {
            bool left = x < b, right = x >= r.right - r.left - b, top = y < b,
                 bottom = y >= r.bottom - r.top - b;
            if (top)
                return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
            if (bottom)
                return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            if (left)
                return HTLEFT;
            if (right)
                return HTRIGHT;
        }
        ScreenToClient(h, &point);
        if (point.y < caption_height(h)) {
            auto child = ChildWindowFromPointEx(p->bar, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            return child == p->bar ? HTCAPTION : HTCLIENT;
        }
        return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK:
        if (w == HTCAPTION) {
            if (GetWindowLongPtrW(h, GWL_STYLE) & WS_MAXIMIZEBOX)
                ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        break;
    case WM_NCRBUTTONUP:
        if (w == HTCAPTION) {
            p->system_menu({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            return 0;
        }
        break;
    case WM_SIZE: {
        auto result = DefSubclassProc(h, m, w, l);
        p->layout();
        return result;
    }
    case WM_DPICHANGED: {
        p->fonts();
        auto result = DefSubclassProc(h, m, w, l);
        p->layout();
        return result;
    }
    case WM_SETTEXT: {
        auto result = DefSubclassProc(h, m, w, l);
        InvalidateRect(p->bar, nullptr, FALSE);
        return result;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, frame_proc, id);
        RemovePropW(h, Property);
        delete p;
        return DefSubclassProc(h, m, w, l);
    }
    return DefSubclassProc(h, m, w, l);
}
void add_button(Caption &p, int id, const wchar_t *label) {
    auto button = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0,
                                  0, 1, 1, p.bar, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SetWindowSubclass(button, button_proc, 1, (DWORD_PTR)&p);
    p.buttons.push_back(button);
}
} // namespace
int caption_height(HWND hwnd) {
    auto p = get(hwnd);
    return p && !p->fullscreen ? MulDiv(32, GetDpiForWindow(hwnd), 96) : 0;
}
bool caption_fullscreen(HWND hwnd, bool on) {
    auto p = get(hwnd);
    if (!p)
        return false;
    if (on) {
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
            return p->fullscreen;
        if (!p->fullscreen) {
            p->windowed.length = sizeof(p->windowed);
            GetWindowPlacement(hwnd, &p->windowed);
            GetWindowRect(hwnd, &p->windowedRect);
            p->windowedStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
            p->fullscreen = true;
            ShowWindow(p->bar, SW_HIDE);
            SetWindowLongPtrW(hwnd, GWL_STYLE, p->windowedStyle & ~(WS_CAPTION | WS_THICKFRAME));
        }
        // Covering the whole monitor, not the work area, lets the taskbar step back. Also re-fits after a DPI
        // or monitor change while already full screen.
        const auto &r = info.rcMonitor;
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else if (p->fullscreen) {
        p->fullscreen = false;
        SetWindowLongPtrW(hwnd, GWL_STYLE, p->windowedStyle);
        // The placement restores the show state and the restore position, but a window that stayed maximized
        // throughout is not moved by it, so the exact previous rectangle is put back as well.
        SetWindowPlacement(hwnd, &p->windowed);
        const auto &r = p->windowedRect;
        SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowWindow(p->bar, SW_SHOWNA);
        p->layout();
    }
    return p->fullscreen;
}
void caption_actions(HWND hwnd, std::initializer_list<CaptionAction> actions) {
    auto p = get(hwnd);
    if (!p)
        return;
    for (auto it = p->buttons.begin(); it != p->buttons.end();)
        if (p->action_index(GetDlgCtrlID(*it)) >= 0) {
            DestroyWindow(*it);
            it = p->buttons.erase(it);
        } else
            ++it;
    p->actions.clear();
    for (const auto &action : actions) {
        p->actions.emplace_back(action.command, action.icon);
        add_button(*p, action.command, action.label);
        auto button = GetDlgItem(p->bar, action.command);
        ui::style_button(button);
        ui::icon_button(button, action.icon, action.tip);
    }
    p->layout();
}
void caption_select(HWND hwnd, int command) {
    auto p = get(hwnd);
    if (!p || p->selectedAction == command)
        return;
    p->selectedAction = command;
    for (const auto &[id, icon] : p->actions)
        InvalidateRect(GetDlgItem(p->bar, id), nullptr, FALSE);
}
void caption_toolbar(HWND hwnd, bool expanded) {
    auto p = get(hwnd);
    if (!p)
        return;
    auto button = GetDlgItem(p->bar, Toolbar);
    bool created = !button;
    if (created) {
        add_button(*p, Toolbar, L"收起工具栏");
        button = GetDlgItem(p->bar, Toolbar);
        ui::style_button(button);
        ui::icon_button(button, Icon::None, L"收起 / 展开工具栏 · Ctrl+Shift+T");
        p->layout();
    }
    if (created || p->toolbarExpanded != expanded) {
        p->toolbarExpanded = expanded;
        SetWindowTextW(button, expanded ? L"收起工具栏" : L"展开工具栏");
        InvalidateRect(button, nullptr, FALSE);
    }
}
HMENU window_menu(HWND hwnd) {
    auto p = get(hwnd);
    return p ? p->menu : GetMenu(hwnd);
}
void caption_menu(HWND hwnd, HMENU menu) {
    auto p = get(hwnd);
    if (!p)
        return;
    for (auto it = p->buttons.begin(); it != p->buttons.end();)
        if (GetDlgCtrlID(*it) >= MenuBase) {
            DestroyWindow(*it);
            it = p->buttons.erase(it);
        } else
            ++it;
    if (p->menu && p->menu != menu)
        DestroyMenu(p->menu);
    p->menu = menu;
    if (GetMenu(hwnd))
        SetMenu(hwnd, nullptr);
    if (menu)
        for (int i = 0; i < GetMenuItemCount(menu); ++i) {
            wchar_t name[80]{};
            MENUITEMINFOW info{sizeof(info)};
            info.fMask = MIIM_DATA | MIIM_STRING;
            info.dwTypeData = name;
            info.cch = 80;
            GetMenuItemInfoW(menu, i, TRUE, &info);
            add_button(*p, MenuBase + i, info.dwItemData ? (const wchar_t *)info.dwItemData : name);
        }
    p->layout();
}
void titlebar(HWND hwnd, bool dark) {
    auto p = get(hwnd);
    if (!p) {
        p = new Caption;
        p->owner = hwnd;
        p->dark = dark;
        p->fonts();
        SetPropW(hwnd, Property, (HANDLE)p);
        SetWindowSubclass(hwnd, frame_proc, 21, (DWORD_PTR)p);
        auto menu = GetMenu(hwnd);
        if (menu)
            SetMenu(hwnd, nullptr);
        SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) & ~WS_CAPTION);
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = bar_proc;
        cls.lpszClassName = L"Keep.Caption";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        p->bar = CreateWindowExW(0, cls.lpszClassName, L"窗口操作", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                 0, 0, 1, 1, hwnd, nullptr, cls.hInstance, p);
        auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (style & WS_MINIMIZEBOX)
            add_button(*p, Minimize, L"最小化窗口");
        if (style & WS_MAXIMIZEBOX)
            add_button(*p, Maximize, L"最大化窗口");
        add_button(*p, Close, L"关闭窗口");
        caption_menu(hwnd, menu);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    p->dark = dark;
    BOOL value = dark;
    DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value));
    DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    RedrawWindow(p->bar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
}
bool caption_translate(MSG &msg) {
    auto root = GetAncestor(msg.hwnd, GA_ROOT);
    auto p = get(root);
    // The bar is hidden in full screen, so its menu, toolbar and system-menu keys have nothing to act on.
    if (!p || p->fullscreen)
        return false;
    if (msg.message == WM_KEYDOWN && msg.wParam == 'T' && GetKeyState(VK_CONTROL) < 0 &&
        GetKeyState(VK_SHIFT) < 0 && GetDlgItem(p->bar, Toolbar)) {
        PostMessageW(root, WM_TOGGLE_TOOLBAR, 0, 0);
        return true;
    }
    if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_SPACE) {
        POINT point{p->px(12), caption_height(root)};
        ClientToScreen(root, &point);
        p->system_menu(point);
        return true;
    }
    bool inside = GetParent(msg.hwnd) == p->bar;
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_F10) {
        p->lastFocus = GetFocus();
        auto first = GetDlgItem(p->bar, MenuBase);
        SetFocus(first ? first : GetDlgItem(p->bar, Close));
        return true;
    }
    if (msg.message == WM_KEYDOWN && inside) {
        if (msg.wParam == VK_ESCAPE) {
            if (IsWindow(p->lastFocus))
                SetFocus(p->lastFocus);
            return true;
        }
        if (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT || msg.wParam == VK_TAB) {
            auto next = GetNextDlgTabItem(p->bar, msg.hwnd,
                                          msg.wParam == VK_LEFT ||
                                              (msg.wParam == VK_TAB && GetKeyState(VK_SHIFT) < 0));
            if (next)
                SetFocus(next);
            return true;
        }
        if (msg.wParam == VK_RETURN || msg.wParam == VK_DOWN) {
            SendMessageW(msg.hwnd, BM_CLICK, 0, 0);
            return true;
        }
    }
    return false;
}
} // namespace keep::ui
