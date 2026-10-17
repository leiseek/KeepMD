#include "math_formula.h"
#include "text.h"
#include <algorithm>
#include <cmath>
#include <dwrite_3.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <windows.h>
#include <wrl/client.h>

// MicroTeX is pinned upstream code built at its own warning level; its headers are not held to KeepMD's.
// rpcndr.h defines `small` as a macro, which MicroTeX uses as a parameter name. Its header templates narrow
// int to char and wchar_t (C4244) when instantiated, which happens at the end of this file, past any
// push/pop region, so that one warning is off for the file; KeepMD's own conversions here are explicit.
#pragma warning(disable : 4244)
#pragma warning(push, 0)
#pragma push_macro("small")
#undef small
#include "core/core.h"
#include "core/formula.h"
#include "fonts/fonts.h"
#include "graphic/graphic.h"
#include "latex.h"
#pragma pop_macro("small")
#pragma warning(pop)

namespace keepmd {
namespace {
using Microsoft::WRL::ComPtr;

// MicroTeX keeps parser and font tables in statics, so typesetting and drawing are serialized.
std::mutex &lock() {
    static std::mutex mutex;
    return mutex;
}

IDWriteFactory5 *dwrite() {
    static ComPtr<IDWriteFactory5> factory = [] {
        ComPtr<IDWriteFactory5> f;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                            reinterpret_cast<IUnknown **>(f.GetAddressOf()));
        return f;
    }();
    return factory.Get();
}

// The embedded Computer Modern faces, keyed by file stem (math_fonts.rc names each resource after its file).
ComPtr<IDWriteFontFace> embedded_face(const std::string &path) {
    static std::map<std::wstring, ComPtr<IDWriteFontFace>> faces;
    static ComPtr<IDWriteInMemoryFontFileLoader> loader;
    auto name = std::filesystem::path(path).stem().wstring();
    for (auto &c : name)
        c = (wchar_t)towupper(c);
    if (auto it = faces.find(name); it != faces.end())
        return it->second;
    auto *factory = dwrite();
    if (!factory)
        return nullptr;
    if (!loader && SUCCEEDED(factory->CreateInMemoryFontFileLoader(loader.GetAddressOf())))
        factory->RegisterFontFileLoader(loader.Get());
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, name.c_str(), RT_RCDATA);
    HGLOBAL handle = resource ? LoadResource(module, resource) : nullptr;
    const void *data = handle ? LockResource(handle) : nullptr;
    ComPtr<IDWriteFontFile> file;
    ComPtr<IDWriteFontFace> face;
    // Resource memory lives as long as the module, so DirectWrite may reference it without a copy.
    if (data && loader &&
        SUCCEEDED(loader->CreateInMemoryFontFileReference(factory, data, SizeofResource(module, resource),
                                                          nullptr, file.GetAddressOf())))
        factory->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, file.GetAddressOf(), 0,
                                DWRITE_FONT_SIMULATIONS_NONE, face.GetAddressOf());
    faces[name] = face;
    return face;
}

// Fonts MicroTeX names that KeepMD does not ship, because their licence or provenance is unclear (see
// THIRD_PARTY_NOTICES.md). Their few glyphs have standard Unicode equivalents, drawn from a Windows face; the
// layout still uses MicroTeX's compiled metrics, so only the glyph shape differs.
struct Substitute {
    const char *stem;
    const wchar_t *family;
    DWRITE_FONT_WEIGHT weight;
    DWRITE_FONT_STYLE style;
    bool special;
};
constexpr Substitute kSubstitutes[] = {
    {"r10", L"Cambria", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, false},
    {"i10", L"Cambria", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, false},
    {"bx10", L"Cambria", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, false},
    {"bi10", L"Cambria", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC, false},
    {"ss10", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, false},
    {"si10", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, false},
    {"sb10", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, false},
    {"sbi10", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC, false},
    {"tt10", L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, false},
    {"special", L"Cambria Math", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, true},
};
// JLaTeXMath's Latin text fonts: guillemets, per mille and per ten thousand, and the dangerous-bend sign.
constexpr std::pair<wchar_t, wchar_t> kLatinGlyphs[] = {{33, L'\u00AB'},  {34, L'\u2039'}, {35, L'\u203A'},
                                                        {36, L'\u00BB'},  {37, L'\u2030'}, {38, L'\u2031'},
                                                        {126, L'\u2621'}, {127, L'\u2621'}};
