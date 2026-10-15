#pragma once
#include "keepmd.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace keepmd {
struct HtmlTag;
struct HtmlImage {
    std::wstring source, alt;
    uint32_t width = 0, height = 0;
};
// A bounded, standalone img tag subset; never interprets HTML/CSS or scripts.
std::optional<HtmlImage> parse_html_image(std::wstring_view text);
// The img attributes of an already parsed tag; rejects closing tags, other elements and unusable src.
std::optional<HtmlImage> html_image(const HtmlTag &tag);
struct ImageExtent {
    float width = 0, height = 0;
};
ImageExtent image_extent(uint32_t bitmapWidth, uint32_t bitmapHeight, uint32_t requestedWidth,
                         uint32_t requestedHeight, float usable, float zoom);
} // namespace keepmd
