#include "ui.h"
#include <algorithm>
#include <commctrl.h>
#include <richedit.h>
#include <windowsx.h>

namespace keepmd::ui {
namespace {
constexpr wchar_t Property[] = L"KeepMD.ScrollSkin";
struct Skin;
struct Bar {
    Skin *skin = nullptr;
    HWND hwnd = nullptr;
    int axis = SB_VERT;
    SCROLLINFO info{sizeof(info), SIF_ALL};
    bool hot = false, dragging = false;
    int grab = 0;
    RECT thumb{};
};
struct Skin {
    HWND target = nullptr;
    ScrollKind kind = ScrollKind::Reader;
    Bar vertical{this, nullptr, SB_VERT}, horizontal{this, nullptr, SB_HORZ}, corner{this, nullptr, SB_BOTH};
    bool dark = false, syncing = false;
};
int limit(const SCROLLINFO &i) {
    return std::max(i.nMin, i.nMax - (int)std::min<UINT>(i.nPage, INT_MAX) + (i.nPage ? 1 : 0));
}
RECT thumb(Bar &bar) {
    RECT r{};
    GetClientRect(bar.hwnd, &r);
    float scale = GetDpiForWindow(bar.hwnd) / 96.f;
    int length = bar.axis == SB_VERT ? r.bottom : r.right;
    int inset = std::max(2, (int)(3 * scale));
    int track = std::max(0, length - 2 * inset);
    int range = limit(bar.info) - bar.info.nMin;
    if (!range || !track)
        return {};
    auto total = std::max(1., (double)bar.info.nMax - bar.info.nMin + 1);
    int size = std::min(track, std::max((int)(26 * scale), (int)(track * bar.info.nPage / total)));
    int start =
        inset + (int)((double)std::clamp(bar.info.nPos - bar.info.nMin, 0, range) * (track - size) / range);
    int width = std::max(4, (int)((bar.hot || bar.dragging ? 9 : 6) * scale));
    if (bar.axis == SB_VERT) {
        int x = (r.right - width) / 2;
        return {x, start, x + width, start + size};
    }
    int y = (r.bottom - width) / 2;
    return {start, y, start + size, y + width};
}
void scroll(Bar &bar, int value) {
    auto &skin = *bar.skin;
    value = std::clamp(value, bar.info.nMin, limit(bar.info));
    if (skin.kind == ScrollKind::Reader) {
        SendMessageW(skin.target, WM_SCROLL_TO, bar.axis, value);
    } else if (skin.kind == ScrollKind::Editor) {
        POINT p{};
        SendMessageW(skin.target, EM_GETSCROLLPOS, 0, (LPARAM)&p);
        if (bar.axis == SB_VERT)
            p.y = value;
        else
            p.x = value;
        SendMessageW(skin.target, EM_SETSCROLLPOS, 0, (LPARAM)&p);
    } else {
        int delta = value - bar.info.nPos;
        if (bar.axis == SB_VERT) {
            RECT item{};
            int top = ListView_GetTopIndex(skin.target);
            int height = 20;
            if (ListView_GetItemRect(skin.target, top, &item, LVIR_BOUNDS))
                height = std::max(1L, item.bottom - item.top);
            ListView_Scroll(skin.target, 0,
                            (int)std::clamp((double)delta * height, (double)INT_MIN, (double)INT_MAX));
        } else
            ListView_Scroll(skin.target, delta, 0);
    }
    sync_scrollbars(skin.target);
}
LRESULT CALLBACK bar_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto bar = (Bar *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        bar = (Bar *)((CREATESTRUCTW *)l)->lpCreateParams;
        bar->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)bar);
    }
    if (!bar)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        auto colors = palette(bar->skin->dark);
        fill(dc, r, colors.background);
        if (bar->axis != SB_BOTH) {
            bar->thumb = thumb(*bar);
            if (!IsRectEmpty(&bar->thumb)) {
                auto color = bar->dragging     ? colors.accent
                             : bar->hot        ? colors.muted
                             : bar->skin->dark ? RGB(78, 91, 111)
                                               : RGB(179, 191, 207);
                rounded(dc, bar->thumb, color, color,
                        (bar->axis == SB_VERT ? bar->thumb.right - bar->thumb.left
                                              : bar->thumb.bottom - bar->thumb.top));
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (bar->axis == SB_BOTH || limit(bar->info) <= bar->info.nMin)
            return 0;
        SetFocus(bar->skin->target);
        auto r = thumb(*bar);
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int coordinate = bar->axis == SB_VERT ? p.y : p.x;
        int start = bar->axis == SB_VERT ? r.top : r.left;
        int end = bar->axis == SB_VERT ? r.bottom : r.right;
        if (coordinate >= start && coordinate < end) {
            bar->dragging = true;
            bar->grab = coordinate - start;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
        } else {
            auto delta = std::max(1, (int)bar->info.nPage);
            double next = (double)bar->info.nPos + (coordinate < start ? -delta : delta);
            scroll(*bar, (int)std::clamp(next, (double)bar->info.nMin, (double)limit(bar->info)));
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!bar->hot) {
            bar->hot = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            InvalidateRect(h, nullptr, FALSE);
        }
        if (bar->dragging) {
            RECT r{};
            GetClientRect(h, &r);
            auto t = thumb(*bar);
            int inset = std::max(2, MulDiv(3, GetDpiForWindow(h), 96));
            int length = bar->axis == SB_VERT ? r.bottom : r.right;
            int size = bar->axis == SB_VERT ? t.bottom - t.top : t.right - t.left;
            int coordinate = bar->axis == SB_VERT ? GET_Y_LPARAM(l) : GET_X_LPARAM(l);
            int travel = std::max(1, length - 2 * inset - size);
            int offset = std::clamp(coordinate - bar->grab - inset, 0, travel);
            auto value =
                bar->info.nMin + (int)((double)offset / travel * (limit(bar->info) - bar->info.nMin));
            scroll(*bar, value);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (bar->dragging) {
            bar->dragging = false;
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        bar->dragging = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSELEAVE:
        bar->hot = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return SendMessageW(bar->skin->target, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}
void place(Bar &bar, RECT rect, bool visible) {
    if (visible) {
        RECT current{};
        GetWindowRect(bar.hwnd, &current);
        MapWindowPoints(nullptr, GetParent(bar.hwnd), (POINT *)&current, 2);
        if (!EqualRect(&rect, &current))
            SetWindowPos(bar.hwnd, HWND_TOP, rect.left, rect.top, rect.right - rect.left,
                         rect.bottom - rect.top, SWP_NOACTIVATE);
        // Split-mode changes can raise a native control without resizing it.
        // Keep the custom lanes above their target even when geometry is unchanged.
        bool above = false;
        for (HWND sibling = GetWindow(bar.skin->target, GW_HWNDPREV); sibling;
             sibling = GetWindow(sibling, GW_HWNDPREV))
            if (sibling == bar.hwnd) {
                above = true;
                break;
            }
        if (!above)
            SetWindowPos(bar.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!IsWindowVisible(bar.hwnd))
            ShowWindow(bar.hwnd, SW_SHOWNOACTIVATE);
    } else if (IsWindowVisible(bar.hwnd))
        ShowWindow(bar.hwnd, SW_HIDE);
}
LRESULT CALLBACK target_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto skin = (Skin *)data;
    if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, target_proc, id);
        RemovePropW(h, Property);
        for (auto b : {&skin->vertical, &skin->horizontal, &skin->corner})
            DestroyWindow(b->hwnd);
        delete skin;
        return DefSubclassProc(h, m, w, l);
    }
    // Place the skin before native nonclient painting, preventing default arrow
    // buttons from becoming visible during control theme/frame refreshes.
    if (m == WM_NCPAINT || m == WM_NCACTIVATE || m == WM_THEMECHANGED)
        sync_scrollbars(h);
    auto result = DefSubclassProc(h, m, w, l);
    // Native controls maintain their own scroll model. Cover the reserved nonclient
    // lanes with lightweight sibling controls; WS_CLIPSIBLINGS prevents native paint
    // from flashing through. Client geometry and native IME/edit semantics stay intact.
    switch (m) {
    case WM_PAINT:
    case WM_SIZE:
    case WM_WINDOWPOSCHANGED:
    case WM_SHOWWINDOW:
    case WM_STYLECHANGED:
    case WM_THEMECHANGED:
    case WM_NCPAINT:
    case WM_VSCROLL:
    case WM_HSCROLL:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_SETTEXT:
    case EM_REPLACESEL:
    case EM_SETSCROLLPOS:
    case LVM_SCROLL:
    case LVM_ENSUREVISIBLE:
        sync_scrollbars(h);
        break;
    }
    return result;
}
} // namespace
void attach_scrollbars(HWND target, ScrollKind kind) {
    if (!target || GetPropW(target, Property))
        return;
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = bar_proc;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"KeepMD.Scrollbar";
    RegisterClassExW(&cls);
    auto skin = new Skin;
    skin->target = target;
    skin->kind = kind;
    SetPropW(target, Property, (HANDLE)skin);
    SetWindowLongPtrW(target, GWL_STYLE, GetWindowLongPtrW(target, GWL_STYLE) | WS_CLIPSIBLINGS);
    for (auto b : {&skin->vertical, &skin->horizontal, &skin->corner}) {
        b->hwnd = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName,
                                  b->axis == SB_VERT   ? L"垂直滚动"
                                  : b->axis == SB_HORZ ? L"水平滚动"
                                                       : L"",
                                  WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, GetParent(target), nullptr,
                                  cls.hInstance, b);
    }
    SetWindowSubclass(target, target_proc, 2, (DWORD_PTR)skin);
    sync_scrollbars(target);
}
void sync_scrollbars(HWND target) {
    auto skin = (Skin *)GetPropW(target, Property);
    if (!skin || skin->syncing)
        return;
    skin->syncing = true;
    RECT wr{}, cr{};
    GetWindowRect(target, &wr);
    GetClientRect(target, &cr);
    MapWindowPoints(target, nullptr, (POINT *)&cr, 2);
    MapWindowPoints(nullptr, GetParent(target), (POINT *)&wr, 2);
    MapWindowPoints(nullptr, GetParent(target), (POINT *)&cr, 2);
    auto style = GetWindowLongPtrW(target, GWL_STYLE);
    bool show = IsWindowVisible(target) != FALSE;
    bool v = show && (style & WS_VSCROLL), h = show && (style & WS_HSCROLL);
    place(skin->vertical, {cr.right, cr.top, wr.right, cr.bottom}, v);
    place(skin->horizontal, {cr.left, cr.bottom, cr.right, wr.bottom}, h);
    place(skin->corner, {cr.right, cr.bottom, wr.right, wr.bottom}, v && h);
    for (auto bar : {&skin->vertical, &skin->horizontal}) {
        SCROLLINFO next{sizeof(next), SIF_ALL};
        GetScrollInfo(target, bar->axis, &next);
        auto &old = bar->info;
        if (old.nMin != next.nMin || old.nMax != next.nMax || old.nPage != next.nPage ||
            old.nPos != next.nPos) {
            old = next;
            InvalidateRect(bar->hwnd, nullptr, FALSE);
        }
    }
    skin->syncing = false;
}
void scroll_theme(HWND target, bool dark) {
    auto skin = (Skin *)GetPropW(target, Property);
    if (!skin)
        return;
    skin->dark = dark;
    for (auto bar : {&skin->vertical, &skin->horizontal, &skin->corner})
        InvalidateRect(bar->hwnd, nullptr, FALSE);
    sync_scrollbars(target);
}
} // namespace keepmd::ui
