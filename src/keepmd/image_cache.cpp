#include "image_cache.h"
#include "text.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <mutex>
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

namespace keepmd {
namespace {
constexpr std::array<uint8_t, 8> Magic{'K','M','D','I','M','G','0','1'};
using Digest = std::array<uint8_t, 32>;
std::optional<Digest> digest(std::span<const uint8_t> bytes) {
    Digest value{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                   (ULONG)bytes.size(), value.data(), (ULONG)value.size()) < 0)
        return {};
    return value;
}
std::mutex diskMutex;
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct UrlLock {
    Handle handle{nullptr};
    bool owned = false;
    UrlLock(const std::wstring &key, const std::atomic_bool &cancel) {
        auto name = L"Local\\KeepMD.ImageCache." + key;
        handle.value = CreateMutexW(nullptr, FALSE, name.c_str());
        auto until = GetTickCount64() + 20000;
        while (handle.value && !cancel.load() && GetTickCount64() < until) {
            auto result = WaitForSingleObject(handle.value, 50);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { owned = true; break; }
            if (result != WAIT_TIMEOUT) break;
        }
    }
    ~UrlLock() { if (owned) ReleaseMutex(handle.value); }
};
std::vector<uint8_t> read_cache(const std::filesystem::path &file) {
    std::error_code ec;
    auto size = std::filesystem::file_size(file, ec);
    if (ec || size <= Magic.size() + Digest{}.size() || size > RemoteImageLimit + Magic.size() + Digest{}.size())
        return {};
    std::ifstream input(file, std::ios::binary);
    std::array<uint8_t, 8> magic{};
    Digest saved{};
    input.read(reinterpret_cast<char *>(magic.data()), magic.size());
    input.read(reinterpret_cast<char *>(saved.data()), saved.size());
    if (!input || magic != Magic)
        return {};
    std::vector<uint8_t> bytes((size_t)size - magic.size() - saved.size());
    input.read(reinterpret_cast<char *>(bytes.data()), (std::streamsize)bytes.size());
    auto actual = digest(bytes);
    if (!input || !actual || *actual != saved)
        return {};
    return bytes;
}
void prune(const std::filesystem::path &directory, const std::filesystem::path &keep) {
    struct File { std::filesystem::path path; uint64_t size; std::filesystem::file_time_type used; };
    std::vector<File> files;
    uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto &entry = *it;
        auto stem = entry.path().stem().wstring();
        if (entry.path().extension() != L".kmi" || stem.size() != 64 ||
            !std::all_of(stem.begin(), stem.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }))
            continue;
        std::error_code error;
        if (!entry.is_regular_file(error) || error) continue;
        auto size = entry.file_size(error);
        if (error) continue;
        auto used = entry.last_write_time(error);
        if (error) continue;
        files.push_back({entry.path(), size, used});
        total += size;
    }
    std::sort(files.begin(), files.end(), [](const File &a, const File &b) { return a.used < b.used; });
    size_t count = files.size();
    for (const auto &file : files) {
        if (total <= ImageCacheLimit && count <= ImageCacheEntries) break;
        if (file.path == keep) continue;
        if (std::filesystem::remove(file.path, ec)) { total -= file.size; --count; }
    }
}
void write_cache(const std::filesystem::path &file, std::span<const uint8_t> bytes) {
    auto hash = digest(bytes);
    if (!hash) return;
    std::lock_guard lock(diskMutex);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) return;
    static std::atomic_uint counter{0};
    auto temporary = file;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++counter) + L".tmp";
    bool ok = false;
    {
        Handle output{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE) return;
        auto write = [&](std::span<const uint8_t> data) {
            while (!data.empty()) {
                DWORD done = 0;
                if (!WriteFile(output.value, data.data(), (DWORD)data.size(), &done, nullptr) || !done)
                    return false;
                data = data.subspan(done);
            }
            return true;
        };
        ok = write(Magic) && write(*hash) && write(bytes) && FlushFileBuffers(output.value);
    }
    if (ok)
        ok = MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) std::filesystem::remove(temporary, ec);
    else prune(file.parent_path(), file);
}
} // namespace
std::filesystem::path default_image_cache_directory() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
        return {};
    std::filesystem::path result = std::filesystem::path(path) / L"KeepMD" / L"Cache" / L"Images";
    CoTaskMemFree(path);
    return result;
}
std::wstring image_cache_key(std::wstring_view canonicalUrl) {
    auto text = keep::utf8(canonicalUrl);
    auto hash = digest({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
    if (!hash) return {};
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    for (auto b : *hash) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}
ImagePixels cached_remote_image(const RemoteImageUrl &url, bool allowDownload,
                                const std::filesystem::path &directory, const std::atomic_bool &cancel) {
    if (cancel.load()) return {};
    auto key = image_cache_key(url.url);
    if (key.empty()) return {0, 0, {}, L"无法生成图片缓存标识"};
    UrlLock lock(key, cancel);
    if (!lock.owned) return {0, 0, {}, cancel.load() ? L"加载已取消" : L"图片缓存正在使用，请稍后重试"};
    auto root = directory.empty() ? default_image_cache_directory() : directory;
    auto file = root.empty() ? std::filesystem::path() : root / (key + L".kmi");
    if (!file.empty()) {
        auto bytes = read_cache(file);
        if (!bytes.empty()) {
            auto image = decode_image_bytes(bytes);
            if (!image.pixels.empty()) {
                std::error_code ec;
                std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), ec);
                return image;
            }
        }
        // A bad cache entry must never poison all subsequent opens of the same URL.
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
    if (!allowDownload || cancel.load()) return {};
    auto response = fetch_remote_image(url, cancel);
    if (cancel.load()) return {};
    if (!response.error.empty()) return {0, 0, {}, std::move(response.error)};
    auto image = decode_image_bytes(response.bytes);
    if (!cancel.load() && !image.pixels.empty() && !file.empty())
        write_cache(file, response.bytes);
    return image;
}
} // namespace keepmd
