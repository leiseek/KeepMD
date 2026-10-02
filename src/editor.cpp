#include "editor.h"
#include <algorithm>
#include <commdlg.h>
#include <cwctype>
#include <richedit.h>
#include <vector>

namespace keepmd {
Editor::Editor(HWND parent, int id) {
    library_ = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library_)
        return;
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                            0, 0, 100, 100, parent, reinterpret_cast<HMENU>((INT_PTR)id), nullptr, nullptr);
    if (!hwnd_)
        return;
    SendMessageW(hwnd_, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO, 0);
    SendMessageW(hwnd_, EM_EXLIMITTEXT, 0, 8 * 1024 * 1024);
    SendMessageW(hwnd_, EM_SETUNDOLIMIT, 200, 0);
    SendMessageW(hwnd_, EM_SETEVENTMASK, 0, ENM_CHANGE);
    CHARFORMAT2W style{};
    style.cbSize = sizeof(style);
    style.dwMask = CFM_FACE | CFM_SIZE;
    wcscpy_s(style.szFaceName, L"Consolas");
    style.yHeight = 220;
    SendMessageW(hwnd_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&style));
}
Editor::~Editor() {
    if (hwnd_)
        DestroyWindow(hwnd_);
    if (library_)
        FreeLibrary(library_);
}
void Editor::load(std::wstring_view value) {
    if (!hwnd_)
        return;
    std::wstring copy(value);
    baseline_ = copy;
    SETTEXTEX set{ST_DEFAULT, 1200};
    SendMessageW(hwnd_, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&set), reinterpret_cast<LPARAM>(copy.c_str()));
    SendMessageW(hwnd_, EM_EMPTYUNDOBUFFER, 0, 0);
    saved();
}
std::wstring Editor::text(bool crlf) const {
    if (!hwnd_)
        return {};
    GETTEXTLENGTHEX length{(DWORD)(GTL_PRECISE | GTL_NUMCHARS | (crlf ? GTL_USECRLF : 0)), 1200};
    auto count = SendMessageW(hwnd_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
    if (count < 0 || count > 16 * 1024 * 1024)
        return {};
    std::wstring result((size_t)count + 1, 0);
    GETTEXTEX get{(DWORD)(result.size() * sizeof(wchar_t)), (DWORD)(crlf ? GT_USECRLF : GT_DEFAULT), 1200,
                  nullptr, nullptr};
    auto copied = SendMessageW(hwnd_, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get),
                               reinterpret_cast<LPARAM>(result.data()));
    result.resize((size_t)std::max<LRESULT>(0, copied));
    if (!crlf)
        for (auto &c : result)
            if (c == L'\r')
                c = L'\n';
    return result;
}
std::wstring Editor::text_for_save(bool crlf) const {
    if (!modified())
        return baseline_;
    auto current = text(false);
    std::wstring normalized;
    normalized.reserve(baseline_.size());
    for (size_t i = 0; i < baseline_.size(); ++i) {
        wchar_t c = baseline_[i];
        if (c == L'\r') {
            normalized += L'\n';
            if (i + 1 < baseline_.size() && baseline_[i + 1] == L'\n')
                ++i;
        } else
            normalized += c;
    }
    if (current == normalized)
        return baseline_;
    return crlf ? text(true) : current;
}
bool Editor::modified() const {
    return hwnd_ && SendMessageW(hwnd_, EM_GETMODIFY, 0, 0) != 0;
}
size_t Editor::caret() const {
    CHARRANGE selection{};
    if (hwnd_)
        SendMessageW(hwnd_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    return (size_t)std::max(0L, selection.cpMax);
}
void Editor::saved() {
    if (hwnd_)
        SendMessageW(hwnd_, EM_SETMODIFY, FALSE, 0);
}
void Editor::theme(bool dark) {
    if (!hwnd_)
        return;
    bool dirty = modified();
    SendMessageW(hwnd_, EM_SETBKGNDCOLOR, 0, dark ? RGB(24, 30, 40) : RGB(252, 252, 251));
    CHARFORMAT2W style{};
    style.cbSize = sizeof(style);
    style.dwMask = CFM_COLOR;
    style.crTextColor = dark ? RGB(225, 230, 239) : RGB(35, 46, 62);
    SendMessageW(hwnd_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&style));
    SendMessageW(hwnd_, EM_SETMODIFY, dirty, 0);
}
bool Editor::find(const std::wstring &query, bool previous, bool reset) {
    if (query.empty() || !hwnd_)
        return false;
    CHARRANGE selection{};
    SendMessageW(hwnd_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    FINDTEXTEXW find{};
    find.lpstrText = const_cast<wchar_t *>(query.c_str());
    find.chrg.cpMin = reset ? 0 : previous ? selection.cpMin : selection.cpMax;
    find.chrg.cpMax = previous ? 0 : -1;
    auto result =
        SendMessageW(hwnd_, EM_FINDTEXTEXW, previous ? 0 : FR_DOWN, reinterpret_cast<LPARAM>(&find));
    if (result < 0 && !reset) {
        find.chrg.cpMin = previous ? -1 : 0;
        result = SendMessageW(hwnd_, EM_FINDTEXTEXW, previous ? 0 : FR_DOWN, reinterpret_cast<LPARAM>(&find));
    }
    if (result < 0)
        return false;
    SendMessageW(hwnd_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&find.chrgText));
    SendMessageW(hwnd_, EM_SCROLLCARET, 0, 0);
    return true;
}
size_t Editor::replace(const std::wstring &query, const std::wstring &replacement, bool all) {
    if (query.empty() || !hwnd_)
        return 0;
    if (!all) {
        CHARRANGE range{};
        SendMessageW(hwnd_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range));
        bool selected = range.cpMax - range.cpMin == (LONG)query.size();
        if (selected) {
            std::wstring current(query.size() + 1, 0);
            SendMessageW(hwnd_, EM_GETSELTEXT, 0, reinterpret_cast<LPARAM>(current.data()));
            current.resize(query.size());
            selected = std::equal(current.begin(), current.end(), query.begin(),
                                  [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
        }
        if (!selected && !find(query, false, false))
            return 0;
        SendMessageW(hwnd_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(replacement.c_str()));
        find(query, false, false);
        return 1;
    }
    auto input = text(false);
    auto lower = input;
    auto needle = query;
    for (auto &c : lower)
        c = (wchar_t)towlower(c);
    for (auto &c : needle)
        c = (wchar_t)towlower(c);
    std::wstring output;
    size_t cursor = 0, count = 0, pos = 0;
    while ((pos = lower.find(needle, cursor)) != std::wstring::npos) {
        output.append(input, cursor, pos - cursor);
        output += replacement;
        cursor = pos + needle.size();
        ++count;
        if (output.size() > 8 * 1024 * 1024)
            return 0;
    }
    if (!count)
        return 0;
    output.append(input, cursor, std::wstring::npos);
    if (output.size() > 8 * 1024 * 1024)
        return 0;
    SendMessageW(hwnd_, EM_SETSEL, 0, -1);
    SendMessageW(hwnd_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(output.c_str()));
    return count;
}
} // namespace keepmd
