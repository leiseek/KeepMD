#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace keepmd {
enum VisualStyle : uint16_t { VHidden = 1, VBold = 2, VItalic = 4, VStrike = 8, VCode = 16, VLink = 32 };
struct VisualParagraph {
    size_t start = 0, end = 0;
    unsigned heading = 0, list = 0, quote = 0;
    bool code = false;
};
struct VisualDiagram {
    size_t start = 0, end = 0;
    std::wstring source;
};
struct VisualPlan {
    std::wstring source;
    std::vector<uint16_t> styles;
    std::vector<VisualParagraph> paragraphs;
    std::vector<VisualDiagram> diagrams;
};
VisualPlan visual_plan(std::wstring source);
std::string rtf_escape(std::wstring_view value);
} // namespace keepmd
