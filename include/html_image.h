#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace keepmd {
struct HtmlImage {
    std::wstring source, alt;
    uint32_t width = 0, height = 0;
};
// A bounded, standalone img tag subset; never interprets HTML/CSS or scripts.
std::optional<HtmlImage> parse_html_image(std::wstring_view text);
struct ImageExtent {
    float width = 0, height = 0;
};
ImageExtent image_extent(uint32_t bitmapWidth, uint32_t bitmapHeight, uint32_t requestedWidth,
                         uint32_t requestedHeight, float usable, float zoom);
} // namespace keepmd
