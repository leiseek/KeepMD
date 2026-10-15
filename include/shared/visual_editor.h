#pragma once
#include <memory>
#include <string>
#include <windows.h>
namespace keep {
// UTF-16 units one visual editor holds; each refresh restyles the whole document.
constexpr size_t VisualLimit = 1024 * 1024;
enum class VisualCommand { Paragraph, H1, H2, H3, Bold, Italic, Bullet, Numbered, Quote, Code, Diagram };
class VisualEditor {
  public:
    VisualEditor(HWND parent, int id);
    ~VisualEditor();
    HWND hwnd() const;
    bool valid() const;
    void load(std::wstring_view source);
    void replace_document(std::wstring_view source);
    std::wstring text(bool crlf = false) const;
    void saved();
    void theme(bool dark);
    // `column` limits the text width in DIPs and centres it; 0 uses the full width.
    void inset(unsigned dpi, int column = 0);
    void changed();
    void refresh();
    void command(VisualCommand command);
    bool composing() const;
    bool internal() const;
    void clear();
    // Markdown (logical) offsets, excluding embedded diagram objects.
    size_t caret() const;
    void reveal(size_t offset);

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace keep
