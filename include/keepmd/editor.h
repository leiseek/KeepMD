#pragma once
#include "keepmd.h"
#include "visual_editor.h"
#include <memory>
#include <string>
#include <string_view>
#include <windows.h>
namespace keepmd {
// Document editors are created on demand. Both keep the Markdown source exact: the source editor is a
// plain-text RichEdit, the visual editor styles Markdown in place and hides its markers.
class Editor {
  public:
    virtual ~Editor() = default;
    virtual HWND hwnd() const = 0;
    bool valid() const {
        return hwnd() != nullptr;
    }
    virtual bool visual() const = 0;
    virtual void load(std::wstring_view text) = 0;
    virtual std::wstring text(bool crlf) const = 0;
    // The original bytes when the content is unchanged, otherwise the text with the file's line endings.
    std::wstring text_for_save(bool crlf) const;
    virtual bool modified() const = 0;
    virtual size_t caret() const = 0;
    void saved(std::wstring value) {
        baseline_ = std::move(value);
        mark_saved();
    }
    const std::wstring &baseline() const {
        return baseline_;
    }
    // Moves an editing session to this editor, keeping the saved baseline and the modified state.
    void adopt(std::wstring_view text, std::wstring baseline);
    virtual void theme(bool dark) = 0;
    // `column` is the preferred text width in DIPs for prose editing; source editing uses the full width.
    virtual void inset(unsigned dpi, int column) = 0;
    virtual bool find(const std::wstring &query, bool previous = false, bool reset = false);
    virtual size_t replace(const std::wstring &, const std::wstring &, bool) {
        return 0;
    }
    virtual void reveal(size_t offset) = 0;
    virtual void format(keep::VisualCommand) {}
    // Formatting passes of the visual editor raise change notifications that are not user edits.
    virtual bool internal() const {
        return false;
    }
    virtual void changed() {}

  protected:
    virtual void mark_saved() = 0;
    virtual void mark_modified() = 0;
    std::wstring normalized_baseline() const;
    std::wstring baseline_;
};
std::unique_ptr<Editor> source_editor(HWND parent, int id);
std::unique_ptr<Editor> visual_editor(HWND parent, int id);
// Each typing pause restyles the whole document, about 0.55 ms per 1,000 UTF-16 units, so larger documents are
// edited as source to keep typing responsive. This is below the editor's own keep::VisualLimit.
constexpr size_t VisualDocumentLimit = 256 * 1024;
// The visual editor also reserves U+FFFC for diagrams.
bool visual_editable(std::wstring_view text);
} // namespace keepmd
