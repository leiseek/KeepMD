#include "ui.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace keepmd::ui {
void icon(HDC target, Icon value, RECT rect, COLORREF foreground, COLORREF background) {
    if (value == Icon::None)
        return;
    int size = std::min(rect.right - rect.left, rect.bottom - rect.top);
    if (size <= 0)
        return;
    // Supersample a tiny, temporary GDI surface. No icon font, image asset,
    // runtime dependency, persistent bitmap cache or animation timer is needed.
    int pixels = size * 3;
    auto dc = CreateCompatibleDC(target);
    if (!dc)
        return;
    auto bitmap = CreateCompatibleBitmap(target, pixels, pixels);
    if (!bitmap) {
        DeleteDC(dc);
        return;
    }
    auto oldBitmap = SelectObject(dc, bitmap);
    fill(dc, {0, 0, pixels, pixels}, background);
    float s = pixels / 24.f;
    auto n = [&](float v) { return (LONG)std::lround(v * s); };
    LOGBRUSH lb{BS_SOLID, foreground, 0};
    auto pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, std::max(1L, n(1.65f)),
                            &lb, 0, nullptr);
    auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    auto path = [&](std::initializer_list<POINT> points) {
        std::vector<POINT> p;
        for (auto pt : points)
            p.push_back({n((float)pt.x), n((float)pt.y)});
        Polyline(dc, p.data(), (int)p.size());
    };
    auto box = [&](float x, float y, float w, float h, float round = 0) {
        if (round)
            RoundRect(dc, n(x), n(y), n(x + w), n(y + h), n(round), n(round));
        else
            Rectangle(dc, n(x), n(y), n(x + w), n(y + h));
    };
    auto circle = [&](float x, float y, float r) { Ellipse(dc, n(x - r), n(y - r), n(x + r), n(y + r)); };
    auto bezier = [&](std::initializer_list<POINT> points) {
        std::vector<POINT> p;
        for (auto pt : points)
            p.push_back({n((float)pt.x), n((float)pt.y)});
        PolyBezier(dc, p.data(), (DWORD)p.size());
    };
    auto digit = [&](int d, int x, int y) {
        if (d == 1)
            path({{x, y + 1}, {x + 1, y}, {x + 1, y + 5}});
        if (d == 2) {
            path({{x, y + 1}, {x + 1, y}, {x + 3, y + 1}, {x + 3, y + 2}, {x, y + 5}, {x + 3, y + 5}});
        }
        if (d == 3) {
            path({{x, y}, {x + 3, y}, {x + 1, y + 2}, {x + 3, y + 3}, {x + 2, y + 5}, {x, y + 5}});
        }
    };
    switch (value) {
    case Icon::Folder:
        path({{3, 8}, {3, 5}, {9, 5}, {11, 7}, {20, 7}, {20, 9}});
        path({{3, 9}, {21, 9}, {18, 19}, {3, 19}, {3, 9}});
        break;
    case Icon::Back:
        path({{14, 5}, {7, 12}, {14, 19}});
        path({{7, 12}, {20, 12}});
        break;
    case Icon::Forward:
        path({{10, 5}, {17, 12}, {10, 19}});
        path({{4, 12}, {17, 12}});
        break;
    case Icon::Outline:
        box(3, 4, 18, 16, 2);
        path({{9, 4}, {9, 20}});
        path({{12, 8}, {18, 8}});
        path({{12, 12}, {18, 12}});
        path({{12, 16}, {16, 16}});
        break;
    case Icon::Search:
        circle(10, 10, 6.5f);
        path({{15, 15}, {21, 21}});
        break;
    case Icon::Minus:
        path({{5, 12}, {19, 12}});
        break;
    case Icon::Plus:
        path({{5, 12}, {19, 12}});
        path({{12, 5}, {12, 19}});
        break;
    case Icon::Sun:
        circle(12, 12, 4);
        for (int i = 0; i < 8; ++i) {
            float a = i * 3.14159265f / 4;
            POINT p[2] = {{n(12 + 7 * std::cos(a)), n(12 + 7 * std::sin(a))},
                          {n(12 + 10 * std::cos(a)), n(12 + 10 * std::sin(a))}};
            Polyline(dc, p, 2);
        }
        break;
    case Icon::Moon:
        bezier({{13, 3}, {3, 2}, {0, 18}, {11, 21}});
        bezier({{11, 21}, {16, 23}, {21, 18}, {21, 13}});
        bezier({{21, 13}, {14, 17}, {8, 10}, {13, 3}});
        break;
    case Icon::Width:
        path({{3, 4}, {3, 20}});
        path({{21, 4}, {21, 20}});
        path({{8, 8}, {4, 12}, {8, 16}});
        path({{16, 8}, {20, 12}, {16, 16}});
        path({{4, 12}, {20, 12}});
        break;
    case Icon::Edit:
        path({{4, 16}, {4, 21}, {9, 20}, {21, 8}, {16, 3}, {4, 16}});
        path({{14, 5}, {19, 10}});
        path({{4, 16}, {9, 20}});
        break;
    case Icon::Book:
        path({{12, 6},
              {8, 4},
              {3, 4},
              {3, 19},
              {8, 19},
              {12, 21},
              {16, 19},
              {21, 19},
              {21, 4},
              {16, 4},
              {12, 6},
              {12, 21}});
        break;
    case Icon::Save:
        path({{4, 3}, {17, 3}, {21, 7}, {21, 21}, {3, 21}, {3, 3}, {4, 3}});
        box(7, 3, 9, 6);
        box(7, 14, 10, 7);
        break;
    case Icon::Split:
        box(3, 4, 18, 16, 2);
        path({{12, 4}, {12, 20}});
        break;
    case Icon::Prompt:
        path({{4, 4},
              {20, 4},
              {21, 5},
              {21, 17},
              {20, 18},
              {10, 18},
              {5, 21},
              {5, 18},
              {3, 18},
              {3, 5},
              {4, 4}});
        path({{7, 8}, {11, 11}, {7, 14}});
        path({{14, 14}, {17, 14}});
        break;
    case Icon::Text:
        path({{4, 5}, {20, 5}});
        path({{12, 5}, {12, 20}});
        path({{8, 20}, {16, 20}});
        break;
    case Icon::H1:
    case Icon::H2:
        path({{3, 4}, {3, 20}});
        path({{13, 4}, {13, 20}});
        path({{3, 12}, {13, 12}});
        digit(value == Icon::H1 ? 1 : 2, 17, 14);
        break;
    case Icon::Bold:
        path({{6, 4}, {6, 20}});
        bezier({{6, 4}, {19, 2}, {21, 12}, {7, 12}});
        bezier({{7, 12}, {23, 10}, {20, 23}, {6, 20}});
        break;
    case Icon::Italic:
        path({{11, 4}, {20, 4}});
        path({{15, 4}, {9, 20}});
        path({{4, 20}, {13, 20}});
        break;
    case Icon::Bullet:
        for (int y : {6, 12, 18}) {
            circle(4, (float)y, .7f);
            path({{9, y}, {21, y}});
        }
        break;
    case Icon::Numbered:
        digit(1, 2, 2);
        digit(2, 2, 9);
        digit(3, 2, 16);
        for (int y : {5, 12, 19})
            path({{10, y}, {21, y}});
        break;
    case Icon::Quote:
        box(3, 5, 6, 7, 1);
        bezier({{9, 12}, {9, 16}, {7, 18}, {4, 19}});
        box(14, 5, 6, 7, 1);
        bezier({{20, 12}, {20, 16}, {18, 18}, {15, 19}});
        break;
    case Icon::Code:
        path({{7, 6}, {2, 12}, {7, 18}});
        path({{17, 6}, {22, 12}, {17, 18}});
        path({{14, 3}, {10, 21}});
        break;
    case Icon::Diagram:
        box(8, 3, 8, 5, 1);
        box(2, 16, 7, 5, 1);
        box(15, 16, 7, 5, 1);
        path({{12, 8}, {12, 12}, {5, 12}, {5, 16}});
        path({{12, 12}, {18, 12}, {18, 16}});
        break;
    case Icon::Keyboard:
        box(2, 5, 20, 14, 2);
        path({{7, 16}, {17, 16}});
        for (int y : {9, 12})
            for (int x : {6, 10, 14, 18})
                path({{x, y}, {x + 1, y}});
        break;
    case Icon::Trash:
        path({{3, 6}, {21, 6}});
        path({{8, 6}, {8, 3}, {16, 3}, {16, 6}});
        path({{5, 6}, {6, 21}, {18, 21}, {19, 6}});
        path({{10, 10}, {10, 17}});
        path({{14, 10}, {14, 17}});
        break;
    case Icon::Copy:
    case Icon::CopyHide:
        box(8, 8, 12, 13, 2);
        path({{15, 5}, {15, 3}, {3, 3}, {3, 16}, {5, 16}});
        if (value == Icon::CopyHide)
            path({{11, 14}, {14, 17}, {18, 12}});
        break;
    case Icon::Check:
        path({{4, 12}, {10, 18}, {21, 6}});
        break;
    case Icon::Reset:
        bezier({{5, 8}, {11, -1}, {24, 7}, {19, 17}});
        bezier({{19, 17}, {15, 23}, {6, 21}, {4, 16}});
        path({{4, 3}, {4, 9}, {10, 9}});
        break;
    default:
        break;
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    int saved = SaveDC(target);
    SetStretchBltMode(target, HALFTONE);
    SetBrushOrgEx(target, 0, 0, nullptr);
    StretchBlt(target, rect.left + (rect.right - rect.left - size) / 2,
               rect.top + (rect.bottom - rect.top - size) / 2, size, size, dc, 0, 0, pixels, pixels, SRCCOPY);
    RestoreDC(target, saved);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}
