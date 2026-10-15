#include "html.h"
#include "html_image.h"
#include <algorithm>
#include <iterator>
#include <optional>

namespace keepmd {
namespace {
constexpr size_t maxTagSize = 16 * 1024;
constexpr size_t maxHtmlBlock = 128 * 1024;
constexpr size_t maxDepth = 64;
constexpr size_t maxContainers = 32;

bool space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f';
}
bool letter(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}
bool name_start(wchar_t c) {
    return letter(c) || c == L'_' || c == L':';
}
bool name_char(wchar_t c) {
    return name_start(c) || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.';
}
// As in CommonMark, "<" only starts markup before a letter, "/letter", "!" or "?".
bool markup_start(std::wstring_view text, size_t cursor) {
    if (cursor + 1 >= text.size() || text[cursor] != L'<')
        return false;
    auto c = text[cursor + 1];
    if (c == L'/')
        return cursor + 2 < text.size() && letter(text[cursor + 2]);
    return c == L'!' || c == L'?' || letter(c);
}
void lowercase(std::wstring &text) {
    for (auto &c : text)
        if (c >= L'A' && c <= L'Z')
            c += L'a' - L'A';
}
std::wstring decoded(std::wstring_view text) {
    return text.find(L'&') == std::wstring_view::npos ? std::wstring(text) : decode_entities(utf8(text));
}
void truncate(Text &text, size_t size) {
    if (size >= text.value.size())
        return;
    text.value.resize(size);
    while (!text.spans.empty() && text.spans.back().start >= size)
        text.spans.pop_back();
    if (!text.spans.empty() && text.spans.back().start + text.spans.back().length > size)
        text.spans.back().length = (uint32_t)(size - text.spans.back().start);
}

// One attribute name and its optional value; `cursor` ends right after them.
bool next_attribute(std::wstring_view text, size_t &cursor, std::wstring &name, std::wstring &value) {
    if (cursor >= text.size() || !name_start(text[cursor]))
        return false;
    size_t end = cursor;
    while (end < text.size() && name_char(text[end]))
        ++end;
    name.assign(text.substr(cursor, end - cursor));
    value.clear();
    size_t equals = text.find_first_not_of(L" \t\r\n\f", end);
    if (equals == std::wstring_view::npos || text[equals] != L'=') {
        cursor = end; // Boolean attribute; the whitespace separates the next one.
        return true;
    }
    cursor = text.find_first_not_of(L" \t\r\n\f", equals + 1);
    if (cursor == std::wstring_view::npos)
        return false;
    if (text[cursor] == L'\'' || text[cursor] == L'"') {
        auto quote = text[cursor++];
        end = text.find(quote, cursor);
        if (end == std::wstring_view::npos)
            return false;
        value.assign(text.substr(cursor, end - cursor));
        cursor = end + 1;
        return true;
    }
    end = cursor;
    while (end < text.size() && !space(text[end]) && text[end] != L'>') {
        auto c = text[end++];
        if (c < 32 || c == L'<' || c == L'\'' || c == L'"' || c == L'=' || c == L'`')
            return false;
    }
    if (end == cursor)
        return false;
    value.assign(text.substr(cursor, end - cursor));
    cursor = end;
    return true;
}
// Reads an open or closing tag whose name starts at `cursor`; `cursor` ends after the tag.
bool parse_tag(std::wstring_view text, size_t &cursor, HtmlTag &tag) {
    if (cursor < text.size() && text[cursor] == L'/') {
        tag.closing = true;
        ++cursor;
    }
    size_t end = cursor;
    while (end < text.size() && name_char(text[end]))
        ++end;
    if (end == cursor || !letter(text[cursor]))
        return false;
    tag.name.assign(text.substr(cursor, end - cursor));
    lowercase(tag.name);
    cursor = end;
    while (cursor < text.size()) {
        bool separated = false;
        while (cursor < text.size() && space(text[cursor])) {
            ++cursor;
            separated = true;
        }
        if (cursor == text.size())
            return false;
        if (text[cursor] == L'>') {
            ++cursor;
            return true;
        }
        if (!tag.closing && text.substr(cursor, 2) == L"/>") {
            tag.self_closing = true;
            cursor += 2;
            return true;
        }
        // End tags take no attributes, and attributes must be separated by whitespace.
        std::wstring name, value;
        if (tag.closing || !separated || !next_attribute(text, cursor, name, value))
            return false;
        lowercase(name);
        if (!tag.attribute(name))
            tag.attributes.emplace_back(std::move(name), decoded(value));
    }
    return false;
}

struct NamedRole {
    std::wstring_view name;
    HtmlRole role;
    uint32_t style;
};
// Sorted by name for binary search. Everything else, including tables, scripts, forms and pre, is
// outside the subset and keeps its source.
constexpr NamedRole roles[] = {
    {L"a", HtmlRole::Link, 0},
    {L"abbr", HtmlRole::Inline, 0},
    {L"address", HtmlRole::Container, 0},
    {L"article", HtmlRole::Container, 0},
    {L"aside", HtmlRole::Container, 0},
    {L"b", HtmlRole::Inline, Bold},
    {L"bdi", HtmlRole::Inline, 0},
    {L"bdo", HtmlRole::Inline, 0},
    {L"big", HtmlRole::Inline, 0},
    {L"blockquote", HtmlRole::Quote, 0},
    {L"br", HtmlRole::Break, 0},
    {L"center", HtmlRole::Container, 0},
    {L"cite", HtmlRole::Inline, Italic},
    {L"code", HtmlRole::Inline, Mono},
    {L"data", HtmlRole::Inline, 0},
    {L"dd", HtmlRole::Container, 0},
    {L"del", HtmlRole::Inline, Strike},
    {L"details", HtmlRole::Container, 0},
    {L"dfn", HtmlRole::Inline, Italic},
    {L"div", HtmlRole::Container, 0},
    {L"dl", HtmlRole::Container, 0},
    {L"dt", HtmlRole::Container, 0},
    {L"em", HtmlRole::Inline, Italic},
    {L"figcaption", HtmlRole::Container, 0},
    {L"figure", HtmlRole::Container, 0},
    {L"font", HtmlRole::Inline, 0},
    {L"footer", HtmlRole::Container, 0},
    {L"h1", HtmlRole::Heading, 0},
    {L"h2", HtmlRole::Heading, 0},
    {L"h3", HtmlRole::Heading, 0},
    {L"h4", HtmlRole::Heading, 0},
    {L"h5", HtmlRole::Heading, 0},
    {L"h6", HtmlRole::Heading, 0},
    {L"header", HtmlRole::Container, 0},
    {L"hgroup", HtmlRole::Container, 0},
    {L"hr", HtmlRole::Rule, 0},
    {L"i", HtmlRole::Inline, Italic},
    {L"img", HtmlRole::Image, 0},
    {L"ins", HtmlRole::Inline, Underline},
    {L"kbd", HtmlRole::Inline, Mono},
    {L"label", HtmlRole::Inline, 0},
    {L"li", HtmlRole::Item, 0},
    {L"main", HtmlRole::Container, 0},
    {L"mark", HtmlRole::Inline, 0},
    {L"nav", HtmlRole::Container, 0},
    {L"nobr", HtmlRole::Inline, 0},
    {L"ol", HtmlRole::List, 0},
    {L"p", HtmlRole::Paragraph, 0},
    {L"picture", HtmlRole::Inline, 0},
    {L"q", HtmlRole::Inline, 0},
    {L"rp", HtmlRole::Inline, 0},
    {L"rt", HtmlRole::Inline, 0},
    {L"ruby", HtmlRole::Inline, 0},
    {L"s", HtmlRole::Inline, Strike},
    {L"samp", HtmlRole::Inline, Mono},
    {L"section", HtmlRole::Container, 0},
    {L"small", HtmlRole::Inline, 0},
    {L"source", HtmlRole::Hidden, 0},
    {L"span", HtmlRole::Inline, 0},
    {L"strike", HtmlRole::Inline, Strike},
    {L"strong", HtmlRole::Inline, Bold},
    {L"sub", HtmlRole::Inline, 0},
    {L"summary", HtmlRole::Summary, 0},
    {L"sup", HtmlRole::Inline, 0},
    {L"time", HtmlRole::Inline, 0},
    {L"tt", HtmlRole::Inline, Mono},
    {L"u", HtmlRole::Inline, Underline},
    {L"ul", HtmlRole::List, 0},
    {L"var", HtmlRole::Inline, Italic},
    {L"wbr", HtmlRole::Hidden, 0},
};
const NamedRole *lookup(std::wstring_view name) {
    auto it = std::lower_bound(std::begin(roles), std::end(roles), name,
                               [](const NamedRole &entry, std::wstring_view key) { return entry.name < key; });
    return it != std::end(roles) && it->name == name ? it : nullptr;
}
bool block_role(HtmlRole role) {
    return role >= HtmlRole::Container;
}
uint32_t list_start(const HtmlTag &tag) {
    const auto *value = tag.attribute(L"start");
    if (!value || value->empty() || value->size() > 9)
        return 1;
    uint32_t start = 0;
    for (auto c : *value) {
        if (c < L'0' || c > L'9')
            return 1;
        start = start * 10 + (uint32_t)(c - L'0');
    }
    return start;
}

// Lays out one HTML block as native blocks, approximating the browser rules that README markup relies
// on: whitespace collapsing, implied paragraph and item ends, and inherited alignment.
class Flow {
  public:
    Flow(const Block &raw, const std::vector<HtmlContainer> &containers)
        : source_(raw.source), indent_(raw.indent), quote_(raw.quote), marker_(raw.marker) {
        for (const auto &container : containers) {
            Element element;
            element.name = container.name;
            element.role = HtmlRole::Container;
            element.align = container.align;
            stack_.push_back(std::move(element));
        }
    }
    bool run(std::wstring_view html) {
        for (size_t cursor = 0; cursor < html.size();) {
            size_t before = cursor;
            HtmlTag tag;
            auto token = next_html_token(html, cursor, tag);
            if (token == HtmlToken::Invalid || cursor <= before)
                return false;
            if (token == HtmlToken::Text)
                add_text(html.substr(before, cursor - before));
            else if (token == HtmlToken::Tag && !handle(tag))
                return false;
        }
        finish_block();
        return true;
    }
    // Containers still open at the end of the block, outermost first.
    std::vector<HtmlContainer> open_containers() const {
        std::vector<HtmlContainer> result;
        for (const auto &element : stack_)
            if (element.role == HtmlRole::Container && result.size() < maxContainers)
                result.push_back({element.name, element.align});
        return result;
    }
    std::vector<Block> &output() {
        return blocks_;
    }

