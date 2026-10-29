#include "diagram.h"
#include "document.h"
#include "file_io.h"
#include "html_image.h"
#include "image_cache.h"
#include "image_decode.h"
#include "math_formula.h"
#include "paths.h"
#include "remote_image.h"
#include "prompt_model.h"
#include "settings.h"
#include "shell_integration.h"
#include "syntax.h"
#include "visual_markdown.h"
#include <cmath>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <windows.h>
// The test client links both products and the shared foundation; the products never link each other.
using namespace keep;
using namespace keepmd;
using namespace keepprompt;
int failures = 0, checks = 0;
void check(bool value, const char *name) {
    ++checks;
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

// Ink a formula leaves on a transparent bitmap, and the box it covers relative to the formula's top-left.
struct Ink {
    size_t pixels = 0, red = 0;
    bool restored = false; // The target's transform is unchanged after drawing.
    float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
};
Ink draw_formula(const MathFormula &formula) {
    using Microsoft::WRL::ComPtr;
    constexpr UINT W = 600, H = 300;
    constexpr float X = 50, Y = 50;
    Ink ink;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1RenderTarget> target;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.GetAddressOf()));
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    if (!wic || !d2d ||
        FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.GetAddressOf())) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(),
                                                target.GetAddressOf())))
        return ink;
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    // Draw under a caller transform, as the reader does for scrolled text, and offset back to the origin.
    const auto callerTransform = D2D1::Matrix3x2F::Translation(7, 3);
    target->SetTransform(callerTransform);
    formula.draw(target.Get(), X - 7, Y - 3, D2D1::ColorF(0, 0, 0));
    D2D1::Matrix3x2F after;
    target->GetTransform(&after);
    ink.restored = after.dx == callerTransform.dx && after.dy == callerTransform.dy && after._11 == 1 &&
                   after._22 == 1 && after._12 == 0 && after._21 == 0;
    target->EndDraw();
    WICRect all{0, 0, (INT)W, (INT)H};
    std::vector<BYTE> pixels(W * H * 4);
    bitmap->CopyPixels(&all, W * 4, (UINT)pixels.size(), pixels.data());
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x)
            if (const BYTE *px = &pixels[(y * W + x) * 4]; px[3] > 64) {
                ++ink.pixels;
                if (px[2] > 160 && px[1] < 80 && px[0] < 80) // Premultiplied BGRA: strongly red.
                    ++ink.red;
                ink.left = std::min(ink.left, x - X);
                ink.right = std::max(ink.right, x + 1 - X);
                ink.top = std::min(ink.top, y - Y);
                ink.bottom = std::max(ink.bottom, y + 1 - Y);
            }
    return ink;
}
// The ink stays inside the typeset box, allowing for antialiasing and side bearings.
bool ink_fits(const MathFormula &formula, const Ink &ink) {
    constexpr float slack = 3;
    return ink.pixels && ink.left >= -slack && ink.top >= -slack && ink.right <= formula.width + slack &&
           ink.bottom <= formula.ascent + formula.descent + slack;
}
int main() {
    // WIC decoding in the image failure checks needs COM on this thread.
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto doc = parse_document("# 标题\n\nHello **bold** *italic* ~~gone~~ `code` &amp; &#x1F600;.\n\n- "
                              "first\n- [x] done\n\n[reference][later]\n\n[later]: https://example.com\n\n| "
                              "A | B |\n|:--|--:|\n|one|two|\n\n```cpp\nint x;\nreturn x;\n```\n");
    check(doc->error.empty(), "markdown parse succeeds");
    check(doc->headings.size() == 1 && doc->headings[0].text == L"标题", "CJK heading");
    check(doc->plain_text().find(L"Hello bold italic gone code & 😀.") != std::wstring::npos,
          "inline styles and entities preserve text");
    bool bold = false, italic = false, strike = false, mono = false, ref = false, task = false, table = false;
    int code = 0;
    for (const auto &b : doc->blocks) {
        for (const auto &s : b.text.spans) {
            bold |= (s.style & Bold) != 0;
            italic |= (s.style & Italic) != 0;
            strike |= (s.style & Strike) != 0;
            mono |= (s.style & Mono) != 0;
            ref |= s.link == L"https://example.com";
        }
        task |= b.marker == L"☑";
        table |= b.kind == Kind::TableRow && b.cells.size() == 2;
        if (b.kind == Kind::Code)
            ++code;
    }
    check(bold && italic && strike && mono, "style spans");
    check(ref, "forward link references");
    check(task, "task list");
    check(table, "GFM tables");
    check(code == 2, "code lines remain individually layoutable");
    auto image = parse_document("before ![测试](image%20name.png) after\n");
    check(image->blocks.size() == 3, "inline image keeps surrounding text");
    check(image->plain_text().find(L"before") != std::wstring::npos &&
              image->plain_text().find(L"after") != std::wstring::npos,
          "inline image no text loss");
    auto headings = parse_document("# Repeat\n\n# Repeat\n");
    const std::string htmlSource = "<img src=\"Ghidra/Features/Base/src/main/resources/images/GHIDRA_3.png\" "
                                   "width=\"400\">\n\n# Ghidra\n";
    auto htmlDoc = parse_document(htmlSource);
    check(htmlDoc->source == htmlSource && htmlDoc->blocks.size() == 2 &&
              htmlDoc->blocks[0].kind == Kind::Image && htmlDoc->blocks[0].image_width == 400 &&
              htmlDoc->blocks[0].target == L"Ghidra/Features/Base/src/main/resources/images/GHIDRA_3.png" &&
              htmlDoc->headings.size() == 1,
          "HTML image renders without altering original source or following heading");
    auto tag = parse_html_image(L" \n<IMG loading=lazy SRC='图片 &amp; 图.png'\nWIDTH=120 height='60' "
                                L"alt=\"a &gt; b\" onclick='ignored()' />\r\n");
    check(tag && tag->source == L"图片 & 图.png" && tag->alt == L"a > b" && tag->width == 120 &&
              tag->height == 60,
          "HTML image case quotes whitespace entities and ignored attributes");
    tag = parse_html_image(L"<img hidden src='first.png' src='second.png' width=50 width=70>");
    check(tag && tag->source == L"first.png" && tag->width == 50, "first duplicate HTML attribute wins");
    tag = parse_html_image(L"<img src=x.png width='99999999999999999' height='50%'>");
    check(tag && !tag->width && !tag->height, "invalid or excessive HTML dimensions use natural size");
    tag = parse_html_image(L"<img src=x.png width='0' height='-2'>");
    check(tag && !tag->width && !tag->height, "nonpositive HTML dimensions use natural size");
    tag = parse_html_image(L"<img src=x.png width='&#52;00'>");
    check(tag && tag->width == 400, "numeric entities in HTML dimensions");
    for (auto invalid :
         {L"<image src=x.png>", L"<img>", L"<img src=''>", L"<img src='unterminated>",
          L"<img src=x.png> trailing", L"<div><img src=x.png></div>", L"<!-- <img src=x.png> -->",
          L"<img src='x.png'alt='y'>", L"<img src='x&#10;y.png'>", L"<img src=x.png><img src=y.png>"})
        check(!parse_html_image(invalid), "unsupported or malformed HTML keeps source fallback");
    check(!parse_html_image(L"<img src='" + std::wstring(17000, L'a') + L"'>"), "HTML tag size bound");
    auto inlineHtml = parse_document("**before** <img src='x.png' alt='图片' width='40'> *after*\n");
    check(inlineHtml->blocks.size() == 3 && inlineHtml->blocks[1].kind == Kind::Image &&
              inlineHtml->blocks[1].text.value == L"图片" && inlineHtml->blocks[1].source == 11 &&
              inlineHtml->blocks[0].text.spans[0].style == Bold &&
              inlineHtml->blocks[2].text.spans[0].style == Italic,
          "inline HTML image keeps surrounding text styles and source offset");
    auto multilineHtml = parse_document("before <IMG\nSRC='x.png'\nwidth=90 height=30> after\n");
    check(multilineHtml->blocks.size() == 3 && multilineHtml->blocks[1].kind == Kind::Image &&
              multilineHtml->blocks[1].image_width == 90 && multilineHtml->blocks[1].image_height == 30,
          "multiline HTML image combines MD4C text callbacks");
    auto oversizedHtml =
        parse_document("before <img alt='" + std::string(17000, 'a') + "' src='x.png'> after\n");
    bool oversizedRendered = false;
    for (const auto &block : oversizedHtml->blocks)
        oversizedRendered |= block.kind == Kind::Image;
    check(!oversizedRendered && oversizedHtml->plain_text().find(L"after") != std::wstring::npos,
          "oversized inline HTML preserves source with bounded tag buffering");
    auto rawHtml = parse_document("```html\n<img src=x.png>\n```\n\n`<img src=x.png>`\n\n"
                                  "<script>\n<img src=x.png>\n</script>\n\n"
                                  "<div><img src=x.png></div>\n\n"
                                  "| image |\n| --- |\n| <img src=x.png> |\n");
    int rawImages = 0;
    for (const auto &block : rawHtml->blocks)
        rawImages += block.kind == Kind::Image;
    auto rawText = rawHtml->plain_text();
    check(rawImages == 1 && rawText.find(L"```") == std::wstring::npos &&
              rawText.find(L"<script>\n<img src=x.png>") != std::wstring::npos &&
              rawText.find(L"<img src=x.png>\n<img src=x.png>") != std::wstring::npos &&
              rawText.ends_with(L"image\n<img src=x.png>\n"),
          "code scripts and table cells preserve source; a div wrapper renders its image");
    auto readme = parse_document(
        "<h1 align=center>\n  <img src=\"https://example.invalid/logo.png\" alt=\"TypeWords\" "
        "style=\"width: 500px;\"/>\n</h1>\n\n<p align=\"center\">\n  <a href=\"/README.md\">English</a> |\n"
        "  <a href=\"/docs/README.es.md\">Espa&ntilde;ol</a>\n</p>\n\n<p align=\"center\">\n"
        "  <b>Practice words</b>\n</p>\n\n# Next\n");
    check(readme->blocks.size() == 4 && readme->blocks[0].kind == Kind::Image &&
              readme->blocks[0].text.value == L"TypeWords" && readme->blocks[0].text_align == 2 &&
              readme->blocks[1].text.value == L"English | Español" && readme->blocks[1].text_align == 2 &&
              readme->blocks[1].text.spans.size() == 2 && readme->blocks[1].text.spans[0].link == L"/README.md" &&
              readme->blocks[1].text.spans[1].start == 10 &&
              readme->blocks[1].text.spans[1].link == L"/docs/README.es.md" &&
              readme->blocks[2].text.value == L"Practice words" && readme->blocks[2].text.spans[0].style == Bold &&
              readme->blocks[3].kind == Kind::Heading && readme->headings.size() == 1 &&
              readme->plain_text().find(L'<') == std::wstring::npos,
          "README header HTML renders as centered native blocks");
    auto inlineTags =
        parse_document("Press <kbd>Ctrl</kbd>+<b>C</b><br>\nnext <a href=\"x.md\">link</a> <!-- note -->"
                       "<u>end</u><sup>1</sup> x <table> y\n\n## <a name=\"install\"></a>Install\n");
    const auto &tagged = inlineTags->blocks[0].text;
    check(tagged.value == L"Press Ctrl+C\nnext link end1 x <table> y" && tagged.spans.size() == 4 &&
              tagged.spans[0].start == 6 && tagged.spans[0].style == Mono && tagged.spans[1].style == Bold &&
              tagged.spans[2].link == L"x.md" && tagged.spans[3].style == Underline &&
              inlineTags->headings.size() == 1 && inlineTags->headings[0].anchor == L"install",
          "inline README tags format text, hide comments and anchors, keep unknown tags");
    auto htmlList = parse_document("<ul>\n<li>one</li>\n<li><b>two</b>\n<ol start=\"3\"><li>three</li></ol>\n"
                                   "</li>\n</ul>\n");
    check(htmlList->blocks.size() == 3 && htmlList->blocks[0].marker == L"•" &&
              htmlList->blocks[0].indent == 1 && htmlList->blocks[1].text.spans[0].style == Bold &&
              htmlList->blocks[2].marker == L"3." && htmlList->blocks[2].indent == 2,
          "HTML lists keep markers start numbers and nesting");
    auto details = parse_document("<details>\n<summary>More</summary>\n\n**hidden**\n\n</details>\n\nAfter\n");
    check(details->blocks.size() == 3 && details->blocks[0].text.value == L"More" &&
              details->blocks[0].marker == L"▾" && details->blocks[1].text.spans[0].style == Bold,
          "details content is shown expanded after its summary");
    auto container = parse_document("<div align=\"center\">\n\n# Title\n\n![logo](logo.png)\n\n**Bold**</div>"
                                    "\n\nAfter\n\n<center>\n\nMiddle\n\n</center>\n\nEnd\n");
    check(container->blocks.size() == 6 && container->blocks[0].text_align == 2 &&
              container->blocks[1].kind == Kind::Image && container->blocks[1].text_align == 2 &&
              container->blocks[2].text_align == 2 && container->blocks[3].text_align == 0 &&
              container->blocks[4].text_align == 2 && container->blocks[5].text_align == 0,
          "open containers align following Markdown until their end tag");
    auto fallback = parse_document("<div align=\"center\">\n<table><tr><td>cell</td></tr></table>\n</div>\n\n"
                                   "After\n\n<div align=\"right\">\n<script>x()</script>\n\nRight\n\n</div>\n\n"
                                   "<p align=\"center\"><img src='x.png'alt='y'></p>\n\nEnd\n");
    bool fallbackRaw = fallback->blocks.size() == 9 && fallback->blocks[0].kind == Kind::Html &&
                       fallback->blocks[3].text.value == L"After" && fallback->blocks[3].text_align == 0 &&
                       fallback->blocks[5].kind == Kind::Html && fallback->blocks[6].text.value == L"Right" &&
                       fallback->blocks[6].text_align == 3 && fallback->blocks[7].kind == Kind::Html &&
                       fallback->blocks[8].text_align == 0;
    check(fallbackRaw, "unsupported HTML keeps source while container nesting stays balanced");
    auto spacing = parse_document("<p>  a&nbsp;b   &amp;\n c <br> line<br/><br>last </p>\n<p>one<p>two</p>\n"
                                  "<!-- hidden -->\n\n<p align=\"center\"><span align=\"left\">x</span></p>\n");
    check(spacing->blocks.size() == 4 && spacing->blocks[0].text.value == L"a b & c\nline\n\nlast" &&
              spacing->blocks[1].text.value == L"one" && spacing->blocks[2].text.value == L"two" &&
              spacing->blocks[3].text_align == 2,
          "HTML whitespace collapses, entities decode, breaks and implied paragraph ends");
    auto across = parse_document("<p>a <b> </b>c <i>d </i> e</p>\n");
    check(across->blocks.size() == 1 && across->blocks[0].text.value == L"a c d e" &&
              across->blocks[0].text.spans.size() == 1 && across->blocks[0].text.spans[0].start == 4 &&
              across->blocks[0].text.spans[0].length == 2 && across->blocks[0].text.spans[0].style == Italic,
          "HTML whitespace collapses across inline element boundaries");
    auto picture = parse_document("- <div>\n  <picture>\n  <source srcset=\"dark.png\">\n  <img src=\"light.png\" "
                                  "alt=\"Logo\">\n  </picture>\n  </div>\n\n<blockquote><p>quoted</p></blockquote>\n");
    check(picture->blocks.size() == 2 && picture->blocks[0].kind == Kind::Image &&
              picture->blocks[0].target == L"light.png" && picture->blocks[0].indent == 1 &&
              picture->blocks[1].quote == 1,
          "picture uses its img fallback inside Markdown list context");
    std::string longHtml = "<p align=\"center\">";
    for (int i = 0; i < 4000; ++i)
        longHtml += "word ";
    auto longHtmlDoc = parse_document(longHtml + "</p>\n");
    bool longAligned = longHtmlDoc->blocks.size() > 2;
    for (const auto &block : longHtmlDoc->blocks)
        longAligned &= block.text_align == 2;
    check(longAligned && longHtmlDoc->blocks.front().joins_next && !longHtmlDoc->blocks.back().joins_next,
          "long HTML paragraphs keep alignment across layout chunks");
    std::string nestedHtml;
    for (int i = 0; i < 80; ++i)
        nestedHtml += "<div>";
    auto deep = parse_document(nestedHtml + "x\n");
    check(!deep->blocks.empty() && deep->blocks[0].kind == Kind::Html, "HTML nesting depth stays bounded");
    // Math is parsed but not typeset: the LaTeX source stays exactly as written and is marked with the
    // Formula style, so nothing is silently dropped or rewritten.
    auto math = parse_document(R"(Inline $E = mc^2$ and display:

$$\int_0^1 x^2 \, dx$$
)");
    std::vector<std::wstring> formulas;
    for (const auto &block : math->blocks)
        for (const auto &span : block.text.spans)
            if (span.style & Formula)
                formulas.push_back(block.text.value.substr(span.start, span.length));
    check(formulas.size() == 2, "inline and display math are both recognized");
    check(formulas.size() == 2 && formulas[0] == L"E = mc^2", "inline math keeps its exact source text");
    check(formulas.size() == 2 && formulas[1] == L"\\int_0^1 x^2 \\, dx",
          "display math keeps its exact source text");
    check(math->plain_text().find(L"E = mc^2") != std::wstring::npos &&
              math->plain_text().find(L"\\int_0^1 x^2 \\, dx") != std::wstring::npos &&
              math->plain_text().find(L"$") == std::wstring::npos,
          "math source stays searchable without its delimiters");
    check(search(*math, L"mc^2").size() == 1, "math source is searchable");
    auto escapedDollar = parse_document("Costs $5 and $6 total.\n");
    check(escapedDollar->plain_text().find(L"$5") != std::wstring::npos, "a lone dollar sign stays literal");
    auto spaced = parse_document("售价 $ 100 和 $ 200 元\n");
    check(spaced->plain_text().find(L"售价 $ 100 和 $ 200 元") != std::wstring::npos && !spaced->blocks.empty() &&
              spaced->blocks[0].text.spans.empty(),
          "dollar signs around spaces stay prose, with the dollars kept");
    std::string longMath(9000, 'a');
    for (size_t i = 100; i < longMath.size(); i += 100)
        longMath[i] = ' ';
    longMath += " $x + y + z$ tail\n";
    // The formula has spaces of its own and straddles the 8192-character chunk limit, so the usual break at
    // the last space before the limit would fall inside it.
    std::string straddling = "q";
    for (int i = 0; i < 99; ++i)
        straddling += " q";
    longMath.insert(8150, " $" + straddling + "$ ");
    auto chunked = parse_document(longMath);
    bool wholeFormulas = true;
    size_t formulaCount = 0;
    for (const auto &block : chunked->blocks)
        for (const auto &span : block.text.spans)
            if (span.style & Formula) {
                ++formulaCount;
                wholeFormulas &= span.length == 199 || span.length == 9;
            }
    check(chunked->blocks.size() > 1 && formulaCount == 2 && wholeFormulas,
          "splitting a long paragraph never cuts through a formula");
    // Verbatim text that merely contains dollar signs must not be captured as math.
    // Case list: expected Formula-span count per line.
    {
        const std::pair<std::wstring, size_t> dollarCases[] = {
            {L"价格 $5 和 $6 之间没有公式。", 0},
            {L"从 $5 到 $10 元。", 0},
            {L"总价 $5。", 0},
            {L"行内 $E = mc^2$ 混排。", 1},
            {L"$a$ 与 $b$", 2},
            {L"**加粗 $x^2$ 结尾**", 1},
            {L"售价 $ 100 和 $ 200 元", 0},
            {L"$$ x^2 $$", 1},
            {L"![图 $x$](a.png) 后文", 0},
        };
        for (const auto &[source, expected] : dollarCases) {
            auto probe = parse_document(keep::utf8(source));
            size_t found = 0;
            for (const auto &item : probe->blocks)
                for (const auto &span : item.text.spans)
                    if (span.style & Formula)
                        ++found;
            check(found == expected,
                  (std::string("dollar signs: ") + keep::utf8(source)).c_str());
        }
    }
    // MicroTeX typesets with KeepMD's Direct2D backend and embedded Computer Modern fonts.
    std::wstring mathError;
    auto energy = MathFormula::create(L"E = mc^2", false, 20, &mathError);
    check(energy && energy->width > 40 && energy->ascent > 8 && mathError.empty(), "inline formula is typeset");
    if (energy) {
        auto ink = draw_formula(*energy);
        check(ink.pixels > 80, "inline formula draws glyphs from the embedded fonts");
        check(ink_fits(*energy, ink), "inline formula ink stays inside its typeset box");
        check(ink.restored, "drawing a formula restores the caller's transform");
    }
    auto half = MathFormula::create(LR"(\frac{1}{2})", true, 20);
    check(half && half->ascent > 10 && half->descent > 5, "display fraction straddles the baseline");
    if (half)
        check(ink_fits(*half, draw_formula(*half)), "fraction bar and digits stay inside the box");
    auto integral = MathFormula::create(LR"(\int_0^1 x^2 \, dx = \frac{1}{3})", true, 20);
    check(integral && integral->width > 80, "display integral is typeset");
    if (integral) {
        auto ink = draw_formula(*integral);
        check(ink.pixels > 200 && ink_fits(*integral, ink), "display integral draws inside its box");
    }
    auto inlineIntegral = MathFormula::create(LR"(\int_0^1 x^2 \, dx)", false, 20);
    auto displayIntegral = MathFormula::create(LR"(\int_0^1 x^2 \, dx)", true, 20);
    check(inlineIntegral && displayIntegral &&
              inlineIntegral->ascent + inlineIntegral->descent < displayIntegral->ascent + displayIntegral->descent,
          "text style sets a lower integral than display style");
    auto chinese = MathFormula::create(LR"(\text{面积} = \pi r^2)", false, 20);
    check(chinese && chinese->width > 60, "Chinese text inside a formula is laid out");
    if (chinese)
        check(draw_formula(*chinese).pixels > 150, "Chinese text inside a formula is drawn");
    // Fonts with an unclear licence are not embedded; their symbols are drawn from Windows faces instead.
    check(FindResourceW(nullptr, L"CMR10", RT_RCDATA) && !FindResourceW(nullptr, L"SPECIAL", RT_RCDATA) &&
              !FindResourceW(nullptr, L"R10", RT_RCDATA) && !FindResourceW(nullptr, L"TT10", RT_RCDATA),
          "only fonts with a known licence are embedded");
    for (const wchar_t *symbol : {LR"(\texteuro)", LR"(\textperthousand)", LR"(\og x \fg)", LR"(\parallelogram)",
                                  LR"(\textmu)", LR"(\TeX)"}) {
        auto formula = MathFormula::create(symbol, false, 20);
        check(formula && draw_formula(*formula).pixels > 20,
              ("substituted symbol is drawn: " + keep::utf8(symbol)).c_str());
    }
    auto robot = MathFormula::create(LR"(a \android b)", false, 20);
    check(robot && draw_formula(*robot).pixels > 20, "a symbol without a stand-in leaves the rest of the formula");
    mathError.clear();
    check(!MathFormula::create(LR"(}{)", false, 20, &mathError) && !mathError.empty(),
          "a formula MicroTeX cannot recover from reports an error instead of drawing");
    // Like KaTeX, an unknown command stays visible in red rather than failing the whole formula.
    auto unknown = MathFormula::create(LR"(a + \nosuchcommand)", false, 20);
    check(unknown && draw_formula(*unknown).red > 20, "an unknown command is drawn in red");
    if (energy)
        check(draw_formula(*energy).red == 0, "a valid formula has no red error ink");
    mathError.clear();
    check(!MathFormula::create(std::wstring(MathFormula::kMaxSource + 1, L'x'), false, 20, &mathError) &&
              !mathError.empty(),
          "oversized formula stays as source");
    auto extent = image_extent(1164, 265, 400, 0, 800, 1);
    check(extent.width == 400 && std::abs(extent.height - 91.0653f) < .01f,
          "Ghidra logo requested width preserves aspect ratio");
    extent = image_extent(240, 120, 0, 30, 800, 2);
    check(extent.width == 120 && extent.height == 60, "height only honors zoom and aspect ratio");
    extent = image_extent(240, 120, 400, 100, 200, 1);
    check(extent.width == 200 && extent.height == 50, "both dimensions fit available width together");
    extent = image_extent(240, 120, 0, 0, 120, 2);
    check(extent.width == 120 && extent.height == 60, "existing unsized Markdown image fitting preserved");
    check(headings->headings[1].anchor == L"repeat-1", "duplicate heading anchor");
    check(search(*doc, L"BOLD").size() == 1, "case insensitive search");
    check(search(*doc, L"标题").size() == 1, "CJK search");
    check(doc->text_range(0, 2) == L"标题", "selection from semantic text");
    check(decode_entities("&lt;&gt;&quot;&unknown;&#0;") == L"<>\"&unknown;�", "entities fallback");
    check(wide(utf8(L"中文 😀 café")) == L"中文 😀 café", "Unicode round trip");
    Heights heights;
    heights.reset({10, 20, 30, 40});
    check(heights.total() == 100 && heights.locate(30) == 2, "height prefix lookup");
    heights.set(1, 35);
    check(heights.total() == 115 && heights.prefix(2) == 45 && heights.locate(44) == 1, "height updates");
    auto flow = parse_diagram("flowchart TD\nA[开始] -->|是| B{判断}\nB -.-> A\n");
    check(flow.ok && flow.graph.nodes.size() == 2 && flow.graph.edges.size() == 2,
          "native Chinese flowchart");
    if (flow.ok) {
        auto layout = mermaid::layout(flow.graph, {{100, 50}, {120, 60}}, 40, 70);
        check(layout.nodes.size() == 2 && std::isfinite(layout.height), "cyclic diagram terminates");
    }
    check(!parse_diagram("sequenceDiagram\nA->>B: hi").ok, "unsupported diagram reports failure");
    check(!parse_diagram("flowchart TD\nA@{ shape: rounded } --> B").ok,
          "unsupported syntax does not silently render");
    std::string huge = "graph LR\n";
    for (int i = 0; i < 205; ++i)
        huge += "N" + std::to_string(i) + "-->N" + std::to_string(i + 1) + "\n";
    check(!parse_diagram(huge).ok, "diagram node budget");
    auto temp = std::filesystem::temp_directory_path() / L"keepmd-core-tests";
    std::filesystem::create_directories(temp);
    std::wstring error;
    for (auto encoding : {Encoding::Utf8, Encoding::Utf8Bom, Encoding::Utf16LE, Encoding::Utf16BE}) {
        auto path = temp / (std::to_wstring((int)encoding) + L" 中文.md");
        check(save_file(path, L"# 中文\r\n😀\r\n", encoding, error), "atomic Unicode save");
        FileData data;
        check(load_file(path, data, error) && data.encoding == encoding && data.crlf &&
                  data.utf8 == "# 中文\r\n😀\r\n",
              "encoding preservation");
        check(save_file(path, L"changed\n", encoding, error), "replace existing file");
        std::filesystem::remove(path);
    }
    auto repo = temp / L"repo 中文";
    std::filesystem::create_directories(repo / L".git");
    std::filesystem::create_directories(repo / L"docs");
    check(linked_path(repo / L"docs", L"/public/imgs/words.png") == repo / L"public/imgs/words.png" &&
              linked_path(repo / L"docs", L"README.es.md") == repo / L"docs/README.es.md" &&
              linked_path(repo, L"/README.md") == repo / L"README.md" &&
              linked_path(repo / L"docs", L"//server/share/x.png").wstring().starts_with(L"//server"),
          "root-relative links resolve to the repository root");
    std::filesystem::remove_all(repo);
    auto bad = temp / L"invalid.md";
    {
        std::ofstream out(bad, std::ios::binary);
        out << "\xffinvalid";
    }
    FileData data;
    check(!load_file(bad, data, error), "invalid UTF8 explicit failure");
    std::filesystem::remove(bad);
    auto locked = temp / L"locked.md";
    save_file(locked, L"original 中文", Encoding::Utf8, error);
    HANDLE lock =
        CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "failure test holds destination against replacement");
    check(!save_file(locked, L"replacement", Encoding::Utf8, error),
          "locked destination reports save failure");
    if (lock != INVALID_HANDLE_VALUE)
        CloseHandle(lock);
    check(load_file(locked, data, error) && data.utf8 == "original 中文",
          "failed save leaves original bytes readable");
    std::filesystem::remove(locked);
    std::atomic_bool cancel = true;
    check(!parse_document("# Cancel\n", &cancel)->error.empty(), "parse cancellation");
    std::string longText;
    for (int i = 0; i < 10000; ++i)
        longText += "word 中文 ";
    longText += "END";
    auto longDoc = parse_document(longText);
    check(longDoc->blocks.size() > 1, "long paragraph has bounded physical chunks");
    check(longDoc->plain_text() == wide(longText) + L"\n", "chunking preserves exact semantic text");
    check(search(*longDoc, L"word 中文").size() == 10000, "chunk boundaries preserve search matches");
    auto longCode = std::string(100000, 'x');
    auto codeDoc = parse_document("```\n" + longCode + "\n```\n");
    check(codeDoc->blocks.size() > 1 && codeDoc->plain_text() == wide(longCode) + L"\n",
          "oversized code line chunks preserve source copying");
    // Highlight lookup agrees with a full scan for every block, and a match straddling a chunk boundary
    // belongs to both chunks so its tail is highlighted too.
    auto wordMatches = search(*longDoc, L"word 中文");
    bool lookupAgrees = true;
    for (const auto &block : longDoc->blocks) {
        size_t begin = block.text_start, end = begin + block_plain(block).size();
        auto [first, last] = matches_in(wordMatches, begin, end);
        size_t expected = 0;
        for (const auto &m : wordMatches)
            expected += m.absolute + m.length > begin && m.absolute < end;
        lookupAgrees &= last - first == expected;
        for (size_t m = first; m < last; ++m)
            lookupAgrees &= wordMatches[m].absolute + wordMatches[m].length > begin && wordMatches[m].absolute < end;
    }
    check(lookupAgrees, "match lookup by text range equals a full scan");
    auto straddleDoc = parse_document("```\n" + std::string(8190, 'a') + "NEEDLE" + std::string(9000, 'a') + "\n```\n");
    auto needle = search(*straddleDoc, L"needle");
    bool straddles = needle.size() == 1 && straddleDoc->blocks[needle[0].block].joins_next &&
                     needle[0].block + 1 < straddleDoc->blocks.size();
    if (straddles) {
        const auto &tail = straddleDoc->blocks[needle[0].block + 1];
        auto [first, last] = matches_in(needle, tail.text_start, tail.text_start + block_plain(tail).size());
        straddles = needle[0].absolute + needle[0].length > tail.text_start && first == 0 && last == 1;
    }
    check(straddles, "a match straddling a chunk boundary is highlighted in the following chunk");
    check(matches_in({}, 0, 100) == std::pair<size_t, size_t>(0, 0), "no matches yields an empty range");
    auto bigDiagram =
        parse_document("```mermaid\ngraph LR\n%%" + std::string(100000, 'x') + "\nA-->B\n```\n");
    check(std::none_of(bigDiagram->blocks.begin(), bigDiagram->blocks.end(),
                       [](const Block &block) { return block.kind == Kind::Diagram; }) &&
              bigDiagram->blocks.size() > 5,
          "oversized diagram fallback uses bounded code layouts");
    for (const std::wstring cluster : {L"é", L"👩‍💻", L"🇨🇳", L"👍🏽", L"1️⃣"})
        for (size_t inside = 1; inside < cluster.size(); ++inside) {
            auto text = std::wstring(8192 - inside, L'x') + cluster + std::wstring(9000, L'z');
            auto chunks = parse_document(utf8(text));
            check(chunks->plain_text() == text + L"\n" && chunks->blocks[0].text.value.back() == L'x',
                  "combining/emoji cluster preserved at fragment boundary");
        }
    for (const char *direction : {"TD", "TB", "BT", "LR", "RL"})
        for (const char *node :
             {"A[开始]", "A(开始)", "A{判断}", "A((圆形))", "A([结束])", "A[\"中文 label\"]"}) {
            auto f =
                parse_diagram(std::string("flowchart ") + direction + "\n" + node + " -->|标签| B[完成]\n");
            check(f.ok && f.graph.nodes.size() == 2 && f.graph.edges.size() == 1,
                  "flow direction/shape semantic matrix");
        }
    check(!parse_diagram("flowchart TD\nsubgraph A\nB-->C\n").ok, "unclosed subgraph fails explicitly");
    check(!parse_diagram("flowchart TD\nA-->B\nstyle A font-size:20px\n").ok,
          "unknown style reports unsupported");
    check(!parse_diagram("flowchart TD\nA-->B\nlinkStyle 0 stroke:red\n").ok,
          "unsupported edge style reports unsupported");
    auto grouped = parse_diagram(
        "graph LR\nsubgraph left [左组]\nA-->B\nend\nsubgraph right [右组]\nC-->D\nend\nB-->C\n");
    check(grouped.ok && grouped.graph.subgraphs.size() == 2 && grouped.graph.edges.size() == 3,
          "cross-group node connections");
    auto self = parse_diagram("graph TD\nA[自己]-->A\n");
    check(self.ok && self.graph.edges.size() == 1, "self loop retained");
    auto styled = parse_diagram("graph LR\nA[Start]:::chosen --> B[End]\nclassDef chosen "
                                "fill:#abc,color:#123456\nstyle A color:red\n");
    check(styled.ok && styled.graph.nodes[0].style.hasFill &&
              styled.graph.nodes[0].style.fill.rgb == 0xaabbcc &&
              styled.graph.nodes[0].style.text.rgb == 0xff0000,
          "late class definition resolves and direct style takes precedence");
    auto defaultStyle = parse_diagram("graph LR\nA-->B\nclassDef default fill:#eeeeee\n");
    check(defaultStyle.ok && defaultStyle.graph.nodes[1].style.fill.rgb == 0xeeeeee,
          "default graph class applies to unclassified nodes");
    ReaderSettings prefs;
    prefs.dark = true;
    prefs.toolbar = false;
    prefs.zoom = 1.5f;
    prefs.reading_width = 700;
    for (int i = 0; i < 12; ++i)
        prefs.remember(temp / (L"文档😀" + std::to_wstring(i) + L".md"), i);
    check(prefs.recent.size() == 10, "recent history remains bounded");
    auto config = temp / L"settings.ini";
    check(store_settings(config, prefs), "preferences atomically saved");
    auto restored = load_settings(config);
    check(!restored.toolbar, "collapsed reader toolbar preference survives restart");
    check(restored.dark && restored.zoom == 1.5f && restored.reading_width == 700 &&
              restored.recent[0].path == prefs.recent[0].path && restored.recent[0].block == 11,
          "Unicode recent path and position survive restart");
    std::filesystem::remove(config);
    auto prefix = L"Software\\KeepMD.IntegrationTests\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64());
    auto plan = registration_plan(L"C:\\Program Files\\KeepMD\\keepmd.exe", prefix);
    check(apply_registration(plan, error), "Open With registration writes only isolated test keys");
    wchar_t value[512]{};
    DWORD bytes = sizeof(value);
    auto key = prefix + L"\\Classes\\KeepMD.Markdown\\shell\\open\\command";
    check(RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes) ==
                  ERROR_SUCCESS &&
              std::wstring(value) == L"\"C:\\Program Files\\KeepMD\\keepmd.exe\" \"%1\"",
          "registered executable and argument are quoted");
    bytes = sizeof(value);
    check(RegGetValueW(HKEY_CURRENT_USER, (prefix + L"\\Classes\\.md").c_str(), nullptr, RRF_RT_ANY, nullptr,
                       value, &bytes) != ERROR_SUCCESS,
          "registration does not replace extension default");
    check(prefix.starts_with(L"Software\\KeepMD.IntegrationTests\\") &&
              RegDeleteTreeW(HKEY_CURRENT_USER, prefix.c_str()) == ERROR_SUCCESS,
          "isolated registry test values removed");
    PromptHotkey hotkey;
    check(parse_prompt_hotkey(L" ctrl + ALT + p ", hotkey, error) && hotkey.key == 'P' &&
              hotkey.modifiers == (MOD_CONTROL | MOD_ALT) && hotkey.label == L"Ctrl+Alt+P",
          "global shortcut normalizes a combination");
    for (const auto *invalid :
         {L"P", L"Shift+A", L"Ctrl+F12", L"Ctrl+P+Q", L"Ctrl++P", L"Ctrl+P+", L"Ctrl+Ctrl+P"})
        check(!parse_prompt_hotkey(invalid, hotkey, error), "unsafe or malformed shortcut rejected");
    check(parse_prompt_hotkey(L"LCtrl x2", hotkey, error) && hotkey.doubleCtrl,
          "legacy double Ctrl supported");
    CtrlTap tap;
    check(!tap.event(true, true, 1000) && !tap.event(true, false, 1020) && !tap.event(true, true, 1100) &&
              tap.event(true, false, 1120),
          "two completed clean Ctrl taps trigger");
    tap = {};
    tap.event(true, true, 1000);
    tap.event(false, true, 1010);
    tap.event(true, false, 1020);
    tap.event(true, true, 1050);
    check(!tap.event(true, false, 1080), "Ctrl+C followed by Ctrl cannot summon prompt");
    tap = {};
    tap.event(true, true, 1000);
    tap.event(true, true, 1010);
    tap.event(true, false, 1800);
    tap.event(true, true, 1820);
    check(!tap.event(true, false, 1840), "held Ctrl and repeats cannot summon prompt");
    tap = {};
    tap.event(true, true, 1000, true);
    tap.event(true, false, 1020, true);
    tap.event(true, true, 1100, true);
    check(!tap.event(true, false, 1120, true), "another held key prevents double Ctrl");
    tap.event(true, true, 1200);
    tap.event(true, false, 1220);
    tap.event(true, true, 1300);
    check(tap.event(true, false, 1320), "double Ctrl recovers after another held modifier is released");
    PromptSettings ps;
    ps.toolbar = false;
    ps.hotkey = L"Ctrl+Shift+F9";
    ps.resident = true;
    ps.opacity = 70;
    ps.preview = false;
    auto promptConfig = temp / L"prompts.ini";
    check(save_prompt_settings(promptConfig, ps, error), "prompt preferences saved atomically");
    auto ps2 = load_prompt_settings(promptConfig);
    check(!ps2.toolbar, "collapsed prompt toolbar preference survives restart independently");
    check(ps2.hotkey == ps.hotkey && ps2.resident && ps2.opacity == 70 && !ps2.preview,
          "prompt preferences survive restart independently from reader");
    std::filesystem::remove(promptConfig);
    std::wstring draft = L"untouched";
    check(
        import_prompt_flow(
            R"({"Prompt":"# 标题\n\n  缩进\n\ud83d\ude00\n","Hotkey":"LCtrl x2","AlwaysOnTop":false,"Opacity":80,"Autostart":true})",
            ps2, draft, error) &&
            draft == L"# 标题\n\n  缩进\n😀\n" && ps2.hotkey == L"LCtrl x2" && !ps2.top && ps2.opacity == 80,
        "Prompt Flow import preserves Markdown whitespace, Chinese and escaped emoji");
    const auto baseline = draft;
    for (const char *invalid :
         {R"({"Prompt":"unterminated})", R"({"Prompt":"\ud800"})", R"({"Prompt":"\u0000"})",
          R"({"Prompt":"a","Prompt":"b"})", R"({"Prompt":"a",})", R"({"Opacity":70})",
          R"({"Prompt":"a","Opacity":500})", R"({"Prompt":{}})", R"({"Prompt":"a"}trailing)"})
        check(!import_prompt_flow(invalid, ps2, draft, error) && draft == baseline,
              "bad legacy input leaves draft unchanged");
    check(
        !import_prompt_flow("{\"Prompt\":\"" + std::string(PromptLimit + 1, 'a') + "\"}", ps2, draft, error),
        "oversized prompt import rejected");
    auto startup = prompt_startup_command(L"C:\\Apps with spaces\\KeepMD.exe", L"C:\\用户\\profile.ini");
    check(startup == L"\"C:\\Apps with spaces\\KeepMD.exe\" --resident --config \"C:\\用户\\profile.ini\"",
          "startup command quotes executable and profile path");
    auto startupKey = L"Software\\KeepMD.IntegrationTests\\Startup-" + std::to_wstring(GetCurrentProcessId());
    check(set_prompt_startup(L"test", startup, true, error, startupKey) &&
              prompt_startup_enabled(L"test", startup, startupKey),
          "startup registration uses isolated test key");
    check(set_prompt_startup(L"test", startup, false, error, startupKey) &&
              !prompt_startup_enabled(L"test", startup, startupKey),
          "startup registration can be removed");
    RegDeleteTreeW(HKEY_CURRENT_USER, startupKey.c_str());
    // Profiles: the portable default, a pre-split folder, and a pre-split startup entry naming the reader profile.
    auto profiles = temp / L"profiles";
    std::filesystem::remove_all(profiles);
    std::filesystem::create_directories(profiles);
    auto fresh = prompt_profile(profiles / L"KeepPrompt.ini", false);
    check(fresh.settings == profiles / L"KeepPrompt.ini" && fresh.draft == profiles / L"KeepPrompt.draft.md" &&
              fresh.readerProfile.empty(),
          "new prompt profile keeps settings and draft beside KeepPrompt.exe");
    check(save_file(profiles / L"keepmd.prompt.md", L"# 旧草稿\n", Encoding::Utf8, error), "legacy draft fixture");
    auto upgraded = prompt_profile(profiles / L"KeepPrompt.ini", false);
    check(upgraded.settings == profiles / L"keepmd.prompt.ini" && upgraded.draft == profiles / L"keepmd.prompt.md" &&
              upgraded.readerProfile == profiles / L"keepmd.ini",
          "pre-split folder keeps using its prompt draft and settings in place");
    check(save_file(profiles / L"KeepPrompt.draft.md", L"", Encoding::Utf8, error) &&
              prompt_profile(profiles / L"KeepPrompt.ini", false).readerProfile.empty(),
          "a folder that already has a KeepPrompt profile uses it");
    auto reader = profiles / L"reader.ini";
    check(save_file(reader, L"[Reader]\nDark=0\n", Encoding::Utf8, error), "reader profile fixture");
    auto entry = prompt_profile(reader, true);
    check(entry.settings == profiles / L"reader.prompt.ini" && entry.draft == profiles / L"reader.prompt.md" &&
              entry.readerProfile == reader,
          "pre-split startup entry never uses the KeepMD reader profile as prompt settings");
    auto own = profiles / L"own.ini";
    check(save_prompt_settings(own, PromptSettings{}, error) && prompt_profile(own, true).settings == own &&
              prompt_profile(own, true).draft == profiles / L"own.draft.md" &&
              prompt_profile(own, true).readerProfile.empty(),
          "explicit KeepPrompt profile is used as given");
    check(save_file(profiles / L"gone.prompt.md", L"x", Encoding::Utf8, error) &&
              prompt_profile(profiles / L"gone.ini", true).readerProfile == profiles / L"gone.ini",
          "startup entry whose reader profile was never written still finds its prompt files");
    std::filesystem::remove_all(profiles);
    auto visual = visual_plan(L"# 标题 😀\n\n正文 **粗体** 和 *斜体* 与 `代码`。\n- 项目\n");
    check(visual.source == L"# 标题 😀\n\n正文 **粗体** 和 *斜体* 与 `代码`。\n- 项目\n",
          "visual mapping preserves original Markdown whitespace and emoji");
    check(visual.paragraphs[0].heading == 1 && (visual.styles[0] & VHidden) && !(visual.styles[2] & VHidden),
          "visual heading hides its syntax but keeps title editable");
    auto visualBold = visual.source.find(L"粗体"), visualItalic = visual.source.find(L"斜体"),
         visualCode = visual.source.find(L"代码");
    check((visual.styles[visualBold] & VBold) && (visual.styles[visualBold - 1] & VHidden) &&
              (visual.styles[visualItalic] & VItalic) && (visual.styles[visualCode] & VCode),
          "visual inline semantic formatting comes from Markdown parser");
    auto nested = visual_plan(L"***nested*** [link](https://example.test) \\*literal\\*\n");
    check((nested.styles[3] & (VBold | VItalic)) == (VBold | VItalic) &&
              (nested.styles[nested.source.find(L"link")] & VLink),
          "nested emphasis and link display mapped without rewriting targets");
    check(!(nested.styles[nested.source.find(L"*literal")] & VHidden),
          "escaped visible literal punctuation remains visible");
    auto flowVisual = visual_plan(L"前文\n\n```mermaid\nflowchart LR\nA-->B\n```\n\n后文\n");
    check(flowVisual.diagrams.size() == 1 && flowVisual.diagrams[0].source == L"flowchart LR\nA-->B\n" &&
              flowVisual.styles[flowVisual.source.find(L"A-->")] & VHidden,
          "visual diagram retains exact source behind native figure");
    auto incomplete = visual_plan(L"```mermaid\nflowchart LR\nA-->B\n");
    check(incomplete.diagrams.empty() && !(incomplete.styles[incomplete.source.find(L"A-->")] & VHidden),
          "incomplete diagram remains editable code until completed");
    std::wstring many;
    for (int i = 0; i < 20; ++i)
        many += L"```mermaid\ngraph TD\nA-->B\n```\n";
    check(visual_plan(many).diagrams.size() == 16, "visual inline picture count is bounded");
    check(visual_plan(L"```mermaid  \ngraph LR\nA-->B\n```\n").diagrams.size() == 1,
          "visual Mermaid language tolerates trailing whitespace");
    check(visual_plan(L"```mermaid\ngraph LR\nA-->B\n```not a closing fence\n").diagrams.empty(),
          "visual fence closing line must contain no info string");
    check(visual_plan(L"a\r\nb\rc\n").source == L"a\nb\nc\n",
          "visual import normalizes only paragraph line endings");
    check(rtf_escape(L"{\\}中文😀").find("\\u") != std::string::npos && rtf_escape(L"{\\}") == "\\{\\\\\\}",
          "RTF output escapes markup and Unicode scalars");
    auto imageUrl = remote_image_url(L"https://example.com:8443/a%20b.png?raw=1#preview");
    check(imageUrl && imageUrl->secure && imageUrl->port == 8443 && imageUrl->host == L"example.com" &&
              imageUrl->resource == L"/a%20b.png?raw=1" && imageUrl->url.find(L'#') == std::wstring::npos,
          "remote image URL keeps port and escaped query, strips fragment");
    auto relativeImage = remote_image_url(L"//example.com/photo.png");
    check(relativeImage && relativeImage->secure && relativeImage->url == L"https://example.com/photo.png",
          "protocol-relative image uses HTTPS");
    auto simpleImage = remote_image_url(L"HTTP://localhost:8080?image=1");
    check(simpleImage && !simpleImage->secure && simpleImage->port == 8080 && simpleImage->resource == L"/?image=1",
          "remote image supports case-insensitive HTTP and an empty path");
    auto ipv6Image = remote_image_url(L"https://[::1]:9443/test.png");
    check(ipv6Image && ipv6Image->port == 9443, "remote image parses bracketed IPv6");
    check(!remote_image_url(L"https://user:secret@example.com/a.png") &&
              !remote_image_url(L"http://@example.com/a.png"), "remote image rejects URL credentials");
    check(!remote_image_url(L"file:///C:/image.png") && !remote_image_url(L"data:image/png;base64,AAAA") &&
              !remote_image_url(L"\\\\server\\image.png"), "only HTTP(S) images may be downloaded");
    check(!remote_image_url(L"https://example.com/a\r\nX-Test: value") &&
              !remote_image_url(L"https://example.com/a b") && !remote_image_url(L"http://"),
          "remote image rejects control characters, unescaped spaces and missing hosts");
    check(!remote_image_url(L"https://example.com/" + std::wstring(8192, L'a')), "remote image URL length is bounded");
    auto canonicalA = remote_image_url(L"HTTPS://EXAMPLE.COM:443/a.png?size=2#image");
    auto canonicalB = remote_image_url(L"https://example.com/a.png?size=2");
    check(canonicalA && canonicalB && canonicalA->url == canonicalB->url,
          "cache identity normalizes scheme, hostname, default port and fragments");
    check(image_cache_key(L"") == L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "persistent URL keys use stable SHA-256");
    check(canonicalA && canonicalB && image_cache_key(canonicalA->url) == image_cache_key(canonicalB->url),
          "equivalent image URLs share a persistent cache entry");
    check(image_cache_key(L"https://example.com/a.png?size=1") != image_cache_key(L"https://example.com/a.png?size=2"),
          "distinct image query parameters retain distinct cache entries");
    std::atomic_bool cancelledImage{true};
    auto cancelledFetch = fetch_remote_image(*relativeImage, cancelledImage);
    check(cancelledFetch.bytes.empty() && !cancelledFetch.error.empty(), "cancelled download starts no request");

    // Decode failures carry a reason, so a missing file is not reported as an unsupported format.
    auto imageDir = std::filesystem::temp_directory_path() / "keepmd-image-failure-tests";
    std::error_code imageEc;
    std::filesystem::remove_all(imageDir, imageEc);
    std::filesystem::create_directories(imageDir, imageEc);
    auto missing = decode_image_file(imageDir / L"absent.png");
    check(missing.failure == ImageFailure::Missing && missing.pixels.empty() && !missing.error.empty(),
          "absent image reports a missing file");
    check(missing.error.find(L"absent.png") != std::wstring::npos,
          "missing image names the file it could not find");
    auto corrupt = imageDir / L"corrupt.png";
    {
        std::ofstream stream(corrupt, std::ios::binary);
        stream << "this is not an image";
    }
    auto corruptImage = decode_image_file(corrupt);
    check(corruptImage.failure == ImageFailure::Unsupported && corruptImage.error == image_failure_message(ImageFailure::Unsupported),
          "unreadable content in a present file reports an unsupported format, not a missing file");
    // Bytes have no path, so every failure there is a format failure.
    check(decode_image_bytes({}).failure == ImageFailure::Unsupported, "empty image bytes report an unsupported format");
    const uint8_t notAnImage[] = {0x00, 0x01, 0x02, 0x03};
    check(decode_image_bytes(notAnImage).failure == ImageFailure::Unsupported, "non-image bytes report an unsupported format");
    auto directoryAsImage = decode_image_file(imageDir);
    check(directoryAsImage.failure == ImageFailure::Unreadable, "a directory in place of an image reports an unreadable file");
    // A present file that cannot be opened (here: held without sharing) is an I/O failure, not a missing file.
    auto lockedImagePath = imageDir / L"locked.png";
    std::filesystem::copy_file(corrupt, lockedImagePath, imageEc);
    HANDLE lockHandle = CreateFileW(lockedImagePath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    auto lockedImage = decode_image_file(lockedImagePath);
    check(lockHandle != INVALID_HANDLE_VALUE && lockedImage.failure == ImageFailure::Unreadable &&
              lockedImage.error.find(L"locked.png") != std::wstring::npos,
          "an image file locked by another handle reports an unreadable file");
    if (lockHandle != INVALID_HANDLE_VALUE)
        CloseHandle(lockHandle);
    check(image_failure_message(ImageFailure::None).empty(), "successful images carry no failure message");
    check(image_failure_message(ImageFailure::Missing) != image_failure_message(ImageFailure::Unsupported) &&
              image_failure_message(ImageFailure::Unreadable) != image_failure_message(ImageFailure::Unsupported) &&
              image_failure_message(ImageFailure::TooLarge) != image_failure_message(ImageFailure::Unsupported),
          "each decode failure reason reads differently in the reader");

    // SVG falls back to Direct2D once WIC rejects it. Pure shapes render at intrinsic size, twice as dense.
    auto svgBytes = [](std::string_view text) {
        return std::vector<uint8_t>(text.begin(), text.end());
    };
    auto shapeSvg = svgBytes("<?xml version='1.0'?><svg xmlns='http://www.w3.org/2000/svg' width='40' height='20'>"
                             "<rect width='40' height='20' fill='#ff0000'/></svg>");
    auto shape = decode_image_bytes(shapeSvg);
    check(shape.failure == ImageFailure::None && shape.density == 2 && shape.width == 80 && shape.height == 40,
          "shape-only SVG rasterizes at twice its intrinsic size");
    // Pixels are premultiplied BGRA: the opaque red fill must actually have been drawn.
    check(shape.pixels.size() == 80u * 40 * 4 && shape.pixels[2] == 255 && shape.pixels[3] == 255 &&
              shape.pixels[0] == 0 && shape.pixels[1] == 0,
          "SVG fill is drawn into the pixels");
    auto boxOnly = decode_image_bytes(svgBytes("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 10'>"
                                               "<circle cx='5' cy='5' r='5'/></svg>"));
    check(boxOnly.failure == ImageFailure::None && boxOnly.width == 60 && boxOnly.height == 20,
          "SVG without width/height takes its size from the viewBox");
    auto oneSide = decode_image_bytes(svgBytes("<svg xmlns='http://www.w3.org/2000/svg' width='50' viewBox='0 0 100 20'>"
                                               "<rect width='100' height='20'/></svg>"));
    check(oneSide.failure == ImageFailure::None && oneSide.width == 100 && oneSide.height == 20,
          "SVG with one explicit side keeps the viewBox aspect ratio");
    auto bare = decode_image_bytes(svgBytes("<svg xmlns='http://www.w3.org/2000/svg'><rect width='1' height='1'/></svg>"));
    check(bare.failure == ImageFailure::None && bare.width == 600 && bare.height == 300,
          "SVG with no size information defaults to 300x150 like a browser");
    auto wide = decode_image_bytes(svgBytes("<svg xmlns='http://www.w3.org/2000/svg' width='4000' height='1000'>"
                                            "<rect width='4000' height='1000'/></svg>"));
    check(wide.failure == ImageFailure::None && wide.width == 1600 && wide.height == 400 && wide.density == 0.4f,
          "large SVG rasterizes within the preview pixel cap and keeps its layout size");
    // Badges carry their meaning in <text>, which Direct2D drops silently: show a placeholder instead.
    auto badge = decode_image_bytes(svgBytes("<svg xmlns='http://www.w3.org/2000/svg' width='90' height='20'>"
                                             "<rect width='90' height='20'/><text x='5' y='14'>build</text></svg>"));
    check(badge.failure == ImageFailure::SvgUnsupported && badge.pixels.empty(), "SVG with text reports dropped content");
    auto styleSheetSvg = decode_image_bytes(svgBytes("<SVG xmlns='http://www.w3.org/2000/svg'><STYLE>.a{fill:red}</STYLE>"
                                              "<rect class='a' width='9' height='9'/></SVG>"));
    check(styleSheetSvg.failure == ImageFailure::SvgUnsupported, "SVG style sheets are detected case-insensitively");
    check(decode_image_bytes(svgBytes("<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect "))
                  .failure == ImageFailure::Unsupported,
          "malformed SVG reports an unsupported format");
    check(decode_image_bytes(svgBytes("<svg xmlns='http://www.w3.org/2000/svg' width='99999' height='10'>"
                                      "<rect width='9' height='9'/></svg>"))
                  .failure == ImageFailure::TooLarge,
          "SVG beyond the preview edge limit is rejected");
    std::string hugeSvg = "<svg xmlns='http://www.w3.org/2000/svg' width='9' height='9'><desc>";
    hugeSvg.append(4 * 1024 * 1024, 'x');
    hugeSvg += "</desc></svg>";
    check(decode_image_bytes(svgBytes(hugeSvg)).failure == ImageFailure::TooLarge, "SVG source size is bounded");
    // Raster images are unaffected by the SVG path.
    check(decode_image_bytes(svgBytes("not xml at all")).failure == ImageFailure::Unsupported,
          "non-SVG text stays unsupported");
    auto svgFile = imageDir / L"logo.svg";
    {
        std::ofstream stream(svgFile, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(shapeSvg.data()), (std::streamsize)shapeSvg.size());
    }
    auto svgFromFile = decode_image_file(svgFile);
    check(svgFromFile.failure == ImageFailure::None && svgFromFile.width == 80 && svgFromFile.height == 40,
          "local SVG files decode through the same path");
    // An HTML <img width> enlarges the SVG on screen, so it rasterizes at twice that size, laying out unchanged.
    auto hinted = decode_image_file(svgFile, 160);
    check(hinted.failure == ImageFailure::None && hinted.width == 320 && hinted.height == 160 &&
              std::lround(hinted.width / hinted.density) == 40,
          "requested width rasterizes the SVG sharply at its displayed size");
    auto hintedHeight = decode_image_bytes(shapeSvg, 0, 60);
    check(hintedHeight.width == 240 && hintedHeight.height == 120, "requested height alone sets the SVG raster size");
    auto shrunk = decode_image_bytes(shapeSvg, 10, 0);
    check(shrunk.width == 80 && shrunk.density == 2, "a smaller requested size never drops below twice intrinsic");
    auto capped = decode_image_bytes(shapeSvg, 100000, 0);
    check(capped.width == 1600 && capped.height == 800, "requested size stays within the raster cap");
    std::filesystem::remove_all(imageDir, imageEc);

    // Fenced code syntax colours: token spans overlay unchanged text and survive line splitting.
    {
        auto has = [](const std::vector<Highlight> &tokens, std::wstring_view text, std::wstring_view word,
                      Token token) {
            for (const auto &t : tokens)
                if (t.token == token && text.substr(t.start, t.length) == word)
                    return true;
            return false;
        };
        std::wstring_view cpp = L"int total = 42; // answer\nreturn say(\"hi\");";
        auto cppTokens = highlight(L"cpp", cpp);
        check(has(cppTokens, cpp, L"int", Token::Builtin) && has(cppTokens, cpp, L"return", Token::Keyword) &&
                  has(cppTokens, cpp, L"42", Token::Number) &&
                  has(cppTokens, cpp, L"// answer", Token::Comment) &&
                  has(cppTokens, cpp, L"\"hi\"", Token::String) && has(cppTokens, cpp, L"say", Token::Function),
              "c++ tokens: type, keyword, number, comment, string, call");
        std::wstring_view py = L"def f(x):\n    return None  # note\ns = f\"v={x}\"";
        auto pyTokens = highlight(L"python", py);
        check(has(pyTokens, py, L"def", Token::Keyword) && has(pyTokens, py, L"None", Token::Builtin) &&
                  has(pyTokens, py, L"# note", Token::Comment) && has(pyTokens, py, L"f\"v={x}\"", Token::String),
              "python tokens: keyword, builtin, comment, prefixed string");
        std::wstring_view js = L"const s = `multi\nline`;\nif (a) g(); // x";
        auto jsTokens = highlight(L"js", js);
        check(has(jsTokens, js, L"const", Token::Keyword) &&
                  has(jsTokens, js, L"`multi\nline`", Token::String) && has(jsTokens, js, L"g", Token::Function),
              "javascript tokens: template string spans lines");
        std::wstring_view rs = L"let x: u32 = 5;\nfn main() { println!(\"hi\"); }";
        auto rsTokens = highlight(L"rust", rs);
        check(has(rsTokens, rs, L"fn", Token::Keyword) && has(rsTokens, rs, L"u32", Token::Builtin) &&
                  has(rsTokens, rs, L"println", Token::Function),
              "rust tokens: keyword, type, macro call");
        std::wstring_view json = L"{\"key\": [1, true, null]}";
        auto jsonTokens = highlight(L"json", json);
        check(has(jsonTokens, json, L"\"key\"", Token::Key) && has(jsonTokens, json, L"1", Token::Number) &&
                  has(jsonTokens, json, L"true", Token::Keyword),
              "json tokens: key, number, literal");
        std::wstring_view html = L"<div class=\"a\" id=b>text</div><!-- note -->";
        auto htmlTokens = highlight(L"html", html);
        check(has(htmlTokens, html, L"div", Token::Keyword) && has(htmlTokens, html, L"class", Token::Builtin) &&
                  has(htmlTokens, html, L"\"a\"", Token::String) &&
                  has(htmlTokens, html, L"<!-- note -->", Token::Comment),
              "markup tokens: tag, attribute, value, comment");
        std::wstring_view sql = L"SELECT a FROM t WHERE x = 'v'; -- note";
        auto sqlTokens = highlight(L"sql", sql);
        check(has(sqlTokens, sql, L"SELECT", Token::Keyword) && has(sqlTokens, sql, L"'v'", Token::String) &&
                  has(sqlTokens, sql, L"-- note", Token::Comment),
              "sql tokens are case-insensitive");
        check(highlight(L"mermaid", L"graph TD; A-->B").empty(), "unknown languages stay plain");
    }
    auto colors = parse_document("```cpp\nint total = 42; // answer\nreturn say(\"hi\");\n```\n");
    bool builtinFirst = false, keywordSecond = false, commentSliced = false, tokensOnly = true;
    std::wstring joined;
    for (const auto &b : colors->blocks) {
        if (b.kind != Kind::Code)
            continue;
        joined += b.text.value + L'\n';
        for (const auto &s : b.text.spans) {
            if (s.style == TokBuiltin && s.start == 0 && s.length == 3)
                builtinFirst = true;
            if (s.style == TokKeyword && s.start == 0 && s.length == 6)
                keywordSecond = true;
            if (s.style == TokComment && s.start == 16 && s.length == 9)
                commentSliced = true;
            if (s.style & ~kTokenStyles)
                tokensOnly = false;
        }
    }
    check(builtinFirst && keywordSecond && commentSliced && tokensOnly &&
              joined == L"int total = 42; // answer\nreturn say(\"hi\");\n" &&
              search(*colors, L"answer").size() == 1,
          "code token spans attach without changing searchable text");
    std::string longLine = "```cpp\n// " + std::string(9000, 'x') + "\n```\n";
    auto split = parse_document(longLine);
    bool tailComment = false, splitGroup = false;
    for (size_t i = 0; i + 1 < split->blocks.size(); ++i)
        if (split->blocks[i].kind == Kind::Code)
            splitGroup = split->blocks[i].joins_next || splitGroup;
    for (const auto &b : split->blocks)
        if (b.kind == Kind::Code && !b.joins_next)
            for (const auto &s : b.text.spans)
                if (s.style == TokComment && s.start == 0 && s.length == b.text.value.size())
                    tailComment = true;
    check(splitGroup && tailComment, "overlong code lines keep their token spans after re-splitting");

    std::cout << checks << " checks, " << failures << " failures\n";
    if (SUCCEEDED(com))
        CoUninitialize();
    return failures ? 1 : 0;
}
