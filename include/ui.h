#pragma once
#include <initializer_list>
#include <string_view>
#include <windows.h>

namespace keepmd::ui {
enum class Icon {
    None,
    Folder,
    Back,
    Forward,
    Outline,
    Search,
    Minus,
    Plus,
    Moon,
    Sun,
    Width,
    Edit,
    Book,
    Save,
    Split,
    Prompt,
    Text,
    H1,
    H2,
    Bold,
    Italic,
    Bullet,
    Numbered,
    Quote,
    Code,
    Diagram,
    Keyboard,
    Trash,
    Copy,
    CopyHide,
    Check,
    Reset
};
void icon(HDC dc, Icon value, RECT rect, COLORREF foreground, COLORREF background);
void icon_face(HDC dc, RECT rect, HFONT font, Icon value, std::wstring_view label, const struct Palette &p,
               bool primary, bool checked, bool hot, bool pressed, bool disabled, bool focused, float scale);
void icon_button(HWND hwnd, Icon value, const wchar_t *tip, bool showLabel = false);
void set_button_icon(HWND hwnd, Icon value);
constexpr UINT WM_SCROLL_TO = WM_APP + 40;
constexpr UINT WM_TOGGLE_TOOLBAR = WM_APP + 41;
enum class ScrollKind { Reader, Editor, List };
void attach_scrollbars(HWND target, ScrollKind kind);
void sync_scrollbars(HWND target);
void scroll_theme(HWND target, bool dark);
// Small, event-driven Win32 chrome. Controls retain native keyboard/accessibility behavior.
struct Palette {
    COLORREF background, surface, text, muted, border, hover, accent, accentText, selected;
};
Palette palette(bool dark);
void fill(HDC dc, RECT rect, COLORREF color);
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color);
void rounded(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius);
void text(HDC dc, HFONT font, std::wstring_view value, RECT rect, COLORREF color,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
HFONT font(unsigned dpi, int size = 15, int weight = FW_NORMAL);
void style_button(HWND hwnd, bool primary = false, bool checked = false);
void draw_button(const DRAWITEMSTRUCT &item, bool dark);
void menu_labels(HMENU menu, std::initializer_list<const wchar_t *> labels);
void measure_menu(MEASUREITEMSTRUCT &item, HWND owner, HFONT font);
void draw_menu(const DRAWITEMSTRUCT &item, HFONT font, bool dark);
void titlebar(HWND hwnd, bool dark);
// Custom caption owns a detached menu and forwards its original commands.
int caption_height(HWND hwnd);
void caption_toolbar(HWND hwnd, bool expanded);
void caption_menu(HWND hwnd, HMENU menu);
HMENU window_menu(HWND hwnd);
bool caption_translate(MSG &message);
void button_face(HDC dc, RECT rect, HFONT font, std::wstring_view title, const Palette &p, bool primary,
                 bool checked, bool hot, bool pressed, bool disabled, bool focused, float scale);
} // namespace keepmd::ui