// special.ttf: the TeX logo letters, euro, micro and parallelogram. Its Android robot has no stand-in and is
// left blank.
constexpr std::pair<wchar_t, wchar_t> kSpecialGlyphs[] = {{66, L'T'},      {67, L'E'},      {68, L'X'},
                                                          {69, L'\u20AC'}, {70, L'\u25B1'}, {101, L'\u20AC'},
                                                          {109, L'\u00B5'}};

const Substitute *substitute_for(const std::string &path) {
    auto stem = std::filesystem::path(path).stem().string();
    for (const auto &entry : kSubstitutes)
        if (stem == entry.stem)
            return &entry;
    return nullptr;
}

// A Windows face with `c` for a substituted glyph: the preferred family, then math and symbol fallbacks.
ComPtr<IDWriteFontFace> system_face(const Substitute &font, wchar_t c) {
    static std::map<std::pair<const Substitute *, wchar_t>, ComPtr<IDWriteFontFace>> faces;
    if (auto it = faces.find({&font, c}); it != faces.end())
        return it->second;
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteFontFace> face;
    if (auto *factory = dwrite(); factory && SUCCEEDED(factory->GetSystemFontCollection(collection.GetAddressOf())))
        for (const wchar_t *family : {font.family, L"Cambria Math", L"Segoe UI Symbol", L"Segoe UI"}) {
            UINT32 index = 0;
            BOOL exists = FALSE, has = FALSE;
            ComPtr<IDWriteFontFamily> found;
            ComPtr<IDWriteFont> match;
            if (SUCCEEDED(collection->FindFamilyName(family, &index, &exists)) && exists &&
                SUCCEEDED(collection->GetFontFamily(index, found.GetAddressOf())) &&
                SUCCEEDED(found->GetFirstMatchingFont(font.weight, DWRITE_FONT_STRETCH_NORMAL, font.style,
                                                      match.GetAddressOf())) &&
                SUCCEEDED(match->HasCharacter(c, &has)) && has &&
                SUCCEEDED(match->CreateFontFace(face.GetAddressOf())))
                break;
            face.Reset();
        }
    faces[{&font, c}] = face;
    return face;
}

// A MicroTeX font: an embedded Computer Modern face (glyphs by code point), a substitute for a font KeepMD does
// not ship, or a system family used for \text{...}, which DirectWrite lays out with fallback so Chinese text
// inside a formula still renders.
class Font final : public tex::Font {
  public:
    ComPtr<IDWriteFontFace> face;
    const Substitute *substitute = nullptr;
    std::wstring family;
    int style = tex::PLAIN;
    float size = 0;