void icon_face(HDC dc, RECT rect, HFONT font, Icon value, std::wstring_view label, const Palette &p,
               bool primary, bool checked, bool hot, bool pressed, bool disabled, bool focused, float scale) {
    auto bg = primary && !disabled ? p.accent
              : checked            ? p.selected
              : pressed || hot     ? p.hover
                                   : p.background;
    auto fg = disabled ? p.muted : primary ? p.accentText : checked ? p.accent : p.text;
    if (primary || checked || hot || pressed)
        rounded(dc, rect, bg, bg, (int)(10 * scale));
    else
        fill(dc, rect, bg);
    int size = (int)(20 * scale), offset = pressed ? 1 : 0;
    int x = rect.left + (rect.right - rect.left - size) / 2;
    if (!label.empty() && value != Icon::None) {
        SIZE extent{};
        auto old = SelectObject(dc, font);
        GetTextExtentPoint32W(dc, label.data(), (int)label.size(), &extent);
        SelectObject(dc, old);
        x = rect.left + (rect.right - rect.left - size - (int)(9 * scale) - extent.cx) / 2;
    }
    int y = rect.top + (rect.bottom - rect.top - size) / 2 + offset;
    icon(dc, value, {x, y, x + size, y + size}, fg, bg);
    if (!label.empty()) {
        RECT textRect = rect;
        if (value != Icon::None)
            textRect.left = x + size + (int)(9 * scale);
        OffsetRect(&textRect, 0, offset);
        text(dc, font, label, textRect, fg,
             DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                 (value == Icon::None ? DT_CENTER : DT_LEFT));
    }
    if (focused) {
        RECT focus = rect;
        InflateRect(&focus, -3, -3);
        SetTextColor(dc, fg);
        SetBkColor(dc, bg);
        DrawFocusRect(dc, &focus);
    }
}
} // namespace keepmd::ui
