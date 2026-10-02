#include "settings.h"
#include "document.h"
#include "file_io.h"
#include <algorithm>
#include <sstream>
namespace keepmd {
void ReaderSettings::remember(const std::filesystem::path &path, size_t block) {
    if (path.empty())
        return;
    auto normal = path.lexically_normal();
    std::erase_if(recent, [&](const auto &value) { return value.path == normal; });
    recent.insert(recent.begin(), {normal, block});
    if (recent.size() > 10)
        recent.resize(10);
}
size_t ReaderSettings::position(const std::filesystem::path &path) const {
    auto normal = path.lexically_normal();
    for (const auto &value : recent)
        if (value.path == normal)
            return value.block;
    return 0;
}
ReaderSettings load_settings(const std::filesystem::path &path) {
    ReaderSettings settings;
    FileData file;
    std::wstring error;
    if (!load_file(path, file, error))
        return settings;
    std::istringstream input(file.utf8);
    std::string line;
    RecentFile values[10];
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        auto key = line.substr(0, equals), value = line.substr(equals + 1);
        try {
            if (key == "Dark")
                settings.dark = value == "1";
            else if (key == "Toolbar")
                settings.toolbar = value != "0";
            else if (key == "Zoom")
                settings.zoom = std::clamp(std::stoi(value) / 1000.f, .65f, 2.5f);
            else if (key == "Width")
                settings.reading_width = std::clamp((float)std::stoi(value), 400.f, 1600.f);
            else if (key.starts_with("Recent")) {
                auto i = std::stoul(key.substr(6));
                if (i < 10)
                    values[i].path = wide(value);
            } else if (key.starts_with("Position")) {
                auto i = std::stoul(key.substr(8));
                if (i < 10)
                    values[i].block = std::min<size_t>(std::stoull(value), 10000000);
            }
        } catch (...) { /* Ignore a malformed preference, preserving other entries. */
        }
    }
    for (auto &value : values)
        if (!value.path.empty())
            settings.recent.push_back(std::move(value));
    return settings;
}
bool store_settings(const std::filesystem::path &path, const ReaderSettings &settings) {
    std::wstring text = L"[Reader]\nDark=" + std::to_wstring(settings.dark ? 1 : 0) + L"\nZoom=" +
                        std::to_wstring((int)(settings.zoom * 1000)) + L"\nWidth=" +
                        std::to_wstring((int)settings.reading_width) + L"\nToolbar=" +
                        std::to_wstring(settings.toolbar) + L"\n";
    for (size_t i = 0; i < settings.recent.size() && i < 10; ++i) {
        text += L"Recent" + std::to_wstring(i) + L"=" + settings.recent[i].path.wstring() + L"\nPosition" +
                std::to_wstring(i) + L"=" + std::to_wstring(settings.recent[i].block) + L"\n";
    }
    std::wstring error;
    return save_file(path, text, Encoding::Utf8, error);
}
} // namespace keepmd
