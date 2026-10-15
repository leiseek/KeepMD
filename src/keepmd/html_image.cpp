#include "html_image.h"
#include "document.h"
#include "html.h"
#include <algorithm>

namespace keepmd {
namespace {
bool space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f';
}
std::wstring_view trim(std::wstring_view text) {
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}
uint32_t dimension(std::wstring_view text) {
    text = trim(text);
    if (text.empty())
        return 0;
    uint32_t value = 0;
    for (auto c : text) {
        if (c < L'0' || c > L'9')
            return 0;
        value = value * 10 + (c - L'0');
        if (value > 16384)
            return 0; // Oversized or invalid requests fall back to the natural size.
    }
    return value;
}
std::wstring attribute(const HtmlTag &tag, std::wstring_view name) {
    const auto *value = tag.attribute(name);
    return value ? *value : std::wstring();
}
} // namespace
std::optional<HtmlImage> html_image(const HtmlTag &tag) {
    if (tag.closing || tag.name != L"img")
        return std::nullopt;
    HtmlImage image;
    image.source = attribute(tag, L"src");
    image.alt = attribute(tag, L"alt");
    image.width = dimension(attribute(tag, L"width"));
    image.height = dimension(attribute(tag, L"height"));
    if (image.source.empty() ||
        std::any_of(image.source.begin(), image.source.end(), [](wchar_t c) { return c < 32 || c == 127; }))
        return std::nullopt;
    return image;
}
std::optional<HtmlImage> parse_html_image(std::wstring_view text) {
    if (text.size() > 16 * 1024)
        return std::nullopt;
    auto trimmed = trim(text);
    if (trimmed.empty())
        return std::nullopt;
    size_t cursor = 0;
    HtmlTag tag;
    if (next_html_token(trimmed, cursor, tag) != HtmlToken::Tag || cursor != trimmed.size())
        return std::nullopt;
    return html_image(tag);
}
ImageExtent image_extent(uint32_t bitmapWidth, uint32_t bitmapHeight, uint32_t requestedWidth,
                         uint32_t requestedHeight, float usable, float zoom) {
    if (!bitmapWidth || !bitmapHeight)
        return {};
    float width = (float)bitmapWidth, height = (float)bitmapHeight;
    if (requestedWidth) {
        width = requestedWidth * zoom;
        height = requestedHeight ? requestedHeight * zoom : width * bitmapHeight / bitmapWidth;
    } else if (requestedHeight) {
        height = requestedHeight * zoom;
        width = height * bitmapWidth / bitmapHeight;
    }
    float fit = std::min(1.f, std::max(0.f, usable) / width);
    return {width * fit, height * fit};
}
} // namespace keepmd