    float getSize() const override {
        return size;
    }
    tex::sptr<tex::Font> deriveFont(int s) const override {
        auto derived = std::make_shared<Font>(*this);
        derived->style = s;
        return derived;
    }
    bool operator==(const tex::Font &other) const override {
        auto &f = static_cast<const Font &>(other);
        return face.Get() == f.face.Get() && substitute == f.substitute && family == f.family && style == f.style &&
               size == f.size;
    }
    bool operator!=(const tex::Font &other) const override {
        return !(*this == other);
    }
    // The face and glyph that draw code point `c`; false when this font has no glyph to draw for it.
    bool glyph(wchar_t c, ComPtr<IDWriteFontFace> &out, UINT16 &index) const {
        UINT32 code = c;
        if (substitute) {
            using Table = std::span<const std::pair<wchar_t, wchar_t>>;
            const Table table = substitute->special ? Table(kSpecialGlyphs) : Table(kLatinGlyphs);
            auto found = std::find_if(table.begin(), table.end(), [c](const auto &pair) { return pair.first == c; });
            if (found == table.end())
                return false;
            code = found->second;
            out = system_face(*substitute, found->second);
        } else
            out = face;
        if (!out)
            return false;
        index = 0;
        out->GetGlyphIndices(&code, 1, &index);
        if (!index && !substitute && c < 0x100) {
            // These Computer Modern builds carry a symbol cmap that places code points at U+F000 + c.
            code = 0xF000 + c;
            out->GetGlyphIndices(&code, 1, &index);
        }
        return index != 0;
    }
    ComPtr<IDWriteTextLayout> layout(const std::wstring &text) const {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> result;
        auto *factory = dwrite();
        if (factory &&
            SUCCEEDED(factory->CreateTextFormat(
                family.c_str(), nullptr, style & tex::BOLD ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                style & tex::ITALIC ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.GetAddressOf()))) {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            factory->CreateTextLayout(text.data(), (UINT32)text.size(), format.Get(), 1e6f, 1e6f,
                                      result.GetAddressOf());
        }
        return result;
    }
};

float baseline_of(IDWriteTextLayout *layout) {
    DWRITE_LINE_METRICS line{};
    UINT32 count = 0;
    layout->GetLineMetrics(&line, 1, &count);
    return count ? line.baseline : 0;
}

