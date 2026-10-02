#pragma once
#include <filesystem>
#include <vector>
namespace keepmd {
struct RecentFile {
    std::filesystem::path path;
    size_t block = 0;
};
struct ReaderSettings {
    bool dark = false;
    bool toolbar = true;
    float zoom = 1;
    float reading_width = 920;
    std::vector<RecentFile> recent;
    void remember(const std::filesystem::path &path, size_t block);
    size_t position(const std::filesystem::path &path) const;
};
ReaderSettings load_settings(const std::filesystem::path &path);
bool store_settings(const std::filesystem::path &path, const ReaderSettings &settings);
} // namespace keepmd
