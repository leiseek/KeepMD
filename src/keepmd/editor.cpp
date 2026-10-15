#include "editor.h"
#include "ui.h"
#include <algorithm>
#include <commdlg.h>
#include <cwctype>
#include <richedit.h>
#include <vector>

namespace keepmd {
namespace {
std::wstring lf(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        wchar_t c = value[i];
        if (c == L'\r') {
            result += L'\n';
            if (i + 1 < value.size() && value[i + 1] == L'\n')
                ++i;
        } else
            result += c;
    }
    return result;
}
bool find_text(HWND hwnd, const std::wstring &query, bool previous, bool reset) {
    if (query.empty() || !hwnd)
        return false;
    CHARRANGE selection{};
    SendMessageW(hwnd, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    FINDTEXTEXW find{};
    find.lpstrText = const_cast<wchar_t *>(query.c_str());
    find.chrg.cpMin = reset ? 0 : previous ? selection.cpMin : selection.cpMax;
    find.chrg.cpMax = previous ? 0 : -1;
    auto result = SendMessageW(hwnd, EM_FINDTEXTEXW, previous ? 0 : FR_DOWN, reinterpret_cast<LPARAM>(&find));
    if (result < 0 && !reset) {
        find.chrg.cpMin = previous ? -1 : 0;
        result = SendMessageW(hwnd, EM_FINDTEXTEXW, previous ? 0 : FR_DOWN, reinterpret_cast<LPARAM>(&find));
    }
    if (result < 0)
        return false;
    SendMessageW(hwnd, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&find.chrgText));
    SendMessageW(hwnd, EM_SCROLLCARET, 0, 0);
    return true;
}

// Plain-text RichEdit: no formatting, exact source.
class SourceEditor final : public Editor {
  public:
    SourceEditor(HWND parent, int id) {
        library_ = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_)
            return;
        hwnd_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
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
        wcscpy_s(style.szFaceName, ui::code_font());
        style.yHeight = 220;
        SendMessageW(hwnd_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&style));
        ui::attach_scrollbars(hwnd_, ui::ScrollKind::Editor);
    }
    ~SourceEditor() override {
        if (hwnd_)
            DestroyWindow(hwnd_);
        if (library_)
            FreeLibrary(library_);
    }
    HWND hwnd() const override {
        return hwnd_;
    }
    bool visual() const override {
        return false;
    }
    void inset(unsigned dpi, int) override {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        int padding = MulDiv(18, dpi, 96);
        rect.left += padding;
        rect.right = std::max(rect.left + 1, rect.right - padding);
        rect.top += padding;
        rect.bottom = std::max(rect.top + 1, rect.bottom - padding);
        SendMessageW(hwnd_, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&rect));
    }
    void load(std::wstring_view value) override {
        if (!hwnd_)
            return;
        std::wstring copy(value);
        baseline_ = copy;
        SETTEXTEX set{ST_DEFAULT, 1200};
        SendMessageW(hwnd_, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&set), reinterpret_cast<LPARAM>(copy.c_str()));
        SendMessageW(hwnd_, EM_EMPTYUNDOBUFFER, 0, 0);
        mark_saved();
    }
    std::wstring text(bool crlf) const override {
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
    bool modified() const override {
        return hwnd_ && SendMessageW(hwnd_, EM_GETMODIFY, 0, 0) != 0;
    }
    size_t caret() const override {
        CHARRANGE selection{};
        if (hwnd_)
            SendMessageW(hwnd_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        return (size_t)std::max(0L, selection.cpMax);
    }
    void theme(bool dark) override {
        if (!hwnd_)
            return;
        bool dirty = modified();
        auto colors = ui::palette(dark);
        SendMessageW(hwnd_, EM_SETBKGNDCOLOR, 0, colors.surface);
        CHARFORMAT2W style{};
        style.cbSize = sizeof(style);
        style.dwMask = CFM_COLOR;
        style.crTextColor = colors.text;
        SendMessageW(hwnd_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&style));
        SendMessageW(hwnd_, EM_SETMODIFY, dirty, 0);
        ui::scroll_theme(hwnd_, dark);
    }
    bool find(const std::wstring &query, bool previous, bool reset) override {
        return find_text(hwnd_, query, previous, reset);
    }
    size_t replace(const std::wstring &query, const std::wstring &replacement, bool all) override {
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
    void reveal(size_t offset) override {
        CHARRANGE range{(LONG)offset, (LONG)offset};
        SendMessageW(hwnd_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
        SendMessageW(hwnd_, EM_SCROLLCARET, 0, 0);
        ui::sync_scrollbars(hwnd_);
    }

  protected:
    void mark_saved() override {
        if (hwnd_)
            SendMessageW(hwnd_, EM_SETMODIFY, FALSE, 0);
    }
    void mark_modified() override {
        if (hwnd_)
            SendMessageW(hwnd_, EM_SETMODIFY, TRUE, 0);
    }

  private:
    HWND hwnd_ = nullptr;
    HMODULE library_ = nullptr;
};

// WYSIWYG Markdown: headings, emphasis, lists, quotes, code and diagrams are styled in place while the stored
// text stays the exact Markdown source.
class VisualDocumentEditor final : public Editor {
  public:
    VisualDocumentEditor(HWND parent, int id) : editor_(parent, id) {}
    HWND hwnd() const override {
        return editor_.hwnd();
    }
    bool visual() const override {
        return true;
    }
    void load(std::wstring_view value) override {
        baseline_ = std::wstring(value);
        editor_.load(value);
    }
    std::wstring text(bool crlf) const override {
        return editor_.text(crlf);
    }
    bool modified() const override {
        return forced_ || editor_.text(false) != normalized_baseline();
    }
    size_t caret() const override {
        return editor_.caret();
    }
    void theme(bool dark) override {
        editor_.theme(dark);
    }
    void inset(unsigned dpi, int column) override {
        editor_.inset(dpi, column);
    }
    bool find(const std::wstring &query, bool previous, bool reset) override {
        return find_text(editor_.hwnd(), query, previous, reset);
    }
    void reveal(size_t offset) override {
        editor_.reveal(offset);
    }
    void format(keep::VisualCommand command) override {
        editor_.command(command);
    }
    bool internal() const override {
        return editor_.internal();
    }
    void changed() override {
        forced_ = false;
        editor_.changed();
    }

  protected:
    void mark_saved() override {
        forced_ = false;
        editor_.saved();
    }
    void mark_modified() override {
        forced_ = true;
    }

  private:
    keep::VisualEditor editor_;
    bool forced_ = false;
};
} // namespace
std::wstring Editor::normalized_baseline() const {
    return lf(baseline_);
}
std::wstring Editor::text_for_save(bool crlf) const {
    if (!modified())
        return baseline_;
    auto current = text(false);
    if (current == normalized_baseline())
        return baseline_;
    return crlf ? text(true) : current;
}
void Editor::adopt(std::wstring_view value, std::wstring baseline) {
    load(value);
    bool changed = lf(value) != lf(baseline);
    baseline_ = std::move(baseline);
    if (changed)
        mark_modified();
    else
        mark_saved();
}
bool Editor::find(const std::wstring &query, bool previous, bool reset) {
    return find_text(hwnd(), query, previous, reset);
}
std::unique_ptr<Editor> source_editor(HWND parent, int id) {
    return std::make_unique<SourceEditor>(parent, id);
}
std::unique_ptr<Editor> visual_editor(HWND parent, int id) {
    return std::make_unique<VisualDocumentEditor>(parent, id);
}
bool visual_editable(std::wstring_view text) {
    return text.size() <= VisualDocumentLimit && text.find(L'\ufffc') == std::wstring_view::npos;
}
} // namespace keepmd
