#include "view.h"
#include "ui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <objidl.h>

namespace keepmd {
namespace {
float clampf(float v, float lo, float hi) {
    return std::clamp(v, lo, std::max(lo, hi));
}
D2D1_COLOR_F rgb(unsigned c, float a = 1) {
    return D2D1::ColorF(c, a);
}
struct CoScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~CoScope() {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};
size_t block_hash(const Block &block) {
    size_t hash = std::hash<std::wstring>{}(block.text.value) ^ (size_t(block.kind) << 8);
    for (const auto &cell : block.cells)
        hash ^= std::hash<std::wstring>{}(cell.value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
}
bool equal_text(const Text &a, const Text &b) {
    if (a.value != b.value || a.spans.size() != b.spans.size())
        return false;
    for (size_t i = 0; i < a.spans.size(); ++i) {
        const auto &x = a.spans[i];
        const auto &y = b.spans[i];
        if (x.start != y.start || x.length != y.length || x.style != y.style || x.link != y.link)
            return false;
    }
    return true;
}
bool equal_block(const Block &a, const Block &b) {
    if (a.kind != b.kind || a.level != b.level || a.indent != b.indent || a.quote != b.quote ||
        a.header != b.header || a.joins_next != b.joins_next || a.target != b.target ||
        a.marker != b.marker || a.align != b.align || a.cells.size() != b.cells.size() ||
        !equal_text(a.text, b.text))
        return false;
    for (size_t i = 0; i < a.cells.size(); ++i)
        if (!equal_text(a.cells[i], b.cells[i]))
            return false;
    return true;
}
std::wstring decode_url_path(const std::wstring &input) {
    std::string value = utf8(input), output;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() && hex(value[i + 1]) >= 0 && hex(value[i + 2]) >= 0) {
            output += (char)(hex(value[i + 1]) * 16 + hex(value[i + 2]));
            i += 2;
        } else
            output += value[i];
    }
    return wide(output);
}
} // namespace
View::View(HWND hwnd) : hwnd_(hwnd) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown **>(dw_.GetAddressOf()));
    if (d2d_) {
        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties();
        properties.dashStyle = D2D1_DASH_STYLE_DASH;
        d2d_->CreateStrokeStyle(properties, nullptr, 0, dashed_.GetAddressOf());
    }
    resize();
}
View::~View() {
    worker_.reset();
}
void View::invalidate() {
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void View::reset_device() {
    brush_.Reset();
    target_.Reset();
    ++generation_;
    if (worker_)
        worker_->clear();
    assets_.clear();
    assetBytes_ = 0;
    reset_layout(true);
    invalidate();
}
D2D1_COLOR_F View::foreground() const {
    return rgb(dark_ ? 0xe1e5ec : 0x243044);
}
D2D1_COLOR_F View::background() const {
    return rgb(dark_ ? 0x171c25 : 0xfdfdfc);
}
float View::content_width() const {
    return std::max(180.f, std::min(width_ - 64.f, readingWidth_ * zoom_));
}
float View::left_margin() const {
    return std::max(24.f, (width_ - content_width()) * .5f) - scrollX_;
}
float View::font_size(const Block &b) const {
    if (b.kind == Kind::Heading) {
        static const float sizes[] = {32, 27, 23, 21, 19, 18};
        return sizes[std::clamp(b.level, 1u, 6u) - 1] * zoom_;
    }
    return (b.kind == Kind::Code || b.kind == Kind::Html ? 15.f : 17.f) * zoom_;
}
void View::set_document(std::shared_ptr<Document> doc, std::filesystem::path path) {
    auto previousDoc = doc_;
    auto previousCache = std::move(cache_);
    auto previousAssets = std::move(assets_);
    auto expanded = std::move(expandedDiagrams_);
    bool sameFile = path_.lexically_normal() == path.lexically_normal();
    ++generation_;
    if (worker_)
        worker_->clear();
    {
        std::lock_guard lock(resultsMutex_);
        results_.clear();
    }
    doc_ = std::move(doc);
    path_ = std::move(path);
    assets_.clear();
    expandedDiagrams_.clear();
    assetBytes_ = 0;
    selectFrom_ = selectTo_ = 0;
    scrollX_ = scrollY_ = 0;
    matches_.clear();
    reset_layout(false);
    reusedLayouts_ = reusedDiagrams_ = 0;
    if (previousDoc && doc_ && sameFile && (!previousCache.empty() || !previousAssets.empty())) {
        std::unordered_map<size_t, std::vector<size_t>> candidates;
        for (const auto &[index, entry] : previousCache) {
            (void)entry;
            if (index < previousDoc->blocks.size())
                candidates[block_hash(previousDoc->blocks[index])].push_back(index);
        }
        for (const auto &[index, asset] : previousAssets) {
            if (index < previousDoc->blocks.size() && asset.scene && !previousCache.contains(index))
                candidates[block_hash(previousDoc->blocks[index])].push_back(index);
        }
        for (size_t i = 0; i < doc_->blocks.size(); ++i) {
            const auto &block = doc_->blocks[i];
            auto group = candidates.find(block_hash(block));
            if (group == candidates.end())
                continue;
            for (auto candidate = group->second.begin(); candidate != group->second.end(); ++candidate) {
                size_t old = *candidate;
                if (!equal_block(block, previousDoc->blocks[old]))
                    continue;
                bool reusable = block.kind != Kind::Image && block.kind != Kind::Diagram;
                auto asset = previousAssets.find(old);
                if (block.kind == Kind::Diagram && asset != previousAssets.end() && asset->second.scene) {
                    assetBytes_ += asset->second.bytes;
                    assets_.emplace(i, std::move(asset->second));
                    ++reusedDiagrams_;
                    reusable = true;
                    if (expanded.contains(old))
                        expandedDiagrams_.insert(i);
                }
                auto entry = previousCache.find(old);
                if (reusable && entry != previousCache.end()) {
                    cacheChars_ += entry->second.chars;
                    heights_.set(i, entry->second.height);
                    widest_ = std::max(widest_,
                                       entry->second.width + (block.indent * 22 + block.quote * 14) * zoom_);
                    cache_.emplace(i, std::move(entry->second));
                    ++reusedLayouts_;
                }
                group->second.erase(candidate);
                break;
            }
        }
    }
    invalidate();
}
void View::reset_layout(bool preserveAnchor) {
    size_t anchor = heights_.locate(scrollY_);
    float local = scrollY_ - heights_.prefix(anchor);
    cache_.clear();
    cacheChars_ = 0;
    widest_ = content_width();
    std::vector<float> values;
    if (doc_) {
        values.reserve(doc_->blocks.size());
        for (const auto &b : doc_->blocks) {
            float fs = font_size(b),
                  usable = std::max(120.f, content_width() - (b.indent * 22 + b.quote * 14) * zoom_);
            float lines = std::max(1.f, std::ceil((float)b.text.value.size() * fs * .55f / usable));
            lines += (float)std::count(b.text.value.begin(), b.text.value.end(), L'\n');
            float h = fs * 1.6f * lines + 12 * zoom_;
            if (b.kind == Kind::Code || b.kind == Kind::Html)
                h = fs * 1.6f;
            if (b.kind == Kind::Rule)
                h = 26 * zoom_;
            if (b.kind == Kind::Image || b.kind == Kind::Diagram)
                h = 220 * zoom_;
            if (b.kind == Kind::TableRow)
                h = 44 * zoom_;
            values.push_back(h);
        }
    }
    heights_.reset(values);
    scrollY_ = preserveAnchor && anchor < values.size() ? heights_.prefix(anchor) + local : 0;
    update_scrollbars();
}
void View::resize() {
    RECT r{};
    GetClientRect(hwnd_, &r);
    dpi_ = (float)(dpiOverride_ ? dpiOverride_ : GetDpiForWindow(hwnd_)) / 96.f;
    if (dpi_ <= 0)
        dpi_ = 1;
    float oldWidth = width_;
    width_ = (r.right - r.left) / dpi_;
    height_ = (r.bottom - r.top) / dpi_;
    if (target_) {
        target_->SetDpi(96 * dpi_, 96 * dpi_);
        target_->Resize(D2D1::SizeU(std::max(1L, r.right), std::max(1L, r.bottom)));
    }
    if (std::abs(width_ - oldWidth) > .1f)
        reset_layout(true);
    update_scrollbars();
    invalidate();
}
void View::set_zoom(float factor) {
    factor = clampf(factor, .65f, 2.5f);
    if (factor == zoom_)
        return;
    float old = zoom_;
    zoom_ = factor;
    size_t anchor = heights_.locate(scrollY_);
    float local = scrollY_ - heights_.prefix(anchor);
    ++generation_;
    if (worker_)
        worker_->clear();
    assets_.clear();
    assetBytes_ = 0;
    reset_layout(false);
    scrollY_ = heights_.prefix(anchor) + local * factor / old;
    update_scrollbars();
    invalidate();
}
void View::set_dark(bool value) {
    dark_ = value;
    ui::scroll_theme(hwnd_, value);
    invalidate();
}
void View::set_reading_width(float value) {
    readingWidth_ = value;
    reset_layout(true);
    invalidate();
}
void View::update_scrollbars() {
    scrollY_ = clampf(scrollY_, 0, heights_.total() + 40 - height_);
    scrollX_ = clampf(scrollX_, 0, widest_ + 64 - width_);
    // Reserve scrollbar space while estimated heights are refined. Hiding a bar
    // changes client width and used to invalidate the layout in an endless loop
    // for a large diagram whose initial placeholder fit on one screen.
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    si.nMin = 0;
    si.nMax = (int)std::min(heights_.total() + 40, (float)INT_MAX - 1);
    si.nPage = (UINT)std::max(1.f, height_);
    si.nPos = (int)scrollY_;
    SetScrollInfo(hwnd_, SB_VERT, &si, FALSE);
    si.nMax = (int)std::min(std::max(width_, widest_ + 64) - 1, (float)INT_MAX - 1);
    si.nPage = (UINT)std::max(1.f, width_);
    si.nPos = (int)scrollX_;
    SetScrollInfo(hwnd_, SB_HORZ, &si, FALSE);
    ui::sync_scrollbars(hwnd_);
}
void View::scroll(float dy) {
    scroll_to(scrollY_ + dy);
}
void View::scroll_to(float y) {
    if (inputStarted_ == std::chrono::steady_clock::time_point{})
        inputStarted_ = std::chrono::steady_clock::now();
    scrollY_ = y;
    update_scrollbars();
    invalidate();
}
void View::hscroll(float dx) {
    if (inputStarted_ == std::chrono::steady_clock::time_point{})
        inputStarted_ = std::chrono::steady_clock::now();
    scrollX_ += dx;
    update_scrollbars();
    invalidate();
}
void View::goto_block(size_t block) {
    if (!doc_ || block >= doc_->blocks.size())
        return;
    scrollY_ = heights_.prefix(block);
    update_scrollbars();
    invalidate();
}
void View::ensure_target() {
    if (target_ || !d2d_)
        return;
    RECT r{};
    GetClientRect(hwnd_, &r);
    wchar_t backend[32]{};
    GetEnvironmentVariableW(L"KEEPMD_RENDERER", backend, 32);
    auto props =
        D2D1::RenderTargetProperties(wcscmp(backend, L"hardware") == 0 ? D2D1_RENDER_TARGET_TYPE_DEFAULT
                                                                       : D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    props.dpiX = props.dpiY = 96 * dpi_;
    auto hr = d2d_->CreateHwndRenderTarget(
        props,
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(std::max(1L, r.right), std::max(1L, r.bottom))),
        target_.GetAddressOf());
    if (FAILED(hr)) {
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        d2d_->CreateHwndRenderTarget(props,
                                     D2D1::HwndRenderTargetProperties(
                                         hwnd_, D2D1::SizeU(std::max(1L, r.right), std::max(1L, r.bottom))),
                                     target_.GetAddressOf());
    }
    if (target_)
        target_->CreateSolidColorBrush(foreground(), brush_.ReleaseAndGetAddressOf());
}
View::Entry &View::layout(size_t index) {
    auto found = cache_.find(index);
    if (found != cache_.end()) {
        found->second.use = ++clock_;
        return found->second;
    }
    Entry entry;
    entry.use = ++clock_;
    const auto &b = doc_->blocks[index];
    const float fs = font_size(b);
    const float indent = (b.indent * 22 + b.quote * 14) * zoom_;
    float usable = std::max(100.f, content_width() - indent);
    entry.width = usable;
    if ((b.kind == Kind::Image || b.kind == Kind::Diagram) && !assets_.contains(index))
        request_asset(index);
    auto asset = assets_.find(index);
    if (asset != assets_.end() && asset->second.status == Asset::Ready) {
        asset->second.use = ++clock_;
        float w = asset->second.scene ? asset->second.scene->width : (float)asset->second.width;
        float h = asset->second.scene ? asset->second.scene->height : (float)asset->second.height;
        if (asset->second.scene && expandedDiagrams_.contains(index))
            entry.width = std::max(usable, w);
        float scale = std::min(1.f, entry.width / std::max(1.f, w));
        entry.height = h * scale + 30 * zoom_;
    } else if (b.kind == Kind::Rule)
        entry.height = 26 * zoom_;
    else {
        std::vector<const Text *> texts;
        Text placeholder;
        bool code = b.kind == Kind::Code || b.kind == Kind::Html;
        if (b.kind == Kind::Image) {
            placeholder.value = b.text.value.empty() ? L"图片" : b.text.value;
            placeholder.value += asset != assets_.end() && asset->second.status == Asset::Failed
                                     ? L"\n" + asset->second.error
                                     : L" · 加载中…";
            texts.push_back(&placeholder);
        } else if (b.kind == Kind::Diagram) {
            if (asset != assets_.end() && asset->second.status == Asset::Failed)
                placeholder.value = asset->second.error + L"\n" + b.text.value;
            else
                placeholder.value = L"流程图 · 正在排版…";
            texts.push_back(&placeholder);
        } else if (b.kind == Kind::TableRow) {
            entry.width = std::max(usable, b.cells.size() * 110.f * zoom_);
            for (const auto &cell : b.cells)
                texts.push_back(&cell);
        } else
            texts.push_back(&b.text);
        float offset = 0;
        uint32_t textOffset = 0;
        float rowHeight = 0;
        float cellWidth = b.kind == Kind::TableRow ? entry.width / std::max(size_t(1), texts.size()) : usable;
        for (size_t c = 0; c < texts.size(); ++c) {
            const auto &text = *texts[c];
            ComPtr<IDWriteTextFormat> format;
            dw_->CreateTextFormat(code ? L"Consolas" : L"Segoe UI", nullptr,
                                  b.kind == Kind::Heading || b.header ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                                                      : DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs, L"zh-CN",
                                  format.GetAddressOf());
            if (!format)
                continue;
            format->SetWordWrapping(code ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
            format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, fs * 1.6f, fs * 1.23f);
            if (b.kind == Kind::TableRow && c < b.align.size())
                format->SetTextAlignment(b.align[c] == 2   ? DWRITE_TEXT_ALIGNMENT_CENTER
                                         : b.align[c] == 3 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                           : DWRITE_TEXT_ALIGNMENT_LEADING);
            ComPtr<IDWriteTextLayout> tl;
            dw_->CreateTextLayout(
                text.value.data(), (UINT32)text.value.size(), format.Get(),
                code ? 1000000.f : std::max(40.f, cellWidth - (b.kind == Kind::TableRow ? 20 * zoom_ : 0)),
                10000000.f, tl.GetAddressOf());
            if (!tl)
                continue;
            for (const auto &span : text.spans) {
                DWRITE_TEXT_RANGE range{span.start, span.length};
                if (span.style & Bold)
                    tl->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
                if (span.style & Italic)
                    tl->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
                if (span.style & Strike)
                    tl->SetStrikethrough(TRUE, range);
                if (span.style & Mono) {
                    tl->SetFontFamilyName(L"Consolas", range);
                    tl->SetFontSize(fs * .94f, range);
                }
                if (!span.link.empty())
                    tl->SetUnderline(TRUE, range);
            }
            DWRITE_TEXT_METRICS metrics{};
            tl->GetMetrics(&metrics);
            rowHeight = std::max(rowHeight, metrics.height);
            if (code)
                entry.width = std::max(usable, metrics.widthIncludingTrailingWhitespace + 16 * zoom_);
            entry.offsets.push_back(offset);
            entry.textOffsets.push_back(textOffset);
            entry.chars += text.value.size();
            entry.layouts.push_back(std::move(tl));
            offset += cellWidth;
            textOffset += (uint32_t)text.value.size() + 1;
        }
        entry.height = rowHeight + (code || b.joins_next       ? 0
                                    : b.kind == Kind::Heading  ? 22
                                    : b.kind == Kind::TableRow ? 16
                                                               : 14) *
                                       zoom_;
        entry.height = std::max(entry.height, 18 * zoom_);
    }
    float previous = heights_.at(index);
    size_t anchor = heights_.locate(scrollY_);
    heights_.set(index, entry.height);
    if (index < anchor)
        scrollY_ += entry.height - previous;
    widest_ = std::max(widest_, entry.width + indent);
    cacheChars_ += entry.chars;
    return cache_.emplace(index, std::move(entry)).first->second;
}
void View::trim(size_t first, size_t last) {
    while (cache_.size() > 128 || cacheChars_ > 500000) {
        auto victim = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->first >= first && it->first <= last)
                continue;
            if (victim == cache_.end() || it->second.use < victim->second.use)
                victim = it;
        }
        if (victim == cache_.end())
            break;
        cacheChars_ -= victim->second.chars;
        cache_.erase(victim);
    }
    while (assets_.size() > 48 || assetBytes_ > 24 * 1024 * 1024) {
        auto victim = assets_.end();
        for (auto it = assets_.begin(); it != assets_.end(); ++it) {
            if ((it->first >= first && it->first <= last) || it->second.status == Asset::Pending)
                continue;
            if (victim == assets_.end() || it->second.use < victim->second.use)
                victim = it;
        }
        if (victim == assets_.end())
            break;
        assetBytes_ -= victim->second.bytes;
        auto cached = cache_.find(victim->first);
        if (cached != cache_.end()) {
            cacheChars_ -= cached->second.chars;
            cache_.erase(cached);
        }
        assets_.erase(victim);
    }
}
void View::fill(ID2D1RenderTarget *target, D2D1_RECT_F rect, D2D1_COLOR_F color) {
    brush_->SetColor(color);
    target->FillRectangle(rect, brush_.Get());
}
void View::draw_text(ID2D1RenderTarget *target, const std::wstring &text, D2D1_RECT_F rect, float size,
                     D2D1_COLOR_F color, bool bold) {
    ComPtr<IDWriteTextFormat> format;
    dw_->CreateTextFormat(
        L"Segoe UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.GetAddressOf());
    if (!format)
        return;
    brush_->SetColor(color);
    target->DrawText(text.data(), (UINT32)text.size(), format.Get(), rect, brush_.Get());
}
void View::range_rects(ID2D1RenderTarget *target, IDWriteTextLayout *text, uint32_t start, uint32_t length,
                       float x, float y, D2D1_COLOR_F color) {
    if (!length)
        return;
    UINT32 count = 0;
    text->HitTestTextRange(start, length, x, y, nullptr, 0, &count);
    if (count > 10000)
        return;
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (SUCCEEDED(text->HitTestTextRange(start, length, x, y, metrics.data(), count, &count)))
        for (const auto &m : metrics)
            fill(target, D2D1::RectF(m.left, m.top, m.left + m.width, m.top + m.height), color);
}
void View::draw_block(ID2D1RenderTarget *target, size_t index, Entry &entry, float y) {
    const auto &b = doc_->blocks[index];
    float x = left_margin() + (b.indent * 22 + b.quote * 14) * zoom_;
    if ((b.kind == Kind::Image || b.kind == Kind::Diagram) && !assets_.contains(index))
        request_asset(index);
    bool code = b.kind == Kind::Code || b.kind == Kind::Html;
    auto box = D2D1::RectF(x - 8 * zoom_, y, x + entry.width + 8 * zoom_, y + entry.height);
    if (code || b.kind == Kind::TableRow)
        fill(target, box, rgb(dark_ ? b.header ? 0x303c50 : 0x202937 : b.header ? 0xe9eef5 : 0xf2f5f8));
    if (b.kind == Kind::Rule) {
        fill(target, D2D1::RectF(x, y + 12 * zoom_, x + entry.width, y + 13 * zoom_),
             rgb(dark_ ? 0x3b4658 : 0xdce2e9));
        return;
    }
    if (b.quote)
        fill(target, D2D1::RectF(x - 14 * zoom_, y, x - 11 * zoom_, y + entry.height - 10 * zoom_),
             rgb(0x7d98b3));
    if (!b.marker.empty())
        draw_text(target, b.marker, D2D1::RectF(x - 23 * zoom_, y, x - 2 * zoom_, y + 30 * zoom_),
                  font_size(b), foreground());
    auto asset = assets_.find(index);
    if (asset != assets_.end() && asset->second.status == Asset::Ready) {
        asset->second.use = ++clock_;
        if (asset->second.scene) {
            float fit = std::min(1.f, entry.width / std::max(1.f, asset->second.scene->width));
            draw_diagram(target, *asset->second.scene, x, y + 8 * zoom_, fit);
        } else if (asset->second.bitmap) {
            float fit = std::min(1.f, entry.width / (float)asset->second.width);
            target->DrawBitmap(asset->second.bitmap.Get(), D2D1::RectF(x, y, x + asset->second.width * fit,
                                                                       y + asset->second.height * fit));
        }
        return;
    }
    size_t selStart = std::min(selectFrom_, selectTo_), selEnd = std::max(selectFrom_, selectTo_);
    for (size_t c = 0; c < entry.layouts.size(); ++c) {
        float tx = x + entry.offsets[c] + (b.kind == Kind::TableRow ? 10 * zoom_ : 0),
              ty = y + (b.kind == Kind::TableRow  ? 8 * zoom_
                        : b.kind == Kind::Heading ? 8 * zoom_
                                                  : 0);
        auto *tl = entry.layouts[c].Get();
        const Text *text = b.kind == Kind::TableRow && c < b.cells.size() ? &b.cells[c] : &b.text;
        bool placeholder = b.kind == Kind::Image || b.kind == Kind::Diagram;
        const size_t start = b.text_start + entry.textOffsets[c], end = start + text->value.size();
        if (!placeholder) {
            if (selEnd > start && selStart < end)
                range_rects(target, tl, (uint32_t)(std::max(selStart, start) - start),
                            (uint32_t)(std::min(selEnd, end) - std::max(selStart, start)), tx, ty,
                            rgb(dark_ ? 0x34598a : 0xc6ddff));
            for (size_t m = 0; m < matches_.size(); ++m) {
                const auto &match = matches_[m];
                if (match.block != index)
                    continue;
                const auto a = match.absolute, z = a + match.length;
                if (z > start && a < end)
                    range_rects(target, tl, (uint32_t)(std::max(a, start) - start),
                                (uint32_t)(std::min(z, end) - std::max(a, start)), tx, ty,
                                rgb((int)m == activeMatch_ ? 0xffbd57 : 0xffe5a1, .8f));
            }
            for (const auto &span : text->spans) {
                if (span.style & Mono)
                    range_rects(target, tl, span.start, span.length, tx, ty,
                                rgb(dark_ ? 0x354357 : 0xe6edf4, .65f));
            }
        }
        brush_->SetColor(placeholder ? rgb(dark_ ? 0xaab6ca : 0x67758a) : foreground());
        target->DrawTextLayout(D2D1::Point2F(tx, ty), tl, brush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        if (b.kind == Kind::TableRow) {
            brush_->SetColor(rgb(dark_ ? 0x3b4658 : 0xd8e0ea));
            float cw = entry.width / std::max(size_t(1), entry.layouts.size());
            target->DrawRectangle(
                D2D1::RectF(x + entry.offsets[c], y, x + entry.offsets[c] + cw, y + entry.height),
                brush_.Get(), .6f);
        }
    }
}
void View::render(ID2D1RenderTarget *target) {
    target->BeginDraw();
    target->Clear(background());
    if (!doc_ || doc_->blocks.empty()) {
        draw_text(target, emptyTitle_.empty() ? L"KeepMD" : emptyTitle_,
                  D2D1::RectF(48, 60, width_ - 48, 110), 34, foreground(), true);
    } else {
        size_t first = heights_.locate(std::max(0.f, scrollY_ - 20));
        float y = heights_.prefix(first) + 24 - scrollY_;
        size_t last = first;
        for (size_t i = first; i < doc_->blocks.size() && y < height_ + 100; ++i) {
            auto &entry = layout(i);
            draw_block(target, i, entry, y);
            y += entry.height;
            last = i;
        }
        trim(first, last);
        update_scrollbars();
    }
    auto hr = target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET && target == target_.Get()) {
        reset_device();
    }
}
void View::paint() {
    auto start = std::chrono::steady_clock::now();
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    ensure_target();
    if (target_ && brush_ && dw_)
        render(target_.Get());
    EndPaint(hwnd_, &ps);
    ++paintCount_;
    lastPaintMs_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (inputStarted_ != std::chrono::steady_clock::time_point{}) {
        double latency =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputStarted_)
                .count();
        if (frameSamples_.size() >= 2000)
            frameSamples_.erase(frameSamples_.begin(), frameSamples_.begin() + 1000);
        frameSamples_.push_back({lastPaintMs_, latency});
        inputStarted_ = {};
    }
}
void View::request_asset(size_t index) {
    if (!doc_ || index >= doc_->blocks.size() || assets_.contains(index))
        return;
    if (!worker_)
        worker_ = std::make_unique<Worker>();
    const auto block = doc_->blocks[index];
    const auto base = path_.parent_path();
    const auto generation = generation_;
    const float zoom = zoom_;
    if (!worker_->push([this, block, base, generation, index, zoom] {
            Result result;
            result.generation = generation;
            result.block = index;
            CoScope co;
            try {
                if (block.kind == Kind::Diagram) {
                    auto parsed = parse_diagram(utf8(block.text.value));
                    if (!parsed.ok)
                        result.error = parsed.error;
                    else {
                        auto scene = std::make_shared<DiagramScene>();
                        scene->graph = std::move(parsed.graph);
                        ComPtr<IDWriteFactory> factory;
                        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                            reinterpret_cast<IUnknown **>(factory.GetAddressOf()));
                        ComPtr<IDWriteTextFormat> format;
                        if (factory)
                            factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                      16 * zoom, L"zh-CN", format.GetAddressOf());
                        std::vector<mermaid::Size> sizes;
                        for (const auto &node : scene->graph.nodes) {
                            auto label = wide(node.label.empty() ? node.id : node.label);
                            ComPtr<IDWriteTextLayout> text;
                            DWRITE_TEXT_METRICS metrics{};
                            if (factory && format)
                                factory->CreateTextLayout(label.data(), (UINT32)label.size(), format.Get(),
                                                          230 * zoom, 10000, text.GetAddressOf());
                            if (text)
                                text->GetMetrics(&metrics);
                            float w = std::max(96 * zoom, metrics.width + 36 * zoom),
                                  h = std::max(48 * zoom, metrics.height + 28 * zoom);
                            if (node.shape == mermaid::NodeShape::Diamond) {
                                w *= 1.45f;
                                h *= 1.5f;
                            }
                            if (node.shape == mermaid::NodeShape::Hexagon)
                                w *= 1.5f;
                            if (node.shape == mermaid::NodeShape::Circle)
                                w = h = std::max(w, h);
                            sizes.push_back({w, h});
                        }
                        scene->layout = mermaid::layout(scene->graph, sizes, 44 * zoom, 80 * zoom);
                        // Leave a border for loop edges, group labels and arrow routing.
                        for (auto &r : scene->layout.nodes) {
                            r.left += 35 * zoom;
                            r.right += 35 * zoom;
                            r.top += 45 * zoom;
                            r.bottom += 45 * zoom;
                        }
                        scene->width = scene->layout.width + 70 * zoom;
                        scene->height = scene->layout.height + 90 * zoom;
                        for (const auto &edge : scene->graph.edges) {
                            auto from = scene->layout.ranks[edge.from], to = scene->layout.ranks[edge.to];
                            if (to <= from || to > from + 1) {
                                bool v = scene->graph.direction == mermaid::Direction::TopToBottom ||
                                         scene->graph.direction == mermaid::Direction::BottomToTop;
                                if (v)
                                    scene->width = scene->layout.width + 170 * zoom;
                                else
                                    scene->height = scene->layout.height + 190 * zoom;
                            }
                        }
                        for (const auto &group : scene->graph.subgraphs) {
                            mermaid::Rect bounds{1e8f, 1e8f, -1e8f, -1e8f};
                            for (auto node : group.nodes)
                                if (node < scene->layout.nodes.size()) {
                                    const auto &r = scene->layout.nodes[node];
                                    bounds.left = std::min(bounds.left, r.left - 16 * zoom);
                                    bounds.top = std::min(bounds.top, r.top - 32 * zoom);
                                    bounds.right = std::max(bounds.right, r.right + 16 * zoom);
                                    bounds.bottom = std::max(bounds.bottom, r.bottom + 16 * zoom);
                                }
                            scene->groups.push_back(bounds);
                        }
                        result.scene = std::move(scene);
                    }
                } else {
                    if (block.target.find(L"://") != std::wstring::npos || block.target.starts_with(L"data:"))
                        result.error = L"远程或内嵌图片暂未加载";
                    else {
                        auto imagePath = std::filesystem::path(decode_url_path(block.target));
                        if (imagePath.is_relative())
                            imagePath = base / imagePath;
                        ComPtr<IWICImagingFactory> factory;
                        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(factory.GetAddressOf()));
                        ComPtr<IWICBitmapDecoder> decoder;
                        ComPtr<IWICBitmapFrameDecode> frame;
                        HRESULT hr = factory ? factory->CreateDecoderFromFilename(
                                                   imagePath.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())
                                             : E_FAIL;
                        if (SUCCEEDED(hr))
                            hr = decoder->GetFrame(0, frame.GetAddressOf());
                        UINT w = 0, h = 0;
                        if (SUCCEEDED(hr))
                            hr = frame->GetSize(&w, &h);
                        bool dimensionsAllowed =
                            w && h && w <= 16384 && h <= 16384 && (uint64_t)w * h <= 64ull * 1024 * 1024;
                        if (SUCCEEDED(hr) && !dimensionsAllowed)
                            result.error = L"图片尺寸超过预览上限（最长边 16384、总计 64M 像素）";
                        if (SUCCEEDED(hr) && dimensionsAllowed) {
                            float scale = std::min({1.f, 1600.f / w, 1600.f / h});
                            result.width = std::max(1u, (UINT)(w * scale));
                            result.height = std::max(1u, (UINT)(h * scale));
                            ComPtr<IWICBitmapScaler> scaler;
                            hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
                            if (SUCCEEDED(hr))
                                hr = scaler->Initialize(frame.Get(), result.width, result.height,
                                                        WICBitmapInterpolationModeFant);
                            ComPtr<IWICFormatConverter> converter;
                            if (SUCCEEDED(hr))
                                hr = factory->CreateFormatConverter(converter.GetAddressOf());
                            if (SUCCEEDED(hr))
                                hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                           WICBitmapDitherTypeNone, nullptr, 0,
                                                           WICBitmapPaletteTypeCustom);
                            if (SUCCEEDED(hr)) {
                                result.pixels.resize((size_t)result.width * result.height * 4);
                                hr = converter->CopyPixels(nullptr, result.width * 4,
                                                           (UINT)result.pixels.size(), result.pixels.data());
                            }
                        } else
                            hr = E_FAIL;
                        if (FAILED(hr)) {
                            if (result.error.empty())
                                result.error = L"无法读取图片：" + imagePath.filename().wstring();
                            result.pixels.clear();
                        }
                    }
                }
            } catch (...) {
                result.error = L"资源处理失败";
                result.pixels.clear();
            }
            {
                std::lock_guard lock(resultsMutex_);
                results_.push_back(std::move(result));
            }
            PostMessageW(hwnd_, WM_ASSET_READY, 0, 0);
        }))
        return;
    assets_.emplace(index, Asset{});
}
void View::collect_assets() {
    std::vector<Result> results;
    {
        std::lock_guard lock(resultsMutex_);
        results.swap(results_);
    }
    ensure_target();
    for (auto &result : results) {
        if (result.generation != generation_ || !assets_.contains(result.block))
            continue;
        auto &asset = assets_[result.block];
        asset.error = std::move(result.error);
        asset.scene = std::move(result.scene);
        asset.width = result.width;
        asset.height = result.height;
        asset.use = ++clock_;
        if (!result.pixels.empty() && target_) {
            auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            auto hr = target_->CreateBitmap(D2D1::SizeU(result.width, result.height), result.pixels.data(),
                                            result.width * 4, props, asset.bitmap.GetAddressOf());
            if (FAILED(hr))
                asset.error = L"图片显示资源不可用";
            else
                asset.bytes = result.pixels.size();
        }
        if (asset.scene)
            asset.bytes = asset.scene->graph.nodes.size() * 512 + asset.scene->graph.edges.size() * 128;
        assetBytes_ += asset.bytes;
        asset.status = asset.scene || asset.bitmap ? Asset::Ready : Asset::Failed;
        auto entry = cache_.find(result.block);
        if (entry != cache_.end()) {
            cacheChars_ -= entry->second.chars;
            cache_.erase(entry);
        }
    }
    invalidate();
}
bool View::assets_pending() const {
    for (const auto &[id, asset] : assets_) {
        (void)id;
        if (asset.status == Asset::Pending)
            return true;
    }
    return false;
}
void View::draw_diagram(ID2D1RenderTarget *target, const DiagramScene &scene, float x, float y, float fit) {
    D2D1_MATRIX_3X2_F old;
    target->GetTransform(&old);
    target->SetTransform(D2D1::Matrix3x2F::Scale(fit, fit) * D2D1::Matrix3x2F::Translation(x, y));
    for (size_t i = 0; i < scene.groups.size(); ++i) {
        const auto &r = scene.groups[i];
        if (r.left > r.right)
            continue;
        auto rect = D2D1::RectF(r.left, r.top, r.right, r.bottom);
        fill(target, rect, rgb(dark_ ? 0x222e40 : 0xf0f5fa));
        brush_->SetColor(rgb(0x8198b1));
        target->DrawRectangle(rect, brush_.Get(), 1);
        draw_text(target, wide(scene.graph.subgraphs[i].label),
                  D2D1::RectF(r.left + 8, r.top + 3, r.right, r.top + 28), 13 * zoom_, foreground(), true);
    }
    bool vertical = scene.graph.direction == mermaid::Direction::TopToBottom ||
                    scene.graph.direction == mermaid::Direction::BottomToTop;
    bool reverse = scene.graph.direction == mermaid::Direction::BottomToTop ||
                   scene.graph.direction == mermaid::Direction::RightToLeft;
    size_t routeIndex = 0;
    for (const auto &edge : scene.graph.edges) {
        if (edge.from >= scene.layout.nodes.size() || edge.to >= scene.layout.nodes.size())
            continue;
        const auto &a = scene.layout.nodes[edge.from];
        const auto &b = scene.layout.nodes[edge.to];
        D2D1_POINT_2F start, end;
        std::vector<D2D1_POINT_2F> points;
        if (vertical) {
            start = D2D1::Point2F((a.left + a.right) * .5f, reverse ? a.top : a.bottom);
            end = D2D1::Point2F((b.left + b.right) * .5f, reverse ? b.bottom : b.top);
        } else {
            start = D2D1::Point2F(reverse ? a.left : a.right, (a.top + a.bottom) * .5f);
            end = D2D1::Point2F(reverse ? b.right : b.left, (b.top + b.bottom) * .5f);
        }
        size_t fromRank = scene.layout.ranks[edge.from], toRank = scene.layout.ranks[edge.to];
        bool skip = toRank > fromRank + 1 || fromRank > toRank + 1;
        bool back = vertical ? (reverse ? end.y >= start.y : end.y <= start.y)
                             : (reverse ? end.x >= start.x : end.x <= start.x);
        if (back || skip) {
            float lane = (34 + (routeIndex++ % 4) * 22) * zoom_;
            if (vertical) {
                float side = 0;
                for (const auto &r : scene.layout.nodes)
                    side = std::max(side, r.right);
                side += lane;
                start = {a.right, (a.top + a.bottom) * .5f};
                end = {b.right, (b.top + b.bottom) * .5f};
                if (edge.from == edge.to) {
                    start.y -= 10 * zoom_;
                    end.y += 10 * zoom_;
                }
                points = {start, {side, start.y}, {side, end.y}};
            } else {
                float side = 0;
                for (const auto &r : scene.layout.nodes)
                    side = std::max(side, r.bottom);
                side += lane;
                start = {(a.left + a.right) * .5f, a.bottom};
                end = {(b.left + b.right) * .5f, b.bottom};
                if (edge.from == edge.to) {
                    start.x -= 10 * zoom_;
                    end.x += 10 * zoom_;
                }
                points = {start, {start.x, side}, {end.x, side}};
            }
        } else if (vertical) {
            points.push_back(start);
            float mid = (start.y + end.y) * .5f;
            points.push_back({start.x, mid});
            points.push_back({end.x, mid});
        } else {
            points.push_back(start);
            float mid = (start.x + end.x) * .5f;
            points.push_back({mid, start.y});
            points.push_back({mid, end.y});
        }
        points.push_back(end);
        brush_->SetColor(rgb(dark_ ? 0x9bacbf : 0x627893));
        for (size_t i = 1; i < points.size(); ++i)
            target->DrawLine(points[i - 1], points[i], brush_.Get(), 1.6f * edge.strokeScale * zoom_,
                             edge.dashed ? dashed_.Get() : nullptr);
        if (edge.directed) {
            auto prev = points[points.size() - 2];
            float dx = end.x - prev.x, dy = end.y - prev.y;
            float len = std::hypot(dx, dy);
            if (len > 0) {
                dx /= len;
                dy /= len;
                float s = 7 * zoom_;
                target->DrawLine(end, {end.x - dx * s + dy * s * .5f, end.y - dy * s - dx * s * .5f},
                                 brush_.Get(), 2 * zoom_);
                target->DrawLine(end, {end.x - dx * s - dy * s * .5f, end.y - dy * s + dx * s * .5f},
                                 brush_.Get(), 2 * zoom_);
            }
        }
        if (!edge.label.empty()) {
            auto p = points[1];
            auto q = points[points.size() - 2];
            float cx = (p.x + q.x) * .5f, cy = (p.y + q.y) * .5f;
            auto label = wide(edge.label);
            float w = std::max(42.f, (float)label.size() * 11.f) * zoom_;
            auto rect = D2D1::RectF(cx - w / 2, cy - 12 * zoom_, cx + w / 2, cy + 12 * zoom_);
            fill(target, rect, background());
            draw_text(target, label, rect, 13 * zoom_, foreground());
        }
    }
    for (size_t i = 0; i < scene.graph.nodes.size(); ++i) {
        const auto &node = scene.graph.nodes[i];
        const auto &r = scene.layout.nodes[i];
        auto rect = D2D1::RectF(r.left, r.top, r.right, r.bottom);
        auto fillColor = node.style.hasFill ? rgb(node.style.fill.rgb, node.style.fill.alpha)
                                            : rgb(dark_ ? 0x2c4261 : 0xe8f0fb);
        auto stroke =
            node.style.hasStroke ? rgb(node.style.stroke.rgb, node.style.stroke.alpha) : rgb(0x6b91c0);
        float sw = node.style.hasStrokeWidth ? node.style.strokeWidth : 1.5f;
        auto shape = node.shape;
        bool ellipse = shape == mermaid::NodeShape::Circle;
        bool diamond = shape == mermaid::NodeShape::Diamond;
        bool hexagon = shape == mermaid::NodeShape::Hexagon;
        if (diamond || hexagon) {
            ComPtr<ID2D1PathGeometry> geometry;
            ComPtr<ID2D1GeometrySink> sink;
            d2d_->CreatePathGeometry(geometry.GetAddressOf());
            if (geometry && SUCCEEDED(geometry->Open(sink.GetAddressOf()))) {
                if (hexagon) {
                    float inset = (r.right - r.left) * .16f;
                    sink->BeginFigure({r.left + inset, r.top}, D2D1_FIGURE_BEGIN_FILLED);
                    D2D1_POINT_2F p[] = {{r.right - inset, r.top},
                                         {r.right, (r.top + r.bottom) / 2},
                                         {r.right - inset, r.bottom},
                                         {r.left + inset, r.bottom},
                                         {r.left, (r.top + r.bottom) / 2}};
                    sink->AddLines(p, 5);
                } else {
                    sink->BeginFigure({(r.left + r.right) / 2, r.top}, D2D1_FIGURE_BEGIN_FILLED);
                    D2D1_POINT_2F p[] = {{r.right, (r.top + r.bottom) / 2},
                                         {(r.left + r.right) / 2, r.bottom},
                                         {r.left, (r.top + r.bottom) / 2}};
                    sink->AddLines(p, 3);
                }
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                brush_->SetColor(fillColor);
                target->FillGeometry(geometry.Get(), brush_.Get());
                brush_->SetColor(stroke);
                target->DrawGeometry(geometry.Get(), brush_.Get(), sw);
            }
        } else if (ellipse) {
            auto e = D2D1::Ellipse({(r.left + r.right) / 2, (r.top + r.bottom) / 2}, (r.right - r.left) / 2,
                                   (r.bottom - r.top) / 2);
            brush_->SetColor(fillColor);
            target->FillEllipse(e, brush_.Get());
            brush_->SetColor(stroke);
            target->DrawEllipse(e, brush_.Get(), sw);
        } else {
            float radius = shape == mermaid::NodeShape::Rectangle ? 0
                           : shape == mermaid::NodeShape::Stadium ? (r.bottom - r.top) / 2
                                                                  : 9 * zoom_;
            auto rr = D2D1::RoundedRect(rect, radius, radius);
            brush_->SetColor(fillColor);
            target->FillRoundedRectangle(rr, brush_.Get());
            brush_->SetColor(stroke);
            target->DrawRoundedRectangle(rr, brush_.Get(), sw);
        }
        ComPtr<IDWriteTextFormat> format;
        dw_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 16 * zoom_, L"zh-CN", format.GetAddressOf());
        if (format) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            auto label = wide(node.label.empty() ? node.id : node.label);
            brush_->SetColor(node.style.hasText ? rgb(node.style.text.rgb) : foreground());
            float pad = diamond ? (r.right - r.left) * .2f : hexagon ? (r.right - r.left) * .17f : 12 * zoom_;
            target->DrawText(
                label.data(), (UINT32)label.size(), format.Get(),
                D2D1::RectF(r.left + pad, r.top + 6 * zoom_, r.right - pad, r.bottom - 6 * zoom_),
                brush_.Get());
        }
    }
    target->SetTransform(old);
}
size_t View::hit(float x, float y, bool strict) {
    if (!doc_ || doc_->blocks.empty())
        return 0;
    float docY = y + scrollY_ - 24;
    size_t i = heights_.locate(std::max(0.f, docY));
    auto &entry = layout(i);
    const auto &b = doc_->blocks[i];
    float left = left_margin() + (b.indent * 22 + b.quote * 14) * zoom_;
    float top = heights_.prefix(i) + 24 - scrollY_;
    if (entry.layouts.empty())
        return b.text_start;
    size_t cell = 0;
    for (size_t c = 1; c < entry.offsets.size(); ++c)
        if (x - left >= entry.offsets[c])
            cell = c;
    float tx = left + entry.offsets[cell] + (b.kind == Kind::TableRow ? 10 * zoom_ : 0),
          ty = top + (b.kind == Kind::TableRow  ? 8 * zoom_
                      : b.kind == Kind::Heading ? 8 * zoom_
                                                : 0);
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    entry.layouts[cell]->HitTestPoint(x - tx, y - ty, &trailing, &inside, &metrics);
    if (strict && (!inside || x - tx < metrics.left || x - tx >= metrics.left + metrics.width ||
                   y - ty < metrics.top || y - ty >= metrics.top + metrics.height))
        return SIZE_MAX;
    auto plain = block_plain(b);
    size_t offset = std::min(plain.size(), (size_t)entry.textOffsets[cell] + metrics.textPosition +
                                               (trailing ? metrics.length : 0));
    return b.text_start + offset;
}
std::wstring View::link_at(float x, float y) {
    if (!doc_ || doc_->blocks.empty())
        return {};
    size_t pos = hit(x, y, true);
    if (pos == SIZE_MAX)
        return {};
    size_t i = heights_.locate(std::max(0.f, y + scrollY_ - 24));
    const auto &b = doc_->blocks[i];
    uint32_t offset = (uint32_t)(pos - b.text_start);
    if (b.kind == Kind::TableRow) {
        for (const auto &cell : b.cells) {
            if (offset <= cell.value.size()) {
                for (const auto &s : cell.spans)
                    if (offset >= s.start && offset < s.start + s.length)
                        return s.link;
                break;
            }
            offset -= (uint32_t)cell.value.size() + 1;
        }
    } else
        for (const auto &s : b.text.spans)
            if (offset >= s.start && offset < s.start + s.length)
                return s.link;
    return {};
}
std::wstring View::source_at(float, float y) const {
    if (!doc_ || doc_->blocks.empty())
        return {};
    size_t index = heights_.locate(std::max(0.f, y + scrollY_ - 24));
    const auto &block = doc_->blocks[index];
    if (block.kind == Kind::Diagram)
        return block.text.value;
    if (block.kind != Kind::Code && block.kind != Kind::Html)
        return {};
    size_t first = index;
    while (first > 0 && doc_->blocks[first - 1].kind == block.kind &&
           doc_->blocks[first - 1].source == block.source)
        --first;
    std::wstring result;
    for (size_t i = first; i < doc_->blocks.size() && doc_->blocks[i].kind == block.kind &&
                           doc_->blocks[i].source == block.source;
         ++i) {
        result += doc_->blocks[i].text.value;
        if (!doc_->blocks[i].joins_next)
            result += L'\n';
    }
    return result;
}
void View::toggle_diagram(float, float y) {
    if (!doc_ || doc_->blocks.empty())
        return;
    size_t index = heights_.locate(std::max(0.f, y + scrollY_ - 24));
    if (doc_->blocks[index].kind != Kind::Diagram)
        return;
    if (expandedDiagrams_.contains(index))
        expandedDiagrams_.erase(index);
    else
        expandedDiagrams_.insert(index);
    auto entry = cache_.find(index);
    if (entry != cache_.end()) {
        cacheChars_ -= entry->second.chars;
        cache_.erase(entry);
    }
    invalidate();
}
void View::mouse_down(float x, float y, bool extend) {
    SetFocus(hwnd_);
    clickedLink_ = link_at(x, y);
    selectTo_ = hit(x, y);
    if (!extend)
        selectFrom_ = selectTo_;
    dragging_ = true;
    moved_ = false;
    mouseStart_ = {x, y};
    SetCapture(hwnd_);
    invalidate();
}
void View::mouse_move(float x, float y) {
    if (!dragging_)
        return;
    if (std::hypot(x - mouseStart_.x, y - mouseStart_.y) > 3)
        moved_ = true;
    selectTo_ = hit(x, y);
    if (y < 0)
        scroll(-20);
    else if (y > height_)
        scroll(20);
    invalidate();
}
void View::mouse_up(float x, float y) {
    if (!dragging_)
        return;
    dragging_ = false;
    ReleaseCapture();
    if (!moved_ && !clickedLink_.empty() && on_link && link_at(x, y) == clickedLink_)
        on_link(clickedLink_);
    else
        selectTo_ = hit(x, y);
    invalidate();
}
void View::select_all() {
    if (doc_) {
        selectFrom_ = 0;
        selectTo_ = doc_->text_size;
        invalidate();
    }
}
std::wstring View::selection() const {
    return doc_ ? doc_->text_range(selectFrom_, selectTo_) : L"";
}
void View::set_matches(std::vector<Match> matches, int active) {
    matches_ = std::move(matches);
    activeMatch_ = active;
    if (active >= 0 && (size_t)active < matches_.size())
        goto_block(matches_[active].block);
    invalidate();
}
bool View::snapshot(const std::filesystem::path &path) {
    // Diagnostic capture of the actual visible reader, including loaded images.
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic.GetAddressOf()))))
        return false;
    UINT w = (UINT)std::max(1.f, width_ * dpi_), h = (UINT)std::max(1.f, height_ * dpi_);
    HDC screen = GetDC(hwnd_), memory = CreateCompatibleDC(screen);
    HBITMAP capture = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(memory, capture);
    BOOL captured = BitBlt(memory, 0, 0, w, h, screen, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(hwnd_, screen);
    ComPtr<IWICBitmap> bitmap;
    HRESULT captureResult =
        captured ? wic->CreateBitmapFromHBITMAP(capture, nullptr, WICBitmapIgnoreAlpha, bitmap.GetAddressOf())
                 : E_FAIL;
    DeleteObject(capture);
    if (FAILED(captureResult))
        return false;
    ComPtr<IWICStream> stream;
    wic->CreateStream(stream.GetAddressOf());
    if (!stream || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (!encoder || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) || FAILED(frame->Initialize(nullptr)))
        return false;
    frame->SetSize(w, h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    return SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
           SUCCEEDED(encoder->Commit());
}
} // namespace keepmd
