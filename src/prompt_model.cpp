#include "prompt_model.h"
#include "document.h"
#include "file_io.h"
#include <algorithm>
#include <cwctype>
#include <map>
#include <sstream>

namespace keepmd {
bool parse_prompt_hotkey(std::wstring text, PromptHotkey &out, std::wstring &error) {
    std::erase_if(text, [](wchar_t c) { return iswspace(c) != 0; });
    for (auto &c : text)
        c = (wchar_t)towlower(c);
    if (text == L"lctrlx2" || text == L"leftctrlx2" || text == L"ctrlx2" || text == L"lctrl*2" ||
        text == L"双击左ctrl") {
        out = {true, 0, 0, L"LCtrl x2"};
        return true;
    }
    PromptHotkey next;
    std::wstring keyName;
    std::wistringstream input(text);
    std::wstring part;
    const std::map<std::wstring, UINT> named = {
        {L"space", VK_SPACE},  {L"tab", VK_TAB},       {L"enter", VK_RETURN},  {L"escape", VK_ESCAPE},
        {L"esc", VK_ESCAPE},   {L"up", VK_UP},         {L"down", VK_DOWN},     {L"left", VK_LEFT},
        {L"right", VK_RIGHT},  {L"insert", VK_INSERT}, {L"delete", VK_DELETE}, {L"home", VK_HOME},
        {L"end", VK_END},      {L"pageup", VK_PRIOR},  {L"pagedown", VK_NEXT}, {L"`", VK_OEM_3},
        {L"-", VK_OEM_MINUS},  {L"=", VK_OEM_PLUS},    {L"[", VK_OEM_4},       {L"]", VK_OEM_6},
        {L"\\", VK_OEM_5},     {L";", VK_OEM_1},       {L"'", VK_OEM_7},       {L",", VK_OEM_COMMA},
        {L".", VK_OEM_PERIOD}, {L"/", VK_OEM_2}};
    bool valid = !text.empty() && text.back() != L'+';
    while (std::getline(input, part, L'+')) {
        UINT modifier = part == L"ctrl" || part == L"control" ? MOD_CONTROL
                        : part == L"alt"                      ? MOD_ALT
                        : part == L"shift"                    ? MOD_SHIFT
                        : part == L"win"                      ? MOD_WIN
                                                              : 0;
        if (modifier) {
            valid &= (next.modifiers & modifier) == 0;
            next.modifiers |= modifier;
            continue;
        }
        UINT key = 0;
        if (part.size() == 1 &&
            ((part[0] >= L'a' && part[0] <= L'z') || (part[0] >= L'0' && part[0] <= L'9')))
            key = (UINT)towupper(part[0]);
        else if (auto it = named.find(part); it != named.end())
            key = it->second;
        else {
            for (int i = 1; i <= 11; ++i)
                if (part == L"f" + std::to_wstring(i))
                    key = VK_F1 + i - 1;
        }
        valid &= key != 0 && next.key == 0;
        next.key = key;
        keyName = part;
    }
    // Shift-only combinations would intercept normal typing globally.
    if (!valid || !next.key || !(next.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN))) {
        error = L"请输入 Ctrl+Alt+Space 等组合键，或 LCtrl x2。组合需含 Ctrl、Alt 或 Win；F12 为系统保留。";
        return false;
    }
    if (next.modifiers & MOD_CONTROL)
        next.label += L"Ctrl+";
    if (next.modifiers & MOD_ALT)
        next.label += L"Alt+";
    if (next.modifiers & MOD_SHIFT)
        next.label += L"Shift+";
    if (next.modifiers & MOD_WIN)
        next.label += L"Win+";
    if (!keyName.empty())
        keyName[0] = (wchar_t)towupper(keyName[0]);
    next.label += keyName;
    out = std::move(next);
    return true;
}
bool CtrlTap::event(bool leftCtrl, bool keyDown, uint64_t now, bool otherHeld) {
    if (!leftCtrl || otherHeld) {
        released = 0;
        clean = false;
        if (leftCtrl)
            down = keyDown;
        return false;
    }
    if (keyDown) {
        if (!down) {
            down = clean = true;
            pressed = now;
        }
        return false;
    }
    if (!down)
        return false;
    down = false;
    if (!clean || now - pressed > 350) {
        released = 0;
        return false;
    }
    bool fire = released && now - released <= 350;
    released = fire ? 0 : now;
    return fire;
}
PromptSettings load_prompt_settings(const std::filesystem::path &path) {
    PromptSettings settings;
    FileData data;
    std::wstring error;
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > 8192 || ec || !load_file(path, data, error))
        return settings;
    std::istringstream in(data.utf8);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto p = line.find('=');
        if (p == std::string::npos)
            continue;
        auto key = line.substr(0, p), value = line.substr(p + 1);
        if (key == "Hotkey")
            settings.hotkey = wide(value);
        else if (key == "Resident")
            settings.resident = value == "1";
        else if (key == "Top")
            settings.top = value == "1";
        else if (key == "Preview")
            settings.preview = value == "1";
        else if (key == "Opacity") {
            try {
                settings.opacity = std::clamp(std::stoi(value), 30, 100);
            } catch (...) {
            }
        }
    }
    PromptHotkey parsed;
    if (!parse_prompt_hotkey(settings.hotkey, parsed, error))
        settings.hotkey = L"Ctrl+Alt+Space";
    return settings;
}
bool save_prompt_settings(const std::filesystem::path &path, const PromptSettings &s, std::wstring &error) {
    return save_file(path,
                     L"[Prompt]\nHotkey=" + s.hotkey + L"\nResident=" + std::to_wstring(s.resident) +
                         L"\nTop=" + std::to_wstring(s.top) + L"\nPreview=" + std::to_wstring(s.preview) +
                         L"\nOpacity=" + std::to_wstring(s.opacity) + L"\n",
                     Encoding::Utf8, error);
}
namespace {
// Bounded, strict flat-object parser for Prompt Flow's five-field JSON file.
// Unknown scalar keys are tolerated; nested JSON is intentionally rejected.
struct Json {
    std::wstring s;
    size_t i = 0;
    void space() {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n'))
            ++i;
    }
    bool eat(wchar_t c) {
        space();
        if (i == s.size() || s[i] != c)
            return false;
        ++i;
        return true;
    }
    bool string(std::wstring &out) {
        if (!eat(L'"'))
            return false;
        out.clear();
        while (i < s.size()) {
            wchar_t c = s[i++];
            if (c == L'"')
                return true;
            if (c < 32)
                return false;
            if (c == L'\\') {
                if (i == s.size())
                    return false;
                c = s[i++];
                switch (c) {
                case L'"':
                case L'\\':
                case L'/':
                    break;
                case L'b':
                    c = L'\b';
                    break;
                case L'f':
                    c = L'\f';
                    break;
                case L'n':
                    c = L'\n';
                    break;
                case L'r':
                    c = L'\r';
                    break;
                case L't':
                    c = L'\t';
                    break;
                case L'u': {
                    unsigned n = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (i == s.size())
                            return false;
                        wchar_t d = s[i++];
                        int v = d >= L'0' && d <= L'9'   ? d - L'0'
                                : d >= L'a' && d <= L'f' ? d - L'a' + 10
                                : d >= L'A' && d <= L'F' ? d - L'A' + 10
                                                         : -1;
                        if (v < 0)
                            return false;
                        n = n * 16 + v;
                    }
                    c = (wchar_t)n;
                    if (!c)
                        return false; // Win32 text controls cannot preserve embedded NUL.
                    break;
                }
                default:
                    return false;
                }
            }
            out += c;
        }
        return false;
    }
};
} // namespace
bool import_prompt_flow(std::string_view json, PromptSettings &settings, std::wstring &draft,
                        std::wstring &error) {
    error = L"Prompt Flow 配置无效或超出 1 Mi 字符上限，未更改当前草稿。";
    if (json.size() > 8 * PromptLimit || (!json.empty() && wide(json) == L"\uFFFD"))
        return false;
    Json in{wide(json)};
    PromptSettings next = settings;
    std::wstring prompt;
    std::map<std::wstring, bool> seen;
    if (!in.eat(L'{'))
        return false;
    if (!in.eat(L'}')) {
        do {
            std::wstring key, value;
            if (!in.string(key) || !in.eat(L':') || seen.contains(key))
                return false;
            seen[key] = true;
            in.space();
            bool quoted = in.i < in.s.size() && in.s[in.i] == L'"';
            if (quoted) {
                if (!in.string(value))
                    return false;
            } else {
                size_t begin = in.i;
                while (in.i < in.s.size() && in.s[in.i] != L',' && in.s[in.i] != L'}' &&
                       !iswspace(in.s[in.i]))
                    ++in.i;
                value = in.s.substr(begin, in.i - begin);
                if (value != L"true" && value != L"false" && value != L"null" &&
                    (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos))
                    return false;
            }
            if (key == L"Prompt") {
                if (!quoted)
                    return false;
                prompt = value;
            } else if (key == L"Hotkey") {
                if (!quoted)
                    return false;
                next.hotkey = value;
            } else if (key == L"AlwaysOnTop") {
                if (quoted || (value != L"true" && value != L"false"))
                    return false;
                next.top = value == L"true";
            } else if (key == L"Opacity") {
                if (quoted)
                    return false;
                try {
                    next.opacity = std::stoi(value);
                } catch (...) {
                    return false;
                }
                if (next.opacity < 30 || next.opacity > 100)
                    return false;
            }
        } while (in.eat(L','));
        if (!in.eat(L'}'))
            return false;
    }
    in.space();
    PromptHotkey parsed;
    if (in.i != in.s.size() || !seen.contains(L"Prompt") || prompt.size() > PromptLimit ||
        (!prompt.empty() && utf8(prompt).empty()) || !parse_prompt_hotkey(next.hotkey, parsed, error))
        return false;
    settings = std::move(next);
    draft = std::move(prompt);
    error.clear();
    return true;
}
std::wstring prompt_profile_id(const std::filesystem::path &path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    auto text = (ec ? path.lexically_normal() : normalized).wstring();
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : text) {
        hash ^= (uint16_t)towlower(c);
        hash *= 1099511628211ull;
    }
    return std::to_wstring(hash);
}
std::wstring prompt_startup_command(const std::filesystem::path &exe, const std::filesystem::path &config) {
    return L"\"" + exe.wstring() + L"\" --resident --config \"" + config.wstring() + L"\"";
}
bool prompt_startup_enabled(const std::wstring &value, const std::wstring &command, const std::wstring &key) {
    std::wstring data(32768, 0);
    DWORD bytes = (DWORD)(data.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, data.data(),
                     &bytes) != ERROR_SUCCESS)
        return false;
    data.resize(wcsnlen(data.c_str(), data.size()));
    return data == command;
}
bool set_prompt_startup(const std::wstring &value, const std::wstring &command, bool enable,
                        std::wstring &error, const std::wstring &key) {
    HKEY handle = nullptr;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                     &handle, nullptr);
    if (result == ERROR_SUCCESS) {
        result = enable ? RegSetValueExW(handle, value.c_str(), 0, REG_SZ, (const BYTE *)command.c_str(),
                                         (DWORD)((command.size() + 1) * sizeof(wchar_t)))
                        : RegDeleteValueW(handle, value.c_str());
        if (!enable && result == ERROR_FILE_NOT_FOUND)
            result = ERROR_SUCCESS;
        RegCloseKey(handle);
    }
    if (result != ERROR_SUCCESS)
        error = L"无法更新当前用户的开机启动项，错误 " + std::to_wstring(result);
    return result == ERROR_SUCCESS;
}
} // namespace keepmd
