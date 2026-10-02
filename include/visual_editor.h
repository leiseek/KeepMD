#pragma once
#include <memory>
#include <string>
#include <windows.h>
namespace keepmd {
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
    void inset(unsigned dpi);
    void changed();
    void refresh();
    void command(VisualCommand command);
    bool composing() const;
    bool internal() const;
    void clear();

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace keepmd
