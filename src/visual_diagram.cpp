#include "visual_diagram.h"
#include "diagram.h"
#include "document.h"
#include "ui.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace keepmd {
std::string visual_diagram_rtf(const std::wstring &source, int maxWidth, bool dark) {
    auto parsed = parse_diagram(utf8(source));
    auto colors = ui::palette(dark);
    auto measure = CreateCompatibleDC(nullptr);
    auto font = ui::font(96, 15);
    auto oldFont = SelectObject(measure, font);
    std::vector<mermaid::Size> sizes;
    for (const auto &node : parsed.graph.nodes) {
        auto label = wide(node.label);
        SIZE s{};
        GetTextExtentPoint32W(measure, label.c_str(), (int)label.size(), &s);
        float width = (float)std::clamp((int)s.cx + 36, 76, 320), height = 54;
        if (node.shape == mermaid::NodeShape::Diamond) {
            width *= 1.35f;
            height *= 1.3f;
        }
        if (node.shape == mermaid::NodeShape::Circle)
            width = height = std::max(width, height);
        sizes.push_back({width, height});
    }
    auto layout = mermaid::layout(parsed.graph, sizes, 40, 64);
    float logicalWidth = std::max(240.f, layout.width + 80),
          logicalHeight = std::max(80.f, layout.height + 80);
    if (!parsed.ok) {
        logicalWidth = 500;
        logicalHeight = 110;
    }
    float scale = std::min({1.f, maxWidth / logicalWidth, 600.f / logicalHeight});
    int width = std::max(1, (int)std::ceil(logicalWidth * scale)),
        height = std::max(1, (int)std::ceil(logicalHeight * scale));
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void *pixels = nullptr;
    auto bitmap = CreateDIBSection(measure, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) {
        SelectObject(measure, oldFont);
        DeleteObject(font);
        DeleteDC(measure);
        return {};
    }
    auto previous = SelectObject(measure, bitmap);
    ui::fill(measure, {0, 0, width, height}, dark ? RGB(24, 30, 40) : RGB(252, 252, 251));
    SetGraphicsMode(measure, GM_ADVANCED);
    XFORM transform{scale, 0, 0, scale, 0, 0};
    SetWorldTransform(measure, &transform);
    if (!parsed.ok) {
        ui::text(measure, font, L"流程图", {16, 10, 480, 38}, colors.text);
        ui::text(measure, font, parsed.error, {16, 40, 480, 100}, colors.muted,
                 DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    } else {
        for (auto &r : layout.nodes) {
            r.left += 40;
            r.right += 40;
            r.top += 40;
            r.bottom += 40;
        }
        bool vertical = parsed.graph.direction == mermaid::Direction::TopToBottom ||
                        parsed.graph.direction == mermaid::Direction::BottomToTop;
        bool reverse = parsed.graph.direction == mermaid::Direction::BottomToTop ||
                       parsed.graph.direction == mermaid::Direction::RightToLeft;
        for (const auto &group : parsed.graph.subgraphs) {
            RECT bounds{100000, 100000, -100000, -100000};
            for (auto n : group.nodes)
                if (n < layout.nodes.size()) {
                    auto r = layout.nodes[n];
                    bounds.left = std::min(bounds.left, (LONG)r.left - 12);
                    bounds.top = std::min(bounds.top, (LONG)r.top - 28);
                    bounds.right = std::max(bounds.right, (LONG)r.right + 12);
                    bounds.bottom = std::max(bounds.bottom, (LONG)r.bottom + 12);
                }
            if (bounds.left < bounds.right) {
                ui::rounded(measure, bounds, colors.surface, colors.border, 8);
                auto label = bounds;
                label.left += 8;
                label.bottom = label.top + 25;
                ui::text(measure, font, wide(group.label), label, colors.muted);
            }
        }
        for (const auto &edge : parsed.graph.edges) {
            auto a = layout.nodes[edge.from], b = layout.nodes[edge.to];
            POINT from = vertical ? POINT{(LONG)((a.left + a.right) / 2), (LONG)(reverse ? a.top : a.bottom)}
                                  : POINT{(LONG)(reverse ? a.left : a.right), (LONG)((a.top + a.bottom) / 2)};
            POINT to = vertical ? POINT{(LONG)((b.left + b.right) / 2), (LONG)(reverse ? b.bottom : b.top)}
                                : POINT{(LONG)(reverse ? b.right : b.left), (LONG)((b.top + b.bottom) / 2)};
            auto pen = CreatePen(edge.dashed ? PS_DASH : PS_SOLID,
                                 std::max(1, (int)(edge.strokeScale * 1.5f)), colors.muted);
            auto old = SelectObject(measure, pen);
            POINT points[4];
            points[0] = from;
            points[3] = to;
            if (vertical) {
                LONG mid = (from.y + to.y) / 2;
                points[1] = {from.x, mid};
                points[2] = {to.x, mid};
            } else {
                LONG mid = (from.x + to.x) / 2;
                points[1] = {mid, from.y};
                points[2] = {mid, to.y};
            }
            if (edge.from == edge.to || layout.ranks[edge.to] <= layout.ranks[edge.from]) {
                if (vertical) {
                    points[0] = {(LONG)a.right, (LONG)((a.top + a.bottom) / 2) - 5};
                    points[3] = {(LONG)b.right, (LONG)((b.top + b.bottom) / 2) + 5};
                    LONG side = (LONG)logicalWidth - 8;
                    points[1] = {side, points[0].y};
                    points[2] = {side, points[3].y};
                } else {
                    points[0] = {(LONG)((a.left + a.right) / 2) - 5, (LONG)a.bottom};
                    points[3] = {(LONG)((b.left + b.right) / 2) + 5, (LONG)b.bottom};
                    LONG side = (LONG)logicalHeight - 8;
                    points[1] = {points[0].x, side};
                    points[2] = {points[3].x, side};
                }
            }
            Polyline(measure, points, 4);
            if (edge.directed) {
                auto tip = points[3];
                double dx = tip.x - points[2].x, dy = tip.y - points[2].y, len = std::hypot(dx, dy);
                if (len) {
                    dx /= len;
                    dy /= len;
                    POINT arrow[3] = {{(LONG)(tip.x - dx * 8 + dy * 4), (LONG)(tip.y - dy * 8 - dx * 4)},
                                      tip,
                                      {(LONG)(tip.x - dx * 8 - dy * 4), (LONG)(tip.y - dy * 8 + dx * 4)}};
                    Polyline(measure, arrow, 3);
                }
            }
            SelectObject(measure, old);
            DeleteObject(pen);
            if (!edge.label.empty()) {
                LONG x = (points[1].x + points[2].x) / 2, y = (points[1].y + points[2].y) / 2;
                RECT rect{x - 65, y - 12, x + 65, y + 12};
                ui::fill(measure, rect, colors.background);
                ui::text(measure, font, wide(edge.label), rect, colors.text,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        auto color = [](uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); };
        for (size_t i = 0; i < parsed.graph.nodes.size(); ++i) {
            auto &n = parsed.graph.nodes[i];
            auto r = layout.nodes[i];
            RECT box{(LONG)r.left, (LONG)r.top, (LONG)r.right, (LONG)r.bottom};
            auto fill = n.style.hasFill ? color(n.style.fill.rgb) : colors.selected;
            auto border = n.style.hasStroke ? color(n.style.stroke.rgb) : colors.accent;
            auto pen = CreatePen(PS_SOLID, 1, border);
            auto brush = CreateSolidBrush(fill);
            auto op = SelectObject(measure, pen), ob = SelectObject(measure, brush);
            if (n.shape == mermaid::NodeShape::Diamond) {
                POINT p[4] = {{(box.left + box.right) / 2, box.top},
                              {box.right, (box.top + box.bottom) / 2},
                              {(box.left + box.right) / 2, box.bottom},
                              {box.left, (box.top + box.bottom) / 2}};
                Polygon(measure, p, 4);
            } else if (n.shape == mermaid::NodeShape::Hexagon) {
                int inset = (box.right - box.left) / 6;
                POINT p[6] = {{box.left + inset, box.top},
                              {box.right - inset, box.top},
                              {box.right, (box.top + box.bottom) / 2},
                              {box.right - inset, box.bottom},
                              {box.left + inset, box.bottom},
                              {box.left, (box.top + box.bottom) / 2}};
                Polygon(measure, p, 6);
            } else if (n.shape == mermaid::NodeShape::Circle)
                Ellipse(measure, box.left, box.top, box.right, box.bottom);
            else {
                int rad = n.shape == mermaid::NodeShape::Rectangle ? 0
                          : n.shape == mermaid::NodeShape::Stadium ? box.bottom - box.top
                                                                   : 14;
                RoundRect(measure, box.left, box.top, box.right, box.bottom, rad, rad);
            }
            SelectObject(measure, op);
            SelectObject(measure, ob);
            DeleteObject(pen);
            DeleteObject(brush);
            InflateRect(&box, -10, -4);
            ui::text(measure, font, wide(n.label), box,
                     n.style.hasText ? color(n.style.text.rgb) : colors.text,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    GdiFlush();
    // RTF DIB pictures carry a bottom-up BITMAPINFOHEADER and opaque BGR pixels.
    auto header = info.bmiHeader;
    header.biHeight = height;
    header.biSizeImage = width * height * 4;
    std::vector<unsigned char> data(sizeof(header) + (size_t)width * height * 4);
    memcpy(data.data(), &header, sizeof(header));
    for (int y = 0; y < height; ++y)
        memcpy(data.data() + sizeof(header) + (size_t)y * width * 4,
               (BYTE *)pixels + (size_t)(height - y - 1) * width * 4, (size_t)width * 4);
    std::string out = "{\\pict\\dibitmap0\\picw" + std::to_string(width) + "\\pich" + std::to_string(height) +
                      "\\picwgoal" + std::to_string(width * 15) + "\\pichgoal" + std::to_string(height * 15) +
                      " ";
    const char digits[] = "0123456789abcdef";
    out.reserve(out.size() + data.size() * 2 + 2);
    for (auto byte : data) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    out += '}';
    SelectObject(measure, previous);
    SelectObject(measure, oldFont);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(measure);
    return out;
}
} // namespace keepmd
