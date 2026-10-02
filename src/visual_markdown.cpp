#include "visual_markdown.h"
#include "document.h"
#include "md4c.h"
#include <algorithm>
#include <cwctype>

namespace keepmd {
namespace {
struct Parser {
    VisualPlan &plan;
    std::string bytes;
    std::vector<size_t> map;
    std::vector<uint16_t> stack;
    uint16_t style = 0;
    unsigned code = 0;
};
int block_in(MD_BLOCKTYPE type, void *, void *data) {
    auto &p = *(Parser *)data;
    if (type == MD_BLOCK_CODE)
        ++p.code;
    return 0;
}
int block_out(MD_BLOCKTYPE type, void *, void *data) {
    auto &p = *(Parser *)data;
    if (type == MD_BLOCK_CODE)
        --p.code;
    return 0;
}
int span_in(MD_SPANTYPE type, void *, void *data) {
    auto &p = *(Parser *)data;
    p.stack.push_back(p.style);
    if (type == MD_SPAN_STRONG)
        p.style |= VBold;
    if (type == MD_SPAN_EM)
        p.style |= VItalic;
    if (type == MD_SPAN_DEL)
        p.style |= VStrike;
    if (type == MD_SPAN_CODE)
        p.style |= VCode;
    if (type == MD_SPAN_A)
        p.style |= VLink;
    return 0;
}
int span_out(MD_SPANTYPE, void *, void *data) {
    auto &p = *(Parser *)data;
    if (!p.stack.empty()) {
        p.style = p.stack.back();
        p.stack.pop_back();
    }
    return 0;
}
int text(MD_TEXTTYPE, const char *value, MD_SIZE size, void *data) {
    auto &p = *(Parser *)data;
    auto address = (uintptr_t)value, begin = (uintptr_t)p.bytes.data();
    if (address < begin || address + size > begin + p.bytes.size())
        return 0;
    size_t a = p.map[address - begin], z = p.map[address - begin + size];
    auto style = p.style | (p.code ? VCode : 0);
    for (size_t i = a; i < z; ++i)
        p.plan.styles[i] = (uint16_t)style;
    return 0;
}
} // namespace
VisualPlan visual_plan(std::wstring source) {
    VisualPlan plan;
    // RichEdit uses one CR per paragraph. Normalize only line endings, never trim content.
    for (size_t i = 0; i < source.size(); ++i) {
        wchar_t c = source[i];
        if (c == L'\r') {
            c = L'\n';
            if (i + 1 < source.size() && source[i + 1] == L'\n')
                ++i;
        }
        plan.source += c;
    }
    plan.styles.resize(plan.source.size(), VHidden);
    Parser p{plan, utf8(plan.source)};
    p.map.resize(p.bytes.size() + 1);
    size_t byte = 0;
    for (size_t i = 0; i < plan.source.size();) {
        size_t n = 1;
        if (plan.source[i] >= 0xd800 && plan.source[i] <= 0xdbff && i + 1 < plan.source.size())
            n = 2;
        auto encoded = utf8(std::wstring_view(plan.source).substr(i, n));
        for (size_t k = 0; k < encoded.size(); ++k)
            p.map[byte++] = i;
        i += n;
        p.map[byte] = i;
    }
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = block_in;
    parser.leave_block = block_out;
    parser.enter_span = span_in;
    parser.leave_span = span_out;
    parser.text = text;
    if (md_parse(p.bytes.data(), (MD_SIZE)p.bytes.size(), &parser, &p) != 0)
        std::fill(plan.styles.begin(), plan.styles.end(), uint16_t(0));
    bool fence = false, mermaid = false;
    wchar_t fenceChar = 0;
    size_t fenceLength = 0, diagramStart = 0, bodyStart = 0;
    for (size_t a = 0; a <= plan.source.size();) {
        auto end = plan.source.find(L'\n', a);
        if (end == std::wstring::npos)
            end = plan.source.size();
        VisualParagraph para{a, end};
        size_t i = a;
        while (i < end && (plan.source[i] == L' ' || plan.source[i] == L'\t'))
            ++i;
        size_t lead = i;
        if (i < end && (plan.source[i] == L'`' || plan.source[i] == L'~')) {
            auto ch = plan.source[i];
            size_t z = i;
            while (z < end && plan.source[z] == ch)
                ++z;
            bool closingTail = plan.source.substr(z, end - z).find_first_not_of(L" \t") == std::wstring::npos;
            if (i - a <= 3 && z - i >= 3 &&
                (!fence || (ch == fenceChar && z - i >= fenceLength && closingTail))) {
                if (!fence) {
                    fence = true;
                    fenceChar = ch;
                    fenceLength = z - i;
                    auto language = plan.source.substr(z, end - z);
                    auto first = language.find_first_not_of(L" \t");
                    auto last = language.find_last_not_of(L" \t");
                    mermaid =
                        first != std::wstring::npos && language.substr(first, last - first + 1) == L"mermaid";
                    diagramStart = a;
                    bodyStart = std::min(end + 1, plan.source.size());
                } else {
                    if (mermaid && plan.diagrams.size() < 16)
                        plan.diagrams.push_back({diagramStart, std::min(end + 1, plan.source.size()),
                                                 plan.source.substr(bodyStart, a - bodyStart)});
                    fence = false;
                    mermaid = false;
                }
                for (size_t k = a; k < std::min(end + 1, plan.source.size()); ++k)
                    plan.styles[k] = VHidden;
                para.code = true;
                plan.paragraphs.push_back(para);
                if (end == plan.source.size())
                    break;
                a = end + 1;
                continue;
            }
        }
        if (fence) {
            para.code = true;
            for (size_t k = a; k < end; ++k)
                plan.styles[k] = VCode;
        } else {
            while (i < end && plan.source[i] == L'>') {
                ++para.quote;
                ++i;
                while (i < end && plan.source[i] == L' ')
                    ++i;
            }
            size_t h = i;
            while (h < end && plan.source[h] == L'#' && h - i < 6)
                ++h;
            if (h > i && (h == end || plan.source[h] == L' '))
                para.heading = (unsigned)(h - i);
            if (i + 1 < end && (plan.source[i] == L'-' || plan.source[i] == L'*' || plan.source[i] == L'+') &&
                plan.source[i + 1] == L' ')
                para.list = 1;
            size_t n = i;
            while (n < end && iswdigit(plan.source[n]))
                ++n;
            if (n > i && n + 1 < end && (plan.source[n] == L'.' || plan.source[n] == L')') &&
                plan.source[n + 1] == L' ')
                para.list = 2;
            // Preserve whitespace in ordinary prose, including indentation and empty paragraphs.
            for (size_t k = a; k < end; ++k)
                if (iswspace(plan.source[k]))
                    plan.styles[k] &= ~VHidden;
            size_t prefix = para.heading     ? std::min(h + 1, end)
                            : para.list == 1 ? i + 2
                            : para.list == 2 ? n + 2
                            : para.quote     ? i
                                             : lead;
            if (para.heading || para.list || para.quote)
                for (size_t k = a; k < prefix; ++k)
                    plan.styles[k] = VHidden;
        }
        if (end < plan.styles.size())
            plan.styles[end] = para.code ? VCode : 0;
        plan.paragraphs.push_back(para);
        if (end == plan.source.size())
            break;
        a = end + 1;
    }
    // Incomplete fences remain editable code until a closing fence exists.
    for (const auto &d : plan.diagrams)
        for (size_t i = d.start; i < d.end; ++i)
            plan.styles[i] = VHidden;
    return plan;
}
std::string rtf_escape(std::wstring_view value) {
    std::string out;
    for (auto c : value) {
        if (c == L'\n' || c == L'\r')
            out += "\\par\n";
        else if (c == L'\t')
            out += "\\tab ";
        else if (c == L'{' || c == L'}' || c == L'\\') {
            out += '\\';
            out += (char)c;
        } else if (c >= 32 && c < 127)
            out += (char)c;
        else
            out += "\\u" + std::to_string((int)(int16_t)c) + "?";
    }
    return out;
}
} // namespace keepmd