class Graphics final : public tex::Graphics2D {
  public:
    // Draws in a space whose origin is `offset` within the target's current one; the target's own transform is
    // restored when drawing ends.
    Graphics(ID2D1RenderTarget *target, D2D1_POINT_2F offset) : target_(target) {
        target_->GetTransform(&restore_);
        base_ = D2D1::Matrix3x2F::Translation(offset.x, offset.y) * restore_;
        target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.GetAddressOf());
        apply();
    }
    ~Graphics() {
        target_->SetTransform(restore_);
    }
    bool ready() const {
        return brush_ != nullptr;
    }

    void setColor(tex::color c) override {
        color_ = c;
        brush_->SetColor(D2D1::ColorF(tex::color_r(c) / 255.f, tex::color_g(c) / 255.f, tex::color_b(c) / 255.f,
                                      tex::color_a(c) / 255.f));
    }
    tex::color getColor() const override {
        return color_;
    }
    void setStroke(const tex::Stroke &s) override {
        stroke_ = s;
        strokeStyle_.Reset();
    }
    const tex::Stroke &getStroke() const override {
        return stroke_;
    }
    void setStrokeWidth(float w) override {
        stroke_.lineWidth = w;
    }
    const tex::Font *getFont() const override {
        return font_;
    }
    void setFont(const tex::Font *font) override {
        font_ = static_cast<const Font *>(font);
    }

    void translate(float dx, float dy) override {
        local_ = D2D1::Matrix3x2F::Translation(dx, dy) * local_;
        apply();
    }
    void scale(float x, float y) override {
        sx_ *= x;
        sy_ *= y;
        local_ = D2D1::Matrix3x2F::Scale(x, y) * local_;
        apply();
    }
    void rotate(float angle) override {
        local_ = D2D1::Matrix3x2F::Rotation(degrees(angle)) * local_;
        apply();
    }
    void rotate(float angle, float px, float py) override {
        local_ = D2D1::Matrix3x2F::Rotation(degrees(angle), D2D1::Point2F(px, py)) * local_;
        apply();
    }
    void reset() override {
        local_ = D2D1::Matrix3x2F::Identity();
        sx_ = sy_ = 1;
        apply();
    }
    float sx() const override {
        return sx_;
    }
    float sy() const override {
        return sy_;
    }

    void drawChar(wchar_t c, float x, float y) override {
        if (!font_)
            return;
        if (!font_->face && !font_->substitute) {
            drawText(std::wstring(1, c), x, y);
            return;
        }
        ComPtr<IDWriteFontFace> face;
        UINT16 glyph = 0;
        if (!font_->glyph(c, face, glyph))
            return;
        FLOAT advance = 0;
        DWRITE_GLYPH_RUN run{face.Get(), font_->size, 1, &glyph, &advance, nullptr, FALSE, 0};
        target_->DrawGlyphRun(D2D1::Point2F(x, y), &run, brush_.Get());
    }
    void drawText(const std::wstring &text, float x, float y) override {
        if (!font_)
            return;
        if (font_->face || font_->substitute) {
            for (wchar_t c : text) {
                drawChar(c, x, y);
                x += advance(c);
            }
            return;
        }
        if (auto layout = font_->layout(text))
            target_->DrawTextLayout(D2D1::Point2F(x, y - baseline_of(layout.Get())), layout.Get(), brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    void drawLine(float x1, float y1, float x2, float y2) override {
        target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(), stroke_.lineWidth, style());
    }
    void drawRect(float x, float y, float w, float h) override {
        target_->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), brush_.Get(), stroke_.lineWidth, style());
    }
    void fillRect(float x, float y, float w, float h) override {
        target_->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush_.Get());
    }
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry) override {
        target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry), brush_.Get(),
                                      stroke_.lineWidth, style());
    }
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry) override {
        target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry), brush_.Get());
    }

  private:
    ID2D1RenderTarget *target_;
    D2D1::Matrix3x2F restore_, base_, local_ = D2D1::Matrix3x2F::Identity();
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> strokeStyle_;
    tex::color color_ = tex::black;
    tex::Stroke stroke_;
    const Font *font_ = nullptr;
    float sx_ = 1, sy_ = 1;

    static float degrees(float radians) {
        return radians * 180.f / 3.14159265358979f;
    }
    void apply() {
        target_->SetTransform(local_ * base_);
    }
    float advance(wchar_t c) const {
        ComPtr<IDWriteFontFace> face;
        UINT16 glyph = 0;
        if (!font_->glyph(c, face, glyph))
            return 0;
        DWRITE_FONT_METRICS font{};
        DWRITE_GLYPH_METRICS metrics{};
        face->GetMetrics(&font);
        face->GetDesignGlyphMetrics(&glyph, 1, &metrics, FALSE);
        return font.designUnitsPerEm ? metrics.advanceWidth * font_->size / font.designUnitsPerEm : 0;
    }
    ID2D1StrokeStyle *style() {
        if (!strokeStyle_) {
            ComPtr<ID2D1Factory> factory;
            target_->GetFactory(factory.GetAddressOf());
            auto cap = stroke_.cap == tex::CAP_ROUND    ? D2D1_CAP_STYLE_ROUND
                       : stroke_.cap == tex::CAP_SQUARE ? D2D1_CAP_STYLE_SQUARE
                                                        : D2D1_CAP_STYLE_FLAT;
            auto join = stroke_.join == tex::JOIN_ROUND   ? D2D1_LINE_JOIN_ROUND
                        : stroke_.join == tex::JOIN_BEVEL ? D2D1_LINE_JOIN_BEVEL
                                                          : D2D1_LINE_JOIN_MITER;
            factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(cap, cap, cap, join,
                                                                   std::max(1.f, stroke_.miterLimit)),
                                       nullptr, 0, strokeStyle_.GetAddressOf());
        }
        return strokeStyle_.Get();
    }
};

class TextLayout final : public tex::TextLayout {
  public:
    TextLayout(std::wstring text, std::shared_ptr<Font> font) : text_(std::move(text)), font_(std::move(font)) {}
    void getBounds(tex::Rect &bounds) override {
        bounds = {};
        if (auto layout = font_->layout(text_)) {
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            bounds = tex::Rect(0, -baseline_of(layout.Get()), metrics.widthIncludingTrailingWhitespace,
                               metrics.height);
        }
    }
    void draw(tex::Graphics2D &g2, float x, float y) override {
        auto previous = g2.getFont();
        g2.setFont(font_.get());
        g2.drawText(text_, x, y);
        g2.setFont(previous);
    }

