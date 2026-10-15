#pragma once
#include "image_decode.h"
#include "remote_image.h"
#include <filesystem>

namespace keepmd {
constexpr uint64_t ImageCacheLimit = 256ull * 1024 * 1024;
constexpr size_t ImageCacheEntries = 1024;
std::filesystem::path default_image_cache_directory();
std::wstring image_cache_key(std::wstring_view canonicalUrl);
// Cache miss with downloads disabled returns empty pixels and no error. All disk/HTTP work stays on
// workers. Only verified, successfully decoded images are stored; write failure doesn't prevent display.
ImagePixels cached_remote_image(const RemoteImageUrl &url, bool allowDownload,
                                const std::filesystem::path &directory, const std::atomic_bool &cancel);
} // namespace keepmd
