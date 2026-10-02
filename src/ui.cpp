#include "ui.h"
#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <string>

namespace keepmd::ui {
namespace {
constexpr wchar_t Style[] = L"KeepMD.ButtonStyle", Hot[] = L"KeepMD.ButtonHot";
constexpr wchar_t IconData[] = L"KeepMD.IconButton", Tips[] = L"KeepMD.ButtonTips";
struct IconButton {
    Icon value = Icon::None;
    bool label = false;
    HWND tooltip = nullptr;
    std::wstring tip;
};
LRESULT CALLBACK tips_owner(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_NCDESTROY) {
        auto tip = (HWND)RemovePropW(h, Tips);
        if (IsWindow(tip))
            DestroyWindow(tip);
        RemoveWindowSubclass(h, tips_owner, id);
    }
    return DefSubclassProc(h, m, w, l);
}
LRESULT CALLBACK button_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_MOUSEMOVE && !GetPropW(h, Hot)) {
        SetPropW(h, Hot, (HANDLE)1);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
        TrackMouseEvent(&track);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        RemovePropW(h, Hot);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_NCDESTROY) {
        if (auto data = (IconButton *)RemovePropW(h, IconData)) {
            if (IsWindow(data->tooltip)) {
                TOOLINFOW tool{sizeof(tool)};
                tool.hwnd = GetParent(h);
                tool.uId = (UINT_PTR)h;
                tool.uFlags = TTF_IDISHWND;
                SendMessageW(data->tooltip, TTM_DELTOOLW, 0, (LPARAM)&tool);
            }
            delete data;
        }
        RemovePropW(h, Style);
        RemovePropW(h, Hot);
        RemoveWindowSubclass(h, button_proc, id);
    }
    return DefSubclassProc(h, m, w, l);
}
} // namespace
void icon_button(HWND hwnd, Icon value, const wchar_t *tip, bool showLabel) {
    auto data = (IconButton *)GetPropW(hwnd, IconData);
    bool existing = data != nullptr;
    if (!data) {
        data = new IconButton;
        SetPropW(hwnd, IconData, (HANDLE)data);
    }
    data->value = value;
    data->label = showLabel;
    data->tip = tip;
    auto parent = GetParent(hwnd);
    auto tooltip = (HWND)GetPropW(parent, Tips);
    if (!tooltip) {
        tooltip = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetPropW(parent, Tips, (HANDLE)tooltip);
        SetWindowSubclass(parent, tips_owner, 35, 0);
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
        SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 10000);
    }
    data->tooltip = tooltip;
    TOOLINFOW tool{sizeof(tool)};
    tool.hwnd = parent;
    tool.uId = (UINT_PTR)hwnd;
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.lpszText = data->tip.data();
    SendMessageW(tooltip, existing ? TTM_UPDATETIPTEXTW : TTM_ADDTOOLW, 0, (LPARAM)&tool);
    InvalidateRect(hwnd, nullptr, FALSE);
}
void set_button_icon(HWND hwnd, Icon value) {
    if (auto data = (IconButton *)GetPropW(hwnd, IconData)) {
        data->value = value;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}
Palette palette(bool dark) {
    HIGHCONTRASTW high{sizeof(high)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high), &high, 0) &&
        (high.dwFlags & HCF_HIGHCONTRASTON))
        return {
            GetSysColor(COLOR_BTNFACE),    GetSysColor(COLOR_WINDOW),        GetSysColor(COLOR_WINDOWTEXT),
            GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOWTEXT),    GetSysColor(COLOR_BTNFACE),
            GetSysColor(COLOR_HIGHLIGHT),  GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_HIGHLIGHT)};
    return dark ? Palette{RGB(23, 28, 36),    RGB(30, 36, 47), RGB(228, 234, 243),
                          RGB(154, 169, 190), RGB(53, 64, 81), RGB(43, 53, 69),
                          RGB(125, 170, 255), RGB(15, 30, 52), RGB(42, 61, 87)}
                : Palette{RGB(245, 247, 251), RGB(255, 255, 255), RGB(35, 48, 68),
                          RGB(100, 115, 137), RGB(217, 224, 234), RGB(232, 238, 247),
                          RGB(43, 96, 190),   RGB(255, 255, 255), RGB(223, 235, 255)};
}
void fill(HDC dc, RECT rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, (HBRUSH)GetStockObject(DC_BRUSH));
}
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color) {
    auto old = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, color);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
}
void rounded(HDC dc, RECT rect, COLORREF color, COLORREF border, int radius) {
    auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, border);
    SetDCBrushColor(dc, color);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
}
void text(HDC dc, HFONT font, std::wstring_view value, RECT rect, COLORREF color, UINT flags) {
    auto old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value.data(), (int)value.size(), &rect, flags);
    SelectObject(dc, old);
}
HFONT font(unsigned dpi, int size, int weight) {
    return CreateFontW(-MulDiv(size, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       L"Segoe UI");
}
void style_button(HWND h, bool primary, bool checked) {
    auto style = GetWindowLongPtrW(h, GWL_STYLE);
    SetWindowLongPtrW(h, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
    SetPropW(h, Style, (HANDLE)(INT_PTR)(1 | (primary ? 2 : 0) | (checked ? 4 : 0)));
    SetWindowSubclass(h, button_proc, 1, 0);
    InvalidateRect(h, nullptr, FALSE);
}
void button_face(HDC dc, RECT rect, HFONT font, std::wstring_view title, const Palette &p, bool primary,
                 bool checked, bool hot, bool pressed, bool disabled, bool focused, float scale) {
    auto bg = primary && !disabled ? p.accent : checked ? p.selected : hot || pressed ? p.hover : p.surface;
    auto fg = disabled ? p.muted : primary ? p.accentText : checked ? p.accent : p.text;
    rounded(dc, rect, bg, primary || checked || focused ? p.accent : p.border, (int)(10 * scale));
    RECT label = rect;
    InflateRect(&label, -(int)(7 * scale), 0);
    if (pressed)
        OffsetRect(&label, 0, 1);
    text(dc, font, title, label, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (focused) {
        RECT focus = rect;
        InflateRect(&focus, -(int)(4 * scale), -(int)(4 * scale));
        SetTextColor(dc, fg);
        SetBkColor(dc, bg);
        DrawFocusRect(dc, &focus);
    }
}
void draw_button(const DRAWITEMSTRUCT &item, bool dark) {
    int saved = SaveDC(item.hDC);
    auto p = palette(dark);
    fill(item.hDC, item.rcItem, p.background);
    wchar_t title[160]{};
    GetWindowTextW(item.hwndItem, title, 160);
    auto flags = (INT_PTR)GetPropW(item.hwndItem, Style);
    auto rect = item.rcItem;
    InflateRect(&rect, -1, -1);
    if (auto data = (IconButton *)GetPropW(item.hwndItem, IconData)) {
        icon_face(item.hDC, rect, (HFONT)SendMessageW(item.hwndItem, WM_GETFONT, 0, 0), data->value,
                  data->label ? std::wstring_view(title) : std::wstring_view(), p, (flags & 2) != 0,
                  (flags & 4) != 0, GetPropW(item.hwndItem, Hot) != nullptr,
                  (item.itemState & ODS_SELECTED) != 0, (item.itemState & ODS_DISABLED) != 0,
                  (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT),
                  GetDpiForWindow(item.hwndItem) / 96.f);
    } else
        button_face(item.hDC, rect, (HFONT)SendMessageW(item.hwndItem, WM_GETFONT, 0, 0), title, p,
                    (flags & 2) != 0, (flags & 4) != 0, GetPropW(item.hwndItem, Hot) != nullptr,
                    (item.itemState & ODS_SELECTED) != 0, (item.itemState & ODS_DISABLED) != 0,
                    (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT),
                    GetDpiForWindow(item.hwndItem) / 96.f);
    RestoreDC(item.hDC, saved);
}
void menu_labels(HMENU menu, std::initializer_list<const wchar_t *> labels) {
    int index = 0;
    for (auto title : labels) {
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_FTYPE | MIIM_DATA;
        info.fType = MFT_OWNERDRAW;
        info.dwItemData = (ULONG_PTR)title;
        SetMenuItemInfoW(menu, index++, TRUE, &info);
    }
}
void measure_menu(MEASUREITEMSTRUCT &item, HWND owner, HFONT font) {
    auto title = (const wchar_t *)item.itemData;
    if (!title)
        return;
    auto dc = GetDC(owner);
    auto old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SIZE size{};
    GetTextExtentPoint32W(dc, title, (int)wcslen(title), &size);
    auto dpi = GetDpiForWindow(owner);
    item.itemWidth = size.cx + MulDiv(20, dpi, 96);
    item.itemHeight = MulDiv(26, dpi, 96);
    SelectObject(dc, old);
    ReleaseDC(owner, dc);
}
void draw_menu(const DRAWITEMSTRUCT &item, HFONT font, bool dark) {
    auto title = (const wchar_t *)item.itemData;
    if (!title)
        return;
    auto p = palette(dark);
    int saved = SaveDC(item.hDC);
    bool selected = (item.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    fill(item.hDC, item.rcItem, selected ? p.selected : p.background);
    text(item.hDC, font ? font : (HFONT)GetStockObject(DEFAULT_GUI_FONT), title, item.rcItem,
         selected ? p.accent : p.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(item.hDC, saved);
}
} // namespace keepmd::ui
