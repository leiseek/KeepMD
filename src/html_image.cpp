#include "html_image.h"
#include "document.h"
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
bool equal_ascii(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        auto c = a[i];
        if (c >= L'A' && c <= L'Z')
            c += L'a' - L'A';
        if (c != b[i])
            return false;
    }
    return true;
}
uint32_t dimension(std::wstring_view text) {
    text = trim(text);
    uint32_t value = 0;
    for (auto c : text) {
        if (c < L'0' || c > L'9')
            return 0;
        value = value * 10 + (c - L'0');
        if (value > 16384)
            return 0;
    }
    return value;
}
} // namespace
std::optional<HtmlImage> parse_html_image(std::wstring_view text) {
    if (text.size() > 16 * 1024)
        return std::nullopt;
    text = trim(text);
    if (text.size() < 5 || !equal_ascii(text.substr(0, 4), L"<img") ||
        (!space(text[4]) && text[4] != L'>' && text[4] != L'/'))
        return std::nullopt;
    HtmlImage image;
    bool sourceSeen = false, altSeen = false, widthSeen = false, heightSeen = false;
    size_t cursor = 4;
    while (cursor < text.size()) {
        bool separated = space(text[cursor]);
        while (cursor < text.size() && space(text[cursor]))
            ++cursor;
        if (text.substr(cursor) == L">" || text.substr(cursor) == L"/>") {
            if (image.source.empty() || std::any_of(image.source.begin(), image.source.end(),
                                                    [](wchar_t c) { return c < 32 || c == 127; }))
                return std::nullopt;
            return image;
        }
        if (!separated || cursor == text.size())
            return std::nullopt;
        size_t begin = cursor;
        while (cursor < text.size() && !space(text[cursor]) && text[cursor] != L'=' && text[cursor] != L'/' &&
               text[cursor] != L'>') {
            auto c = text[cursor++];
            if (c < 32 || c == L'<' || c == L'\'' || c == L'"' || c == L'`')
                return std::nullopt;
        }
        if (cursor == begin)
            return std::nullopt;
        auto name = text.substr(begin, cursor - begin);
        size_t nameEnd = cursor;
        while (cursor < text.size() && space(text[cursor]))
            ++cursor;
        std::wstring_view value;
        if (cursor < text.size() && text[cursor] == L'=') {
            ++cursor;
            while (cursor < text.size() && space(text[cursor]))
                ++cursor;
            if (cursor == text.size())
                return std::nullopt;
            auto quote = text[cursor];
            if (quote == L'\'' || quote == L'"') {
                begin = ++cursor;
                cursor = text.find(quote, cursor);
                if (cursor == std::wstring_view::npos)
                    return std::nullopt;
                value = text.substr(begin, cursor++ - begin);
            } else {
                begin = cursor;
                while (cursor < text.size() && !space(text[cursor]) && text[cursor] != L'>') {
                    auto c = text[cursor++];
                    if (c < 32 || c == L'<' || c == L'\'' || c == L'"' || c == L'=' || c == L'`')
                        return std::nullopt;
                }
                if (cursor == begin)
                    return std::nullopt;
                value = text.substr(begin, cursor - begin);
            }
        } else {
            // Leave whitespace for the next (possibly boolean) attribute.
            cursor = nameEnd;
        }
        if (!sourceSeen && equal_ascii(name, L"src")) {
            image.source = decode_entities(utf8(value));
            sourceSeen = true;
        } else if (!altSeen && equal_ascii(name, L"alt")) {
            image.alt = decode_entities(utf8(value));
            altSeen = true;
        } else if (!widthSeen && equal_ascii(name, L"width")) {
            image.width = dimension(decode_entities(utf8(value)));
            widthSeen = true;
        } else if (!heightSeen && equal_ascii(name, L"height")) {
            image.height = dimension(decode_entities(utf8(value)));
            heightSeen = true;
        }
    }
    return std::nullopt;
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
