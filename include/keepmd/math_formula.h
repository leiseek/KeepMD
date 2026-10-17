#pragma once
#include <d2d1.h>
#include <memory>
#include <string>

namespace keepmd {

// A LaTeX formula typeset by the pinned MicroTeX against embedded Computer Modern fonts and drawn with
// Direct2D. Metrics are in layout units; the baseline sits `ascent` below the top.
class MathFormula {
  public:
    // Longest source accepted; longer formulas stay as their source text, like oversized diagrams.
    static constexpr size_t kMaxSource = 8 * 1024;

    // Returns nullptr when MicroTeX cannot typeset `source` (the text between the $ delimiters); `error`
    // then names the reason. `display` selects TeX display style for $$...$$, text style otherwise.
    static std::shared_ptr<MathFormula> create(const std::wstring &source, bool display, float size,
                                               std::wstring *error = nullptr);

    float width = 0, ascent = 0, descent = 0;

    // Draws with the top-left corner at (x, y) in the target's current coordinate space, leaving the
    // target's transform as it was.
    void draw(ID2D1RenderTarget *target, float x, float y, D2D1_COLOR_F color) const;

    ~MathFormula();

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    float size_ = 0;
    MathFormula();
};

} // namespace keepmd
