#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace keepmd {
constexpr size_t RemoteImageLimit = 8 * 1024 * 1024;
struct RemoteImageUrl {
    std::wstring url, host, resource;
    uint16_t port = 0;
    bool secure = false;
};
// Parsing never accesses the network. Protocol-relative URLs use HTTPS; credentials are not accepted.
std::optional<RemoteImageUrl> remote_image_url(std::wstring_view source);
struct RemoteImageData {
    std::vector<uint8_t> bytes;
    std::wstring error;
};
// Call only while network images are enabled for the current document. Cancellation closes the request.
RemoteImageData fetch_remote_image(const RemoteImageUrl &url, const std::atomic_bool &cancel);
} // namespace keepmd
