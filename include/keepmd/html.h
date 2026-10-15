#pragma once
#include "document.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace keepmd {
struct HtmlTag {
    std::wstring name;                                             // ASCII lower case
    std::vector<std::pair<std::wstring, std::wstring>> attributes; // Lower-case names; first duplicate wins.
    bool closing = false, self_closing = false;
    const std::wstring *attribute(std::wstring_view lowerName) const;
};
enum class HtmlToken { Text, Tag, Comment, Invalid };
// Reads the token at `cursor` and advances past it. As in CommonMark, a "<" that cannot start markup is
// text. Declarations, processing instructions, CDATA, malformed or oversized tags and unterminated
// comments are Invalid so callers keep the original source.
HtmlToken next_html_token(std::wstring_view text, size_t &cursor, HtmlTag &tag);

// The README subset of HTML. Never interprets CSS, scripts or event attributes.
enum class HtmlRole {
    Unsupported,
    Inline, // Formatting or a transparent wrapper.
    Link,
    Break,
    Image,
    Hidden, // Void elements without visible content.
    // Block roles follow; each one starts a new native block.
    Container,
    Paragraph,
    Heading,
    List,
    Item,
    Quote,
    Rule,
    Summary
};
HtmlRole html_role(std::wstring_view name);
uint32_t html_style(std::wstring_view name);
int html_align(const HtmlTag &tag); // Block::text_align from align= or <center>; 0 inherits.

// A div/center/details left open by one HTML block also aligns the Markdown blocks that follow.
struct HtmlContainer {
    std::wstring name;
    int align = 0;
};
// Converts one raw HTML block into native blocks. Returns false, leaving `containers` unchanged, when the
// block needs markup outside the subset (tables, scripts, unknown or malformed tags).
bool render_html_block(const Block &raw, std::vector<HtmlContainer> &containers, std::vector<Block> &blocks);
// Keeps container nesting balanced across a block that is shown as source instead.
void track_html_containers(std::wstring_view html, std::vector<HtmlContainer> &containers);
} // namespace keepmd
