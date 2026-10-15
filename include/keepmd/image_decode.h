#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace keepmd {

// Why an image could not be shown. Callers surface `error` to the reader; the failure kind lets tests and
// future diagnostics tell a missing file apart from a format the decoders cannot handle.
enum class ImageFailure {
    None,
    Missing,        // No such file: the path resolved to nothing on disk.
    Unreadable,     // The file exists but could not be opened (permissions, sharing, I/O).
    Unsupported,    // Opened, but no decoder accepted it: wrong or unsupported format, or corrupt data.
    TooLarge,       // Beyond the preview limits below.
    SvgUnsupported, // An SVG relying on content Direct2D would silently drop (text, style sheets, masks...).
};

// Message shown in the reader for a failure kind. `name` is the file name for path-based failures and is
// ignored by the others. Returns an empty string for ImageFailure::None.
std::wstring image_failure_message(ImageFailure failure, const std::wstring &name = {});

struct ImagePixels {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
    std::wstring error;
    ImageFailure failure = ImageFailure::None;
    // Pixels per layout unit. Raster images are 1; vector images are rasterized above their intrinsic size so
    // they stay sharp when zoomed or on high-DPI displays, and lay out at width / density.
    float density = 1;
};
// Caller initializes COM. Both paths apply identical decoding and pixel/dimension limits. WIC decodes raster
// formats; SVG, which WIC cannot read, falls back to Direct2D's SVG subset. `requestedWidth`/`requestedHeight`
// are an HTML <img> size in layout units (0 = unset): vector images use them to rasterize sharply at the size
// they will be shown, raster images ignore them.
ImagePixels decode_image_file(const std::filesystem::path &path, uint32_t requestedWidth = 0,
                              uint32_t requestedHeight = 0);
ImagePixels decode_image_bytes(std::span<const uint8_t> bytes, uint32_t requestedWidth = 0,
                               uint32_t requestedHeight = 0);

} // namespace keepmd
