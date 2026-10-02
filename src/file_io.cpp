#include "file_io.h"
#include "document.h"
#include <fstream>
#include <limits>
#include <vector>
#include <windows.h>

namespace keepmd {
std::wstring wide(std::string_view text) {
    if (text.empty())
        return {};
    if (text.size() > INT_MAX)
        return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (!count)
        return L"\uFFFD";
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), result.data(), count);
    return result;
}
std::string utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    if (text.size() > INT_MAX)
        return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0,
                                    nullptr, nullptr);
    if (!count)
        return {};
    std::string result(count, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), result.data(), count,
                        nullptr, nullptr);
    return result;
}
bool load_file(const std::filesystem::path &path, FileData &result, std::wstring &error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = L"无法打开文件：" + path.wstring();
        return false;
    }
    if (size > 128ull * 1024 * 1024) {
        error = L"文件超过当前 128 MiB 读取上限。";
        return false;
    }
    const auto stampBefore = std::filesystem::last_write_time(path, ec);
    if (ec) {
        error = L"无法读取文件状态。";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!in || (!bytes.empty() && !in.read(bytes.data(), (std::streamsize)size))) {
        error = L"读取失败，文件可能正在被替换。";
        return false;
    }
    FileData next;
    if (bytes.size() >= 2 && ((uint8_t)bytes[0] == 0xff && (uint8_t)bytes[1] == 0xfe ||
                              (uint8_t)bytes[0] == 0xfe && (uint8_t)bytes[1] == 0xff)) {
        if ((bytes.size() - 2) % 2) {
            error = L"UTF-16 文件长度无效。";
            return false;
        }
        bool le = (uint8_t)bytes[0] == 0xff;
        next.encoding = le ? Encoding::Utf16LE : Encoding::Utf16BE;
        std::wstring text;
        text.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i < bytes.size(); i += 2) {
            unsigned a = (uint8_t)bytes[i], b = (uint8_t)bytes[i + 1];
            text.push_back((wchar_t)(le ? a | (b << 8) : (a << 8) | b));
        }
        next.utf8 = utf8(text);
        if (!text.empty() && next.utf8.empty()) {
            error = L"UTF-16 含无效字符序列。";
            return false;
        }
    } else {
        size_t offset = bytes.starts_with("\xef\xbb\xbf") ? 3 : 0;
        next.encoding = offset ? Encoding::Utf8Bom : Encoding::Utf8;
        if (bytes.size() > offset &&
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset,
                                 (int)(bytes.size() - offset), nullptr, 0)) {
            error = L"无法识别编码，请将文件转换为 UTF-8 或带 BOM 的 UTF-16。";
            return false;
        }
        if (offset)
            bytes.erase(0, offset);
        next.utf8 = std::move(bytes);
    }
    next.crlf = next.utf8.find("\r\n") != std::string::npos;
    next.stamp = std::filesystem::last_write_time(path, ec);
    if (ec || next.stamp != stampBefore || std::filesystem::file_size(path, ec) != size || ec) {
        error = L"文件在读取过程中发生变化，请重试。";
        return false;
    }
    result = std::move(next);
    return true;
}
bool save_file(const std::filesystem::path &path, std::wstring_view text, Encoding encoding,
               std::wstring &error) {
    std::string bytes;
    if (encoding == Encoding::Utf8 || encoding == Encoding::Utf8Bom) {
        bytes = utf8(text);
        if (!text.empty() && bytes.empty()) {
            error = L"文本含无效 Unicode，未保存。";
            return false;
        }
        if (encoding == Encoding::Utf8Bom)
            bytes.insert(0, "\xef\xbb\xbf");
    } else {
        // Validate UTF-16 before writing, including surrogate pairs.
        if (!text.empty() && utf8(text).empty()) {
            error = L"文本含无效 Unicode，未保存。";
            return false;
        }
        bool le = encoding == Encoding::Utf16LE;
        bytes = le ? "\xff\xfe" : "\xfe\xff";
        for (wchar_t c : text) {
            bytes.push_back((char)(le ? c & 0xff : c >> 8));
            bytes.push_back((char)(le ? c >> 8 : c & 0xff));
        }
    }
    // Create in the destination directory so replacement stays on one volume.
    auto temp = path;
    temp += L".keepmd-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
            L".tmp";
    HANDLE file =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建保存临时文件，原文件未修改。";
        return false;
    }
    DWORD written = 0;
    bool ok = bytes.size() <= MAXDWORD &&
              WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) &&
              written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            ok = ReplaceFileW(path.c_str(), temp.c_str(), nullptr, 0, nullptr, nullptr) != FALSE;
        else
            ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) {
        DeleteFileW(temp.c_str());
        error = L"保存失败，原文件未替换。请检查权限或文件是否被占用。";
    }
    return ok;
}
} // namespace keepmd
