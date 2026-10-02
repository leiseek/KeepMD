#pragma once
#include <string>
#include <string_view>
#include <windows.h>
namespace keepmd {
// The optional source editor uses Windows' native plain-text RichEdit control.
// It is instantiated only on demand and does not format or serialize Markdown.
class Editor {
  public:
    Editor(HWND parent, int id);
    ~Editor();
    HWND hwnd() const {
        return hwnd_;
    }
    bool valid() const {
        return hwnd_ != nullptr;
    }
    void load(std::wstring_view text);
    std::wstring text(bool crlf) const;
    std::wstring text_for_save(bool crlf) const;
    bool modified() const;
    size_t caret() const;
    void saved();
    void saved(std::wstring value) {
        baseline_ = std::move(value);
        saved();
    }
    void theme(bool dark);
    void inset(unsigned dpi);
    bool find(const std::wstring &query, bool previous = false, bool reset = false);
    size_t replace(const std::wstring &query, const std::wstring &replacement, bool all);

  private:
    HWND hwnd_ = nullptr;
    HMODULE library_ = nullptr;
    std::wstring baseline_;
};
} // namespace keepmd
