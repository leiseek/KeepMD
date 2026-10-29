#include "view.h"
#include "html_image.h"
#include "paths.h"
#include "remote_image.h"
#include "image_cache.h"
#include "math_formula.h"
#include "ui.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <limits>
#include <numeric>
#include <objidl.h>

namespace keepmd {
namespace {
// Computer Modern sits visually smaller than the UI face at the same pixel size, so formulas are set slightly
// larger than the surrounding text, as KaTeX does.
constexpr float kMathScale = 1.15f;

// Reader-side timer clearing the expired "已复制" chip label. The reader window has no other timers.
constexpr UINT kCopyFeedbackTimer = 5;

// Muted, GitHub-like token colours that stay legible on both themes' code surfaces. Order matches
// the Style token bits; entries without their own hue share a colour by design (numbers and keys).
struct TokenColor {
    uint32_t style;
    COLORREF light, dark;
};
constexpr TokenColor kTokenColors[] = {
    {TokKeyword, RGB(175, 34, 45), RGB(255, 116, 106)},  {TokBuiltin, RGB(150, 62, 8), RGB(255, 166, 87)},
    {TokString, RGB(14, 52, 118), RGB(165, 214, 255)},   {TokComment, RGB(100, 109, 122), RGB(139, 148, 158)},
    {TokNumber, RGB(5, 76, 168), RGB(121, 192, 255)},    {TokFunction, RGB(121, 72, 212), RGB(210, 168, 255)},
    {TokKey, RGB(5, 76, 168), RGB(121, 192, 255)},
};

// A typeset formula standing in for its LaTeX source inside a text layout. The source text stays in the layout,
// so selection, copy, search and hit testing keep working on it.
class FormulaObject final : public IDWriteInlineObject {
  public:
    FormulaObject(std::shared_ptr<MathFormula> formula, std::shared_ptr<FormulaInk> ink)
        : formula_(std::move(formula)), ink_(std::move(ink)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteInlineObject)) {
            *object = static_cast<IDWriteInlineObject *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs_;
        if (!left)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE Draw(void *, IDWriteTextRenderer *, FLOAT x, FLOAT y, BOOL, BOOL,
                                   IUnknown *) override {
        if (ink_->target)
            formula_->draw(ink_->target, x, y, ink_->color);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS *metrics) override {
        *metrics = {formula_->width, formula_->ascent + formula_->descent, formula_->ascent, FALSE};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS *overhangs) override {
        *overhangs = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION *before,
                                                 DWRITE_BREAK_CONDITION *after) override {
        *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

  private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<MathFormula> formula_;
    std::shared_ptr<FormulaInk> ink_;
};

float clampf(float v, float lo, float hi) {
    return std::clamp(v, lo, std::max(lo, hi));
}
D2D1_COLOR_F rgb(unsigned c, float a = 1) {
    return D2D1::ColorF(c, a);
}
D2D1_COLOR_F color(COLORREF c, float a = 1) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}
std::wstring host_of(std::wstring_view url) {
    auto start = url.find(L"://");
    if (start == std::wstring_view::npos)
        return {};
    start += 3;
    auto end = url.find_first_of(L"/?#", start);
    return std::wstring(url.substr(start, end == std::wstring_view::npos ? url.npos : end - start));
}
// Opaque mix of two theme colours; `amount` is the share of `top`.
D2D1_COLOR_F mix(COLORREF top, COLORREF bottom, float amount) {
    auto channel = [&](int a, int b) { return (a * amount + b * (1 - amount)) / 255.f; };
    return D2D1::ColorF(channel(GetRValue(top), GetRValue(bottom)), channel(GetGValue(top), GetGValue(bottom)),
                        channel(GetBValue(top), GetBValue(bottom)));
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
        a.image_width != b.image_width || a.image_height != b.image_height || a.text_align != b.text_align ||
        !equal_text(a.text, b.text))
        return false;
    for (size_t i = 0; i < a.cells.size(); ++i)
        if (!equal_text(a.cells[i], b.cells[i]))
            return false;
    return true;
}
// Layout size of a decoded image: vector images are rasterized at `density` pixels per layout unit.
UINT layout_extent(UINT pixels, float density) {
    return std::max(1u, (UINT)std::lround(pixels / std::max(density, 1e-3f)));
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
View::View(HWND hwnd, std::filesystem::path imageCache) : hwnd_(hwnd), imageCache_(std::move(imageCache)) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown **>(dw_.GetAddressOf()));
    if (d2d_) {
        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties();
        properties.dashStyle = D2D1_DASH_STYLE_DASH;
        d2d_->CreateStrokeStyle(properties, nullptr, 0, dashed_.GetAddressOf());
    }
    // Windows 11 ships optical-size variants of Segoe UI; older systems keep Segoe UI.
    ComPtr<IDWriteFontCollection> fonts;
    if (dw_ && SUCCEEDED(dw_->GetSystemFontCollection(fonts.GetAddressOf()))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists)) && exists)
            bodyFamily_ = L"Segoe UI Variable Text";
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Display", &index, &exists)) && exists)
            headingFamily_ = L"Segoe UI Variable Display";
    }
    resize();
}
View::~View() {
    remoteCancel_->store(true);
    for (auto &worker : remoteWorkers_) worker.reset();
    worker_.reset();
}
void View::invalidate() {
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void View::reset_device() {
    brush_.Reset();
    target_.Reset();
    for (auto &brush : tokenBrushes_)
        brush.Reset();
    ++generation_;
    if (worker_)
        worker_->clear();
    assets_.clear();
    assetBytes_ = 0;
    reset_layout(true);
    invalidate();
}
D2D1_COLOR_F View::foreground() const {
    return color(ui::palette(dark_).text);
}
D2D1_COLOR_F View::background() const {
    return color(ui::palette(dark_).surface);
}
float View::content_width() const {
    return std::max(180.f, std::min(width_ - 64.f, readingWidth_ * zoom_));
}
float View::left_margin() const {
    return std::max(24.f, (width_ - content_width()) * .5f) - scrollX_;
}
float View::font_size(const Block &b) const {
    if (b.kind == Kind::Heading) {
        static const float sizes[] = {30, 24, 20, 18, 17, 16};
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
    if (!sameFile) {
        remoteCancel_->store(true);
        remoteCancel_ = std::make_shared<std::atomic_bool>(false);
        ++remoteGeneration_;
        for (auto &worker : remoteWorkers_) if (worker) worker->clear();
        remoteQueue_.clear();
        remoteActive_ = 0;
        remoteEnabled_ = false;
        remoteAssets_.clear();
        remoteBytes_ = 0;
    }
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
                                       entry->second.width + (block.indent * 22 + block.quote * 18) * zoom_);
                    cache_.emplace(i, std::move(entry->second));
                    ++reusedLayouts_;
                }
                group->second.erase(candidate);
                break;
            }
        }
    }
    if (remoteEnabled_) queue_document_images();
    invalidate();
}
void View::reset_layout(bool preserveAnchor) {
    size_t anchor = heights_.locate(scrollY_);
    float local = scrollY_ - heights_.prefix(anchor);
    cache_.clear();
    cacheChars_ = 0;
    groupWidth_.clear();
    widest_ = content_width();
    std::vector<float> values;
    if (doc_) {
        values.reserve(doc_->blocks.size());
        for (const auto &b : doc_->blocks) {
            float fs = font_size(b),
                  usable = std::max(120.f, content_width() - (b.indent * 22 + b.quote * 18) * zoom_);
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
    if (target_) {
        target_->CreateSolidColorBrush(foreground(), brush_.ReleaseAndGetAddressOf());
        target_->CreateSolidColorBrush(foreground(), linkBrush_.ReleaseAndGetAddressOf());
        target_->CreateSolidColorBrush(foreground(), mutedBrush_.ReleaseAndGetAddressOf());
        for (auto &brush : tokenBrushes_)
            target_->CreateSolidColorBrush(foreground(), brush.ReleaseAndGetAddressOf());
    }
}
bool View::same_group(size_t a, size_t b) const {
    const auto &x = doc_->blocks[a], &y = doc_->blocks[b];
    if (x.kind != y.kind || x.indent != y.indent || x.quote != y.quote)
        return false;
    if (x.kind == Kind::Code || x.kind == Kind::Html)
        return x.source == y.source;
    return x.kind == Kind::TableRow && !y.header;
}
bool View::group_first(size_t index) const {
    return index == 0 || !same_group(index - 1, index);
}
bool View::group_last(size_t index) const {
    return index + 1 >= doc_->blocks.size() || !same_group(index, index + 1);
}
float View::block_left(const Block &b) const {
    return left_margin() + (b.indent * 22 + b.quote * 18) * zoom_;
}
D2D1_POINT_2F View::text_origin(size_t index, const Entry &entry, size_t cell, float x, float y) const {
    const auto &b = doc_->blocks[index];
    float tx = x + (cell < entry.offsets.size() ? entry.offsets[cell] : 0), ty = y;
    if (b.kind == Kind::TableRow) {
        tx += 12 * zoom_;
        ty += 9 * zoom_;
    } else if (b.kind == Kind::Code || b.kind == Kind::Html) {
        tx += 6 * zoom_;
        ty += group_first(index) ? 12 * zoom_ : 0;
    } else if (b.kind == Kind::Heading)
        ty += entry.top;
    else if (b.kind == Kind::Image || b.kind == Kind::Diagram) {
        tx += entry.cardLeft + 46 * zoom_;
        ty += 14 * zoom_;
    }
    return {tx, ty};
}
View::Entry &View::layout(size_t index) {
    auto found = cache_.find(index);
    if (found != cache_.end()) {
        found->second.use = ++clock_;
        return found->second;
    }
    ensure_target(); // Text layouts reference the theme brushes as drawing effects.
    Entry entry;
    entry.use = ++clock_;
    const auto &b = doc_->blocks[index];
    const float fs = font_size(b);
    const float indent = (b.indent * 22 + b.quote * 18) * zoom_;
    float usable = std::max(100.f, content_width() - indent);
    entry.width = usable;
    if ((b.kind == Kind::Image || b.kind == Kind::Diagram) && !assets_.contains(index))
        request_asset(index);
    auto asset = assets_.find(index);
    if (asset != assets_.end() && asset->second.status == Asset::Ready) {
        asset->second.use = ++clock_;
        float w = asset->second.scene ? asset->second.scene->width : (float)asset->second.width;
        float h = asset->second.scene ? asset->second.scene->height : (float)asset->second.height;
        if (!asset->second.scene) {
            auto size = image_extent(asset->second.width, asset->second.height, b.image_width, b.image_height,
                                     usable, zoom_);
            w = size.width;
            h = size.height;
        }
        if (asset->second.scene && expandedDiagrams_.contains(index))
            entry.width = std::max(usable, w);
        float scale = std::min(1.f, entry.width / std::max(1.f, w));
        entry.height = h * scale + 30 * zoom_;
    } else if (b.kind == Kind::Rule)
        entry.height = 30 * zoom_;
    else {
        std::vector<const Text *> texts;
        Text placeholder;
        bool code = b.kind == Kind::Code || b.kind == Kind::Html;
        bool card = b.kind == Kind::Image || b.kind == Kind::Diagram;
        if (b.kind == Kind::Image) {
            placeholder.value = b.text.value.empty() ? L"图片" : b.text.value;
            auto detail = image_detail(index);
            uint32_t start = (uint32_t)placeholder.value.size() + 1;
            placeholder.value += L"\n" + detail.first;
            // A non-empty link marks the actionable line; it is drawn in the accent colour.
            placeholder.spans.push_back({start, (uint32_t)detail.first.size(), 0, detail.second ? L"#" : L""});
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
        float rowHeight = 0, textWidth = 0;
        float cellWidth = b.kind == Kind::TableRow ? entry.width / std::max(size_t(1), texts.size()) : usable;
        float maxWidth = code   ? 1000000.f
                         : card ? std::max(40.f, usable - 66 * zoom_)
                                : std::max(40.f, cellWidth - (b.kind == Kind::TableRow ? 24 * zoom_ : 0));
        bool heading = b.kind == Kind::Heading;
        float size = card ? 15 * zoom_ : fs;
        for (size_t c = 0; c < texts.size(); ++c) {
            const auto &text = *texts[c];
            ComPtr<IDWriteTextFormat> format;
            dw_->CreateTextFormat(code      ? ui::code_font()
                                  : heading ? headingFamily_.c_str()
                                            : bodyFamily_.c_str(),
                                  nullptr,
                                  heading || b.header ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
                                  format.GetAddressOf());
            if (!format)
                continue;
            format->SetWordWrapping(code ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
            // Typeset formulas first: a tall one (a fraction, an integral with limits) widens the line spacing.
            // A formula MicroTeX cannot typeset keeps its source text, styled below.
            std::vector<std::pair<DWRITE_TEXT_RANGE, std::shared_ptr<MathFormula>>> formulas;
            float mathAscent = 0, mathDescent = 0;
            bool displayOnly = false;
            if (!code && !card)
                for (const auto &span : text.spans) {
                    if (!(span.style & Formula))
                        continue;
                    auto formula = MathFormula::create(text.value.substr(span.start, span.length),
                                                       (span.style & DisplayMath) != 0, size * kMathScale);
                    if (!formula) {
                        ++entry.formulasSource;
                        continue;
                    }
                    ++entry.formulasTypeset;
                    mathAscent = std::max(mathAscent, formula->ascent);
                    mathDescent = std::max(mathDescent, formula->descent);
                    displayOnly = (span.style & DisplayMath) && span.start == 0 &&
                                  span.length == text.value.size() && text.spans.size() == 1;
                    formulas.push_back({{span.start, span.length}, std::move(formula)});
                }
            float lineHeight = size * (heading ? 1.32f : 1.6f), baseline = size * (heading ? 1.06f : 1.23f);
            if (!formulas.empty()) {
                // Every line of the paragraph grows alike, so its spacing stays even while the tallest formula
                // clears the lines above and below it.
                float below = std::max(lineHeight - baseline, mathDescent + size * .2f);
                baseline = std::max(baseline, mathAscent + size * .2f);
                lineHeight = baseline + below;
            }
            format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineHeight, baseline);
            if (displayOnly && b.kind != Kind::TableRow)
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            else if (b.kind == Kind::TableRow && c < b.align.size())
                format->SetTextAlignment(b.align[c] == 2   ? DWRITE_TEXT_ALIGNMENT_CENTER
                                         : b.align[c] == 3 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                           : DWRITE_TEXT_ALIGNMENT_LEADING);
            else if (!code && !card && b.kind != Kind::TableRow && b.text_align >= 2)
                format->SetTextAlignment(b.text_align == 2 ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                           : DWRITE_TEXT_ALIGNMENT_TRAILING);
            ComPtr<IDWriteTextLayout> tl;
            dw_->CreateTextLayout(text.value.data(), (UINT32)text.value.size(), format.Get(), maxWidth, 10000000.f,
                                  tl.GetAddressOf());
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
                    tl->SetFontFamilyName(ui::code_font(), range);
                    tl->SetFontSize(fs * .9f, range);
                }
                if (span.style & Formula) {
                    // Source kept for a formula that could not be typeset: the code face, tinted like inline code.
                    tl->SetFontFamilyName(ui::code_font(), range);
                    tl->SetFontSize(fs * .95f, range);
                }
                if (code && (span.style & kTokenStyles)) {
                    if (span.style & TokComment)
                        tl->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
                    if (auto *brush = token_brush(span.style))
                        tl->SetDrawingEffect(brush, range);
                }
                if ((!span.link.empty() && !card) || (span.style & Underline))
                    tl->SetUnderline(TRUE, range);
                if (card)
                    tl->SetDrawingEffect(span.link.empty() ? mutedBrush_.Get() : linkBrush_.Get(), range);
                else if (!span.link.empty() && linkBrush_)
                    tl->SetDrawingEffect(linkBrush_.Get(), range);
            }
            for (auto &[range, formula] : formulas) {
                ComPtr<IDWriteInlineObject> object;
                object.Attach(new FormulaObject(formula, formulaInk_));
                tl->SetInlineObject(object.Get(), range);
            }
            DWRITE_TEXT_METRICS metrics{};
            tl->GetMetrics(&metrics);
            rowHeight = std::max(rowHeight, metrics.height);
            textWidth = std::max(textWidth, metrics.widthIncludingTrailingWhitespace);
            if (code) {
                entry.width = std::max(usable, metrics.widthIncludingTrailingWhitespace + 28 * zoom_);
                auto &group = groupWidth_[b.source];
                group = std::max(group, entry.width);
            }
            entry.offsets.push_back(offset);
            entry.textOffsets.push_back(textOffset);
            entry.chars += text.value.size();
            entry.layouts.push_back(std::move(tl));
            offset += cellWidth;
            textOffset += (uint32_t)text.value.size() + 1;
        }
        if (heading) {
            entry.top = (index == 0 ? 8 : b.level <= 2 ? 24 : 16) * zoom_;
            entry.height = entry.top + rowHeight + (b.level <= 2 ? 20 : 12) * zoom_;
        } else if (code)
            entry.height = rowHeight + ((group_first(index) ? 12 : 0) + (group_last(index) ? 28 : 0)) * zoom_;
        else if (b.kind == Kind::TableRow)
            entry.height = rowHeight + (18 + (group_last(index) ? 16 : 0)) * zoom_;
        else if (card) {
            entry.cardWidth = b.kind == Kind::Diagram ? usable
                                                      : std::clamp(textWidth + 66 * zoom_, 240 * zoom_, usable);
            entry.cardLeft = b.text_align == 2   ? (usable - entry.cardWidth) * .5f
                             : b.text_align == 3 ? usable - entry.cardWidth
                                                 : 0;
            entry.height = rowHeight + 42 * zoom_;
        } else
            entry.height = rowHeight + (b.joins_next ? 0 : b.indent ? 8 : 14) * zoom_;
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
                     D2D1_COLOR_F color, bool bold, DWRITE_TEXT_ALIGNMENT align) {
    ComPtr<IDWriteTextFormat> format;
    dw_->CreateTextFormat(bodyFamily_.c_str(), nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
                          format.GetAddressOf());
    if (!format)
        return;
    format->SetTextAlignment(align);
    brush_->SetColor(color);
    target->DrawText(text.data(), (UINT32)text.size(), format.Get(), rect, brush_.Get());
}
void View::range_rects(ID2D1RenderTarget *target, IDWriteTextLayout *text, uint32_t start, uint32_t length,
                       float x, float y, D2D1_COLOR_F color, float radius) {
    if (!length)
        return;
    UINT32 count = 0;
    text->HitTestTextRange(start, length, x, y, nullptr, 0, &count);
    if (count > 10000)
        return;
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (FAILED(text->HitTestTextRange(start, length, x, y, metrics.data(), count, &count)))
        return;
    brush_->SetColor(color);
    for (const auto &m : metrics) {
        if (radius > 0) {
            float pad = 3 * zoom_;
            auto rect = D2D1::RectF(m.left - pad, m.top + 2 * zoom_, m.left + m.width + pad, m.top + m.height - zoom_);
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
        } else
            target->FillRectangle(D2D1::RectF(m.left, m.top, m.left + m.width, m.top + m.height), brush_.Get());
    }
}
void View::frame(ID2D1RenderTarget *target, D2D1_RECT_F box, bool first, bool last, D2D1_COLOR_F fill,
                 D2D1_COLOR_F line) {
    float radius = 8 * zoom_;
    auto shape = box;
    if (!first)
        shape.top -= radius * 2;
    if (!last)
        shape.bottom += radius * 2;
    target->PushAxisAlignedClip(D2D1::RectF(box.left - 2, box.top, box.right + 2, box.bottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
    auto rounded = D2D1::RoundedRect(
        D2D1::RectF(shape.left + .5f, shape.top + .5f, shape.right - .5f, shape.bottom - .5f), radius, radius);
    brush_->SetColor(fill);
    target->FillRoundedRectangle(rounded, brush_.Get());
    brush_->SetColor(line);
    target->DrawRoundedRectangle(rounded, brush_.Get(), 1.f);
    target->PopAxisAlignedClip();
}
void View::picture_icon(ID2D1RenderTarget *target, float x, float y, float size, D2D1_COLOR_F line) {
    float s = size / 20.f, w = 1.5f * zoom_;
    brush_->SetColor(line);
    target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 1 * s, y + 3 * s, x + 19 * s, y + 17 * s),
                                                   3 * s, 3 * s),
                                 brush_.Get(), w);
    target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 7 * s, y + 8 * s), 1.6f * s, 1.6f * s), brush_.Get(), w);
    target->DrawLine({x + 2 * s, y + 15 * s}, {x + 8 * s, y + 10 * s}, brush_.Get(), w);
    target->DrawLine({x + 8 * s, y + 10 * s}, {x + 12 * s, y + 13 * s}, brush_.Get(), w);
    target->DrawLine({x + 12 * s, y + 13 * s}, {x + 15 * s, y + 11 * s}, brush_.Get(), w);
    target->DrawLine({x + 15 * s, y + 11 * s}, {x + 18 * s, y + 14 * s}, brush_.Get(), w);
}
ID2D1SolidColorBrush *View::token_brush(uint32_t style) const {
    for (size_t i = 0; i < sizeof(kTokenColors) / sizeof(kTokenColors[0]); ++i)
        if (style & kTokenColors[i].style)
            return tokenBrushes_[i].Get();
    return nullptr;
}
void View::theme_token_brushes() {
    for (size_t i = 0; i < sizeof(kTokenColors) / sizeof(kTokenColors[0]); ++i)
        if (tokenBrushes_[i])
            tokenBrushes_[i]->SetColor(color(dark_ ? kTokenColors[i].dark : kTokenColors[i].light));
}
std::optional<D2D1_RECT_F> View::copy_button_rect(size_t index) {
    if (!doc_ || index >= doc_->blocks.size())
        return {};
    const auto &b = doc_->blocks[index];
    if (b.kind != Kind::Code || !group_first(index))
        return {};
    const auto &entry = layout(index);
    auto group = groupWidth_.find(b.source);
    float width = std::max(entry.width, group == groupWidth_.end() ? 0.f : group->second);
    float right = block_left(b) + width - 2 * zoom_; // 10 in from the frame's right edge.
    float top = heights_.prefix(index) + 24 - scrollY_ + 3 * zoom_;
    return D2D1::RectF(right - 52 * zoom_, top, right, top + 20 * zoom_);
}
std::optional<size_t> View::copy_button_at(float x, float y) {
    if (!doc_ || doc_->blocks.empty())
        return {};
    size_t index = heights_.locate(std::max(0.f, y + scrollY_ - 24));
    auto rect = copy_button_rect(index);
    if (!rect || x < rect->left || x >= rect->right || y < rect->top || y >= rect->bottom)
        return {};
    return index;
}
void View::draw_block(ID2D1RenderTarget *target, size_t index, Entry &entry, float y) {
    const auto &b = doc_->blocks[index];
    const auto p = ui::palette(dark_);
    theme_token_brushes();
    float x = block_left(b);
    if ((b.kind == Kind::Image || b.kind == Kind::Diagram) && !assets_.contains(index))
        request_asset(index);
    bool code = b.kind == Kind::Code || b.kind == Kind::Html;
    if (code) {
        bool first = group_first(index), last = group_last(index);
        float width = std::max(entry.width, groupWidth_[b.source]);
        auto box = D2D1::RectF(x - 10 * zoom_, y, x + width + 10 * zoom_, y + entry.height - (last ? 16 * zoom_ : 0));
        frame(target, box, first, last, color(p.code), color(p.border));
        if (first && b.kind == Kind::Code) {
            auto chip = copy_button_rect(index);
            if (chip) {
                bool copied = copiedSource_ != SIZE_MAX && b.source == copiedSource_;
                bool hot = copyHover_ && copy_button_at(mouseX_, mouseY_) == index;
                bool pressed = copyArmed_ == index;
                auto face = copied   ? mix(p.surface, p.code, .5f)
                            : pressed ? color(p.pressed)
                            : hot     ? color(p.hover)
                                      : mix(p.surface, p.code, .5f);
                auto rounded = D2D1::RoundedRect(*chip, 6 * zoom_, 6 * zoom_);
                brush_->SetColor(face);
                target->FillRoundedRectangle(rounded, brush_.Get());
                brush_->SetColor(color(p.border));
                target->DrawRoundedRectangle(rounded, brush_.Get(), 1.f);
                draw_text(target, copied ? L"已复制" : L"复制",
                          D2D1::RectF(chip->left, chip->top + 3 * zoom_, chip->right, chip->bottom),
                          11 * zoom_, copied ? color(p.accent) : color(p.muted), false,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            if (!b.language.empty()) {
                float labelRight = (chip ? chip->left : box.right - 12 * zoom_) - 8 * zoom_;
                draw_text(target, b.language,
                          D2D1::RectF(box.right - 220 * zoom_, y + 4 * zoom_, labelRight, y + 22 * zoom_),
                          11 * zoom_, color(p.subtle), false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
        }
    } else if (b.kind == Kind::TableRow) {
        bool first = group_first(index), last = group_last(index);
        auto box = D2D1::RectF(x, y, x + entry.width, y + entry.height - (last ? 16 * zoom_ : 0));
        frame(target, box, first, last, color(b.header ? p.code : p.surface), color(p.border));
        brush_->SetColor(color(p.border));
        for (size_t c = 1; c < entry.offsets.size(); ++c)
            target->DrawLine({x + entry.offsets[c], box.top}, {x + entry.offsets[c], box.bottom}, brush_.Get(), 1.f);
        if (!last)
            target->DrawLine({box.left, box.bottom - .5f}, {box.right, box.bottom - .5f}, brush_.Get(), 1.f);
    }
    if (b.kind == Kind::Rule) {
        fill(target, D2D1::RectF(x, y + 14 * zoom_, x + entry.width, y + 14 * zoom_ + std::max(1.f, zoom_)),
             color(p.border));
        return;
    }
    if (b.quote)
        fill(target, D2D1::RectF(x - 17 * zoom_, y, x - 14 * zoom_, y + entry.height - (b.joins_next ? 0 : 12 * zoom_)),
             color(p.thumb));
    if (!b.marker.empty())
        draw_text(target, b.marker, D2D1::RectF(x - 24 * zoom_, y, x - 2 * zoom_, y + 30 * zoom_), font_size(b),
                  color(b.marker == L"☑" ? p.accent : p.muted));
    if (b.kind == Kind::Heading && b.level <= 2 && !b.joins_next)
        fill(target, D2D1::RectF(x, y + entry.height - 10 * zoom_, x + entry.width,
                                 y + entry.height - 10 * zoom_ + std::max(1.f, zoom_)),
             color(p.border));
    auto asset = assets_.find(index);
    if (asset != assets_.end() && asset->second.status == Asset::Ready) {
        asset->second.use = ++clock_;
        if (asset->second.scene) {
            float fit = std::min(1.f, entry.width / std::max(1.f, asset->second.scene->width));
            draw_diagram(target, *asset->second.scene, x, y + 8 * zoom_, fit);
        } else if (asset->second.bitmap) {
            auto size = image_extent(asset->second.width, asset->second.height, b.image_width, b.image_height,
                                     entry.width, zoom_);
            float left = x + (b.text_align == 2   ? (entry.width - size.width) * .5f
                              : b.text_align == 3 ? entry.width - size.width
                                                  : 0);
            target->DrawBitmap(asset->second.bitmap.Get(), D2D1::RectF(left, y, left + size.width, y + size.height));
        }
        return;
    }
    bool placeholder = b.kind == Kind::Image || b.kind == Kind::Diagram;
    if (placeholder) {
        auto box = D2D1::RectF(x + entry.cardLeft, y, x + entry.cardLeft + entry.cardWidth, y + entry.height - 14 * zoom_);
        frame(target, box, true, true, color(p.code), color(p.border));
        picture_icon(target, box.left + 16 * zoom_, box.top + 14 * zoom_, 20 * zoom_, color(p.muted));
    }
    if (linkBrush_)
        linkBrush_->SetColor(color(p.accent));
    if (mutedBrush_)
        mutedBrush_->SetColor(color(p.muted));
    size_t selStart = std::min(selectFrom_, selectTo_), selEnd = std::max(selectFrom_, selectTo_);
    for (size_t c = 0; c < entry.layouts.size(); ++c) {
        auto origin = text_origin(index, entry, c, x, y);
        float tx = origin.x, ty = origin.y;
        auto *tl = entry.layouts[c].Get();
        const Text *text = b.kind == Kind::TableRow && c < b.cells.size() ? &b.cells[c] : &b.text;
        const size_t start = b.text_start + entry.textOffsets[c], end = start + text->value.size();
        if (!placeholder) {
            for (const auto &span : text->spans)
                if (span.style & Mono)
                    range_rects(target, tl, span.start, span.length, tx, ty, color(p.codeInline), 4 * zoom_);
            for (const auto &span : text->spans) {
                ComPtr<IDWriteInlineObject> typeset;
                if ((span.style & Formula) &&
                    (FAILED(tl->GetInlineObject(span.start, typeset.GetAddressOf(), nullptr)) || !typeset))
                    range_rects(target, tl, span.start, span.length, tx, ty, color(p.codeInline), 4 * zoom_);
            }
            if (selEnd > start && selStart < end)
                range_rects(target, tl, (uint32_t)(std::max(selStart, start) - start),
                            (uint32_t)(std::min(selEnd, end) - std::max(selStart, start)), tx, ty,
                            mix(p.accent, p.surface, dark_ ? .38f : .26f));
            for (auto [m, last] = matches_in(matches_, start, end); m < last; ++m) {
                const auto a = matches_[m].absolute, z = a + matches_[m].length;
                range_rects(target, tl, (uint32_t)(std::max(a, start) - start),
                            (uint32_t)(std::min(z, end) - std::max(a, start)), tx, ty,
                            rgb((int)m == activeMatch_ ? 0xffbd57 : 0xffe5a1, .8f));
            }
        }
        brush_->SetColor(b.quote ? color(p.muted) : foreground());
        *formulaInk_ = {target, b.quote ? color(p.muted) : foreground()};
        target->DrawTextLayout(D2D1::Point2F(tx, ty), tl, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        formulaInk_->target = nullptr;
    }
}
void View::render(ID2D1RenderTarget *target) {
    target->BeginDraw();
    target->Clear(background());
    if (!doc_ || doc_->blocks.empty()) {
        // Quiet start page: the product mark only, without instructional copy.
        auto p = ui::palette(dark_);
        float size = 56, cx = width_ / 2, cy = std::max(120.f, height_ * .42f);
        auto mark = D2D1::RoundedRect(D2D1::RectF(cx - size / 2, cy - size, cx + size / 2, cy), 14, 14);
        brush_->SetColor(color(p.accent));
        target->FillRoundedRectangle(mark, brush_.Get());
        draw_text(target, L"M", D2D1::RectF(cx - size / 2, cy - size + 6, cx + size / 2, cy), 30,
                  color(p.accentText), true, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(target, emptyTitle_.empty() ? L"KeepMD" : emptyTitle_,
                  D2D1::RectF(48, cy + 18, width_ - 48, cy + 60), 22, color(p.muted), true,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        size_t first = heights_.locate(std::max(0.f, scrollY_ - 20));
        // Lay out the visible blocks before drawing so a code block's widest visible line sizes its frame.
        std::vector<size_t> visible;
        float y = heights_.prefix(first) + 24 - scrollY_;
        for (size_t i = first; i < doc_->blocks.size() && y < height_ + 100; ++i) {
            y += layout(i).height;
            visible.push_back(i);
        }
        y = heights_.prefix(first) + 24 - scrollY_;
        for (size_t i : visible) {
            auto &entry = layout(i);
            draw_block(target, i, entry, y);
            y += entry.height;
        }
        size_t last = visible.empty() ? first : visible.back();
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
    const auto block = doc_->blocks[index];
    if (block.kind == Kind::Image) {
        if (auto url = remote_image_url(block.target)) {
            Asset asset;
            auto &state = remoteAssets_[url->url];
            if ((state.status == Asset::NotLoaded && (!state.cacheChecked || remoteEnabled_)) ||
                (state.status == Asset::Ready && !state.image)) {
                queue_remote_image(url->url);
                pump_remote_images();
            }
            asset.status = Asset::NotLoaded;
            if (auto cached = remoteAssets_.find(url->url); cached != remoteAssets_.end()) {
                auto &remote = cached->second;
                remote.use = ++clock_;
                asset.status = remote.status;
                asset.error = remote.error;
                if (remote.image && !remote.image->pixels.empty() && target_) {
                    auto &image = *remote.image;
                    auto props = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    if (SUCCEEDED(target_->CreateBitmap(D2D1::SizeU(image.width, image.height),
                        image.pixels.data(), image.width * 4, props, asset.bitmap.GetAddressOf()))) {
                        asset.width = layout_extent(image.width, image.density);
                        asset.height = layout_extent(image.height, image.density);
                        asset.bytes = image.pixels.size();
                        assetBytes_ += asset.bytes;
                    } else {
                        asset.status = Asset::Failed;
                        asset.error = L"图片显示资源不可用";
                    }
                }
            }
            asset.use = ++clock_;
            assets_.emplace(index, std::move(asset));
            return;
        }
    }
    if (!worker_)
        worker_ = std::make_unique<Worker>();
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
                    auto decodedPath = decode_url_path(block.target);
                    auto imagePath = linked_path(base, decodedPath);
                    auto nativePath = imagePath.wstring();
                    bool networkPath = nativePath.size() >= 2 &&
                                       (nativePath[0] == L'/' || nativePath[0] == L'\\') &&
                                       (nativePath[1] == L'/' || nativePath[1] == L'\\');
                    if (decodedPath.find(L"://") != std::wstring::npos || decodedPath.starts_with(L"data:") ||
                        networkPath)
                        result.error = L"远程或内嵌图片暂未加载";
                    else {
                        auto image = decode_image_file(imagePath, block.image_width, block.image_height);
                        result.width = image.width;
                        result.height = image.height;
                        result.density = image.density;
                        result.pixels = std::move(image.pixels);
                        result.error = std::move(image.error);
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
    std::vector<RemoteResult> remoteResults;
    {
        std::lock_guard lock(resultsMutex_);
        remoteResults.swap(remoteResults_);
    }
    for (auto &result : remoteResults) {
        auto found = remoteAssets_.find(result.url);
        if (result.generation != remoteGeneration_ || found == remoteAssets_.end())
            continue;
        if (remoteActive_) --remoteActive_;
        auto &remote = found->second;
        if (remote.image) remoteBytes_ -= remote.image->pixels.size();
        remote.image = std::move(result.image);
        remote.error = remote.image->error;
        remote.status = !remote.image->pixels.empty() ? Asset::Ready : remote.error.empty() ? Asset::NotLoaded : Asset::Failed;
        remote.cacheChecked = true;
        remote.use = ++clock_;
        remoteBytes_ += remote.image->pixels.size();
        if (remote.status == Asset::NotLoaded && remoteEnabled_ && !result.allowedDownload)
            queue_remote_image(result.url);
        invalidate_remote_images(result.url);
    }
    trim_remote_assets();
    pump_remote_images();
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
        asset.width = layout_extent(result.width, result.density);
        asset.height = layout_extent(result.height, result.density);
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
    if (remoteActive_ || !remoteQueue_.empty()) return true;
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
        fill(target, rect, color(ui::palette(dark_).code));
        brush_->SetColor(color(ui::palette(dark_).border));
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
        brush_->SetColor(color(ui::palette(dark_).muted));
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
                                            : color(ui::palette(dark_).selected);
        auto stroke =
            node.style.hasStroke ? rgb(node.style.stroke.rgb, node.style.stroke.alpha) : color(ui::palette(dark_).accent);
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
    float left = block_left(b);
    float top = heights_.prefix(i) + 24 - scrollY_;
    if (entry.layouts.empty())
        return b.text_start;
    size_t cell = 0;
    for (size_t c = 1; c < entry.offsets.size(); ++c)
        if (x - left >= entry.offsets[c])
            cell = c;
    auto origin = text_origin(i, entry, cell, left, top);
    float tx = origin.x, ty = origin.y;
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
    if (b.kind == Kind::Image && remote_image_url(b.target)) {
        auto asset = assets_.find(i);
        if (asset == assets_.end() || asset->second.status != Asset::Ready) return {};
    }
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
// Whole fenced block (or raw HTML block) source, joined from its layout line blocks.
std::wstring View::code_group_text(size_t index) const {
    if (!doc_ || index >= doc_->blocks.size())
        return {};
    const auto &block = doc_->blocks[index];
    if (block.kind != Kind::Code && block.kind != Kind::Html)
        return {};
    size_t first = index;
    while (first > 0 && same_group(first - 1, first))
        --first;
    std::wstring result;
    for (size_t i = first; i < doc_->blocks.size() && same_group(first, i); ++i) {
        result += doc_->blocks[i].text.value;
        if (!doc_->blocks[i].joins_next)
            result += L'\n';
    }
    return result;
}
std::wstring View::source_at(float, float y) const {
    if (!doc_ || doc_->blocks.empty())
        return {};
    size_t index = heights_.locate(std::max(0.f, y + scrollY_ - 24));
    const auto &block = doc_->blocks[index];
    if (block.kind == Kind::Diagram)
        return block.text.value;
    return code_group_text(index);
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
    if (auto chip = copy_button_at(x, y)) {
        copyArmed_ = *chip;
        dragging_ = false;
        moved_ = false;
        SetCapture(hwnd_);
        invalidate();
        return;
    }
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
    mouseX_ = x;
    mouseY_ = y;
    bool hover = copy_button_at(x, y).has_value();
    if (hover != copyHover_) {
        copyHover_ = hover;
        invalidate();
    }
    if (copyArmed_ != SIZE_MAX)
        invalidate();
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
    if (copyArmed_ != SIZE_MAX) {
        size_t armed = copyArmed_;
        copyArmed_ = SIZE_MAX;
        ReleaseCapture();
        auto inside = copy_button_at(x, y);
        if (inside && *inside == armed) {
            auto text = code_group_text(armed);
            if (!text.empty() && on_copy) {
                on_copy(text);
                copiedSource_ = doc_->blocks[armed].source;
                copiedAt_ = std::chrono::steady_clock::now();
                SetTimer(hwnd_, kCopyFeedbackTimer, 1500, nullptr);
            }
        }
        invalidate();
        return;
    }
    if (!dragging_)
        return;
    dragging_ = false;
    ReleaseCapture();
    if (!moved_ && !clickedLink_.empty() && on_link && link_at(x, y) == clickedLink_)
        on_link(clickedLink_);
    else
        selectTo_ = hit(x, y);
    clickedLink_.clear();
    invalidate();
}
void View::expire_copy_feedback() {
    if (copiedSource_ == SIZE_MAX)
        return;
    if (std::chrono::steady_clock::now() - copiedAt_ < std::chrono::milliseconds(1300))
        return;
    copiedSource_ = SIZE_MAX;
    KillTimer(hwnd_, kCopyFeedbackTimer);
    invalidate();
}
std::pair<std::wstring, bool> View::image_detail(size_t index) const {
    auto asset = assets_.find(index);
    auto url = remote_image_url(doc_->blocks[index].target);
    if (url) {
        if (asset != assets_.end() && asset->second.status == Asset::Pending)
            return {L"正在加载网络图片 · " + host_of(url->url), false};
        if (asset != assets_.end() && asset->second.status == Asset::Failed)
            return {asset->second.error + L" · 重新开启网络图片可重试", false};
        return {L"网络图片 · " + host_of(url->url) + L" · 从工具栏开启加载", false};
    }
    if (asset == assets_.end() || asset->second.status == Asset::Pending)
        return {L"加载中…", false};
    return {asset->second.error, false};
}
void View::trim_remote_assets() {
    size_t count = 0;
    for (const auto &[url, asset] : remoteAssets_)
        if (asset.image && !asset.image->pixels.empty()) ++count;
    while (count > 48 || remoteBytes_ > 24 * 1024 * 1024) {
        auto victim = remoteAssets_.end();
        for (auto it = remoteAssets_.begin(); it != remoteAssets_.end(); ++it) {
            if (!it->second.image || it->second.image->pixels.empty() || it->second.status == Asset::Pending)
                continue;
            if (victim == remoteAssets_.end() || it->second.use < victim->second.use) victim = it;
        }
        if (victim == remoteAssets_.end()) break;
        remoteBytes_ -= victim->second.image->pixels.size();
        victim->second.image.reset(); // Keep the Ready marker: an off-screen batch must not loop forever.
        --count;
    }
}
void View::invalidate_remote_images(std::wstring_view target) {
    if (!doc_) return;
    for (size_t i = 0; i < doc_->blocks.size(); ++i) {
        const auto &block = doc_->blocks[i];
        if (block.kind != Kind::Image) continue;
        auto url = remote_image_url(block.target);
        if (!url || (!target.empty() && url->url != target)) continue;
        if (auto asset = assets_.find(i); asset != assets_.end()) {
            assetBytes_ -= asset->second.bytes;
            assets_.erase(asset);
        }
        if (auto entry = cache_.find(i); entry != cache_.end()) {
            cacheChars_ -= entry->second.chars;
            cache_.erase(entry);
        }
    }
}
void View::queue_remote_image(const std::wstring &url) {
    auto &entry = remoteAssets_[url];
    if (entry.status == Asset::Pending || (entry.status == Asset::Ready && entry.image)) return;
    if (entry.image) remoteBytes_ -= entry.image->pixels.size();
    entry.image.reset();
    entry.error.clear();
    entry.status = Asset::Pending;
    remoteQueue_.push_back(url);
}
void View::queue_document_images() {
    if (!doc_ || !remoteEnabled_) return;
    for (const auto &block : doc_->blocks) {
        if (block.kind != Kind::Image) continue;
        auto url = remote_image_url(block.target);
        if (!url) continue;
        auto &entry = remoteAssets_[url->url];
        if (entry.status == Asset::NotLoaded) queue_remote_image(url->url);
    }
    pump_remote_images();
}
void View::pump_remote_images() {
    while (remoteActive_ < remoteWorkers_.size() && !remoteQueue_.empty()) {
        auto url = remote_image_url(remoteQueue_.front());
        remoteQueue_.pop_front();
        if (!url) continue;
        size_t slot = remoteWorkerIndex_++ % remoteWorkers_.size();
        if (!remoteWorkers_[slot]) remoteWorkers_[slot] = std::make_unique<Worker>();
        auto epoch = remoteGeneration_;
        auto cancel = remoteCancel_;
        bool allowed = remoteEnabled_;
        auto directory = imageCache_;
        bool queued = remoteWorkers_[slot]->push([this, url = *url, epoch, cancel, allowed, directory] {
            if (cancel->load()) return;
            CoScope co;
            auto image = std::make_shared<ImagePixels>();
            try {
                *image = cached_remote_image(url, allowed, directory, *cancel);
            } catch (...) {
                image->pixels.clear();
                image->error = L"网络图片处理失败";
            }
            if (cancel->load()) return;
            {
                std::lock_guard lock(resultsMutex_);
                remoteResults_.push_back({epoch, url.url, std::move(image), allowed});
            }
            PostMessageW(hwnd_, WM_ASSET_READY, 0, 0);
        });
        if (queued) ++remoteActive_;
        else { remoteQueue_.push_front(url->url); break; }
    }
}
void View::set_remote_images_enabled(bool enabled) {
    if (remoteEnabled_ == enabled) return;
    remoteEnabled_ = enabled;
    if (!enabled) {
        remoteCancel_->store(true);
        remoteCancel_ = std::make_shared<std::atomic_bool>(false);
        ++remoteGeneration_;
        for (auto &worker : remoteWorkers_) if (worker) worker->clear();
        remoteQueue_.clear();
        remoteActive_ = 0;
        for (auto &[url, entry] : remoteAssets_)
            if (entry.status == Asset::Pending) {
                entry.status = Asset::NotLoaded;
                entry.cacheChecked = false;
            }
    } else {
        for (auto &[url, entry] : remoteAssets_)
            if (entry.status == Asset::Failed) entry.status = Asset::NotLoaded;
        queue_document_images();
    }
    invalidate_remote_images();
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