  private:
    std::wstring text_;
    std::shared_ptr<Font> font_;
};

void ensure_initialized() {
    static bool done = false;
    if (done)
        return;
    tex::LaTeX::init();
    done = true;
}
} // namespace

struct MathFormula::Impl {
    tex::sptr<tex::Box> box;
};

MathFormula::MathFormula() : p_(std::make_unique<Impl>()) {}
MathFormula::~MathFormula() = default;

std::shared_ptr<MathFormula> MathFormula::create(const std::wstring &source, bool display, float size,
                                                 std::wstring *error) {
    auto fail = [&](std::wstring reason) -> std::shared_ptr<MathFormula> {
        if (error)
            *error = std::move(reason);
        return nullptr;
    };
    if (source.size() > kMaxSource)
        return fail(L"公式超过 " + std::to_wstring(kMaxSource / 1024) + L" KiB，按源码显示");
    if (size <= 0)
        return fail(L"公式字号无效");
    std::lock_guard guard(lock());
    try {
        ensure_initialized();
        tex::Formula formula(source);
        tex::sptr<tex::Atom> root = formula._root;
        if (!root)
            return fail(L"空公式");
        tex::sptr<tex::TeXFont> font = std::make_shared<tex::DefaultTeXFont>(size);
        tex::Environment environment(display ? tex::TexStyle::display : tex::TexStyle::text, font);
        auto box = root->createBox(environment);
        if (!box)
            return fail(L"公式无法排版");
        std::shared_ptr<MathFormula> result(new MathFormula());
        result->p_->box = box;
        result->size_ = size;
        result->width = std::max(0.f, box->_width * size);
        result->ascent = std::max(0.f, box->_height * size);
        result->descent = std::max(0.f, box->_depth * size);
        return result;
    } catch (const std::exception &e) {
        return fail(L"无法排版公式：" + keep::wide(e.what()));
    }
}

void MathFormula::draw(ID2D1RenderTarget *target, float x, float y, D2D1_COLOR_F color) const {
    if (!target || !p_->box)
        return;
    std::lock_guard guard(lock());
    Graphics g(target, D2D1::Point2F(x, y));
    if (!g.ready())
        return;
    g.scale(size_, size_);
    g.setColor(tex::argb((int)std::lround(color.a * 255), (int)std::lround(color.r * 255),
                         (int)std::lround(color.g * 255), (int)std::lround(color.b * 255)));
    try {
        p_->box->draw(g, 0, p_->box->_height);
    } catch (const std::exception &) {
        // A box that failed mid-draw leaves only what it already painted; layout metrics are unaffected.
    }
}

} // namespace keepmd

// MicroTeX resolves fonts through these two factories, which each platform backend provides.
tex::Font *tex::Font::create(const std::string &file, float size) {
    auto font = new keepmd::Font();
    font->face = keepmd::embedded_face(file);
    if (!font->face)
        font->substitute = keepmd::substitute_for(file);
    font->size = size;
    if (!font->face && !font->substitute) {
        delete font;
        throw tex::ex_invalid_state("embedded math font missing: " + file);
    }
    return font;
}

tex::sptr<tex::Font> tex::Font::_create(const std::string &name, int style, float size) {
    auto font = std::make_shared<keepmd::Font>();
    font->family = name == "Serif" ? L"Times New Roman" : name == "SansSerif" ? L"Segoe UI" : keep::wide(name);
    font->style = style;
    font->size = size;
    return font;
}

tex::sptr<tex::TextLayout> tex::TextLayout::create(const std::wstring &src, const tex::sptr<tex::Font> &font) {
    return std::make_shared<keepmd::TextLayout>(src, std::static_pointer_cast<keepmd::Font>(font));
}
