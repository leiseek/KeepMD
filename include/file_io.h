#pragma once
#include <filesystem>
#include <string>
#include <string_view>
namespace keepmd {
enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE };
struct FileData {
    std::string utf8;
    Encoding encoding = Encoding::Utf8;
    bool crlf = false;
    std::filesystem::file_time_type stamp{};
};
bool load_file(const std::filesystem::path &path, FileData &result, std::wstring &error);
bool save_file(const std::filesystem::path &path, std::wstring_view text, Encoding encoding,
               std::wstring &error);
} // namespace keepmd
