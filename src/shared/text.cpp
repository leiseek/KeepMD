#include "text.h"
#include <climits>
#include <windows.h>

namespace keep {
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
} // namespace keep