  private:
    struct Element {
        std::wstring name, link;
        HtmlRole role = HtmlRole::Unsupported;
        int align = 0;
        uint32_t style = 0, level = 0, next = 1;
        bool ordered = false;
    };
    bool handle(const HtmlTag &tag) {
        const auto *entry = lookup(tag.name);
        if (!entry)
            return false;
        if (tag.closing)
            return close(tag, entry->role);
        switch (entry->role) {
        case HtmlRole::Break:
            line_break();
            return true;
        case HtmlRole::Hidden:
            return true;
        case HtmlRole::Image:
            return add_image(tag);
        case HtmlRole::Inline:
        case HtmlRole::Link:
            settle();
            return push(tag, *entry);
        default:
            break;
        }
        finish_block();
        close_implied(entry->role == HtmlRole::Item);
        if (entry->role == HtmlRole::Rule) {
            blocks_.push_back(make(Kind::Rule));
            return true;
        }
        if (!push(tag, *entry))
            return false;
        if (entry->role == HtmlRole::Item)
            marker_ = next_marker();
        else if (entry->role == HtmlRole::Summary && marker_.empty())
            marker_ = L"▾"; // Details content is always shown expanded.
        return true;
    }
    bool push(const HtmlTag &tag, const NamedRole &entry) {
        if (tag.self_closing)
            return true; // HTML ignores the slash on non-void elements; an empty element stays bounded.
        if (stack_.size() >= maxDepth)
            return false;
        Element element;
        element.name = tag.name;
        element.role = entry.role;
        element.style = entry.style;
        if (entry.role == HtmlRole::Link) {
            if (const auto *href = tag.attribute(L"href"))
                element.link = *href;
        } else if (entry.role == HtmlRole::Heading)
            element.level = (uint32_t)(tag.name[1] - L'0');
        else if (entry.role == HtmlRole::List) {
            element.ordered = tag.name == L"ol";
            element.next = list_start(tag);
        }
        if (block_role(entry.role))
            element.align = html_align(tag);
        stack_.push_back(std::move(element));
        return true;
    }
    // A block start closes an open paragraph, and a new item also closes the previous one.
    void close_implied(bool item) {
        if (item)
            for (size_t i = stack_.size(); i-- > 0;) {
                const auto &element = stack_[i];
                if (element.role == HtmlRole::Item) {
                    stack_.resize(i);
                    break;
                }
                if (block_role(element.role) && element.role != HtmlRole::Paragraph && element.name != L"div" &&
                    element.name != L"address")
                    break;
            }
        for (size_t i = stack_.size(); i-- > 0;)
            if (stack_[i].role == HtmlRole::Paragraph) {
                stack_.resize(i);
                break;
            }
    }
    bool close(const HtmlTag &tag, HtmlRole role) {
        if (role == HtmlRole::Break) {
            line_break(); // Browsers treat </br> as <br>.
            return true;
        }
        size_t found = stack_.size();
        for (size_t i = stack_.size(); i-- > 0;)
            if (stack_[i].name == tag.name) {
                found = i;
                break;
            }
        if (found == stack_.size()) {
            if (role == HtmlRole::Paragraph)
                finish_block(); // A stray </p> still ends the running paragraph.
            return true;        // Other stray end tags are ignored.
        }
        bool block = false;
        for (size_t i = found; i < stack_.size(); ++i)
            block |= block_role(stack_[i].role);
        if (block)
            finish_block();
        else
            settle(); // A space before an inline end tag keeps that element's style.
        stack_.resize(found);
        if (role == HtmlRole::Item || role == HtmlRole::Summary)
            marker_.clear();
        return true;
    }
    // Runs of HTML whitespace become one space, dropped at block edges and after line breaks.
    void add_text(std::wstring_view raw) {
        for (size_t cursor = 0; cursor < raw.size();) {
            if (space(raw[cursor])) {
                ++cursor;
                // Whitespace also collapses across inline element boundaries.
                pending_ = block_ && !block_->text.value.empty() && block_->text.value.back() != L'\n' &&
                           block_->text.value.back() != L' ';
                continue;
            }
            settle();
            size_t end = cursor;
            while (end < raw.size() && !space(raw[end]))
                ++end;
            append(decoded(raw.substr(cursor, end - cursor)));
            cursor = end;
        }
    }
    void settle() {
        if (pending_) {
            pending_ = false;
            append(L" ");
        }
    }
    void append(std::wstring_view value) {
        if (!block_)
            block_ = make(Kind::Paragraph);
        uint32_t style = 0;
        std::wstring_view link;
        for (const auto &element : stack_) {
            style |= element.style;
            if (!element.link.empty())
                link = element.link;
        }
        auto &spans = block_->text.spans;
        size_t at = block_->text.value.size();
        if (style || !link.empty()) {
            if (!spans.empty() && spans.back().style == style && spans.back().link == link &&
                spans.back().start + spans.back().length == at)
                spans.back().length += (uint32_t)value.size();
            else
                spans.push_back({(uint32_t)at, (uint32_t)value.size(), style, std::wstring(link)});
        }
        block_->text.value += value;
    }
    void line_break() {
        pending_ = false;
        if (!block_)
            return; // A break before any text adds no blank line.
        auto &text = block_->text;
        size_t size = text.value.size();
        while (size && text.value[size - 1] == L' ')
            --size;
        truncate(text, size);
        text.value += L'\n';
    }
    bool add_image(const HtmlTag &tag) {
        auto image = html_image(tag);
        if (!image)
            return false; // An img without a usable src keeps the block as source.
        finish_block();
        auto block = make(Kind::Image);
        block.target = std::move(image->source);
        block.text.value = std::move(image->alt);
        block.image_width = image->width;
        block.image_height = image->height;
        blocks_.push_back(std::move(block));
        return true;
    }
    Block make(Kind kind) {
        Block block;
        block.kind = kind;
        block.source = source_;
        block.indent = indent_;
        block.quote = quote_;
        block.marker = std::move(marker_);
        marker_.clear();
        for (const auto &element : stack_) {
            if (element.role == HtmlRole::List)
                ++block.indent;
            else if (element.role == HtmlRole::Quote)
                ++block.quote;
            else if (element.role == HtmlRole::Heading && kind == Kind::Paragraph) {
                block.kind = Kind::Heading;
                block.level = element.level;
            }
            if (element.align)
                block.text_align = element.align; // The innermost explicit alignment wins.
        }
        return block;
    }
    void finish_block() {
        pending_ = false;
        if (!block_)
            return;
        auto block = std::move(*block_);
        block_.reset();
        size_t size = block.text.value.size();
        while (size && space(block.text.value[size - 1]))
            --size;
        truncate(block.text, size);
        if (!block.text.value.empty())
            blocks_.push_back(std::move(block));
        else if (marker_.empty())
            marker_ = std::move(block.marker); // A list marker survives an empty block.
    }
    std::wstring next_marker() {
        for (size_t i = stack_.size(); i-- > 0;)
            if (stack_[i].role == HtmlRole::List)
                return stack_[i].ordered ? std::to_wstring(stack_[i].next++) + L"." : L"•";
        return L"•";
    }

    std::vector<Element> stack_;
    std::optional<Block> block_;
    std::vector<Block> blocks_;
    size_t source_ = 0;
    uint32_t indent_ = 0, quote_ = 0;
    std::wstring marker_; // The Markdown item's marker, then each <li> or <summary> marker.
    bool pending_ = false;
};
} // namespace

const std::wstring *HtmlTag::attribute(std::wstring_view lowerName) const {
    for (const auto &pair : attributes)
        if (pair.first == lowerName)
            return &pair.second;
    return nullptr;
}
HtmlToken next_html_token(std::wstring_view text, size_t &cursor, HtmlTag &tag) {
    tag = {};
    if (cursor >= text.size())
        return HtmlToken::Invalid;
    if (!markup_start(text, cursor)) {
        auto end = text.find(L'<', cursor + 1);
        cursor = end == std::wstring_view::npos ? text.size() : end;
        return HtmlToken::Text;
    }
    if (text.substr(cursor, 4) == L"<!--") {
        // Searching from the second dash also accepts the empty forms <!--> and <!--->.
        auto end = text.find(L"-->", cursor + 2);
        if (end == std::wstring_view::npos)
            return HtmlToken::Invalid;
        cursor = end + 3;
        return HtmlToken::Comment;
    }
    if (text[cursor + 1] == L'!' || text[cursor + 1] == L'?')
        return HtmlToken::Invalid;
    auto window = text.substr(cursor, std::min(text.size() - cursor, maxTagSize));
    size_t size = 1;
    if (!parse_tag(window, size, tag)) {
        tag = {};
        return HtmlToken::Invalid;
    }
    cursor += size;
    return HtmlToken::Tag;
}
HtmlRole html_role(std::wstring_view name) {
    const auto *entry = lookup(name);
    return entry ? entry->role : HtmlRole::Unsupported;
}
uint32_t html_style(std::wstring_view name) {
    const auto *entry = lookup(name);
    return entry ? entry->style : 0;
}
int html_align(const HtmlTag &tag) {
    if (tag.name == L"center")
        return 2;
    const auto *value = tag.attribute(L"align");
    if (!value)
        return 0;
    std::wstring align = *value;
    lowercase(align);
    align.erase(0, std::min(align.size(), align.find_first_not_of(L" \t\r\n\f")));
    align.erase(std::min(align.size(), align.find_last_not_of(L" \t\r\n\f") + 1));
    return align == L"left" ? 1 : align == L"center" ? 2 : align == L"right" ? 3 : 0;
}
bool render_html_block(const Block &raw, std::vector<HtmlContainer> &containers, std::vector<Block> &blocks) {
    if (raw.text.value.size() > maxHtmlBlock)
        return false;
    Flow flow(raw, containers);
    if (!flow.run(raw.text.value))
        return false;
    containers = flow.open_containers();
    auto &rendered = flow.output();
    blocks.insert(blocks.end(), std::make_move_iterator(rendered.begin()), std::make_move_iterator(rendered.end()));
    return true;
}
void track_html_containers(std::wstring_view html, std::vector<HtmlContainer> &containers) {
    for (size_t cursor = 0; cursor < html.size();) {
        size_t before = cursor;
        HtmlTag tag;
        auto token = next_html_token(html, cursor, tag);
        if (token == HtmlToken::Invalid) {
            cursor = before + 1; // Skip broken markup one character at a time.
            continue;
        }
        if (token != HtmlToken::Tag || tag.self_closing || html_role(tag.name) != HtmlRole::Container)
            continue;
        if (!tag.closing) {
            if (containers.size() < maxContainers)
                containers.push_back({tag.name, html_align(tag)});
            continue;
        }
        for (size_t i = containers.size(); i-- > 0;)
            if (containers[i].name == tag.name) {
                containers.resize(i);
                break;
            }
    }
}
} // namespace keepmd
