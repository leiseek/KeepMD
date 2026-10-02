#include "document.h"
#include "md4c.h"
extern "C" {
#include "entity.h"
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <limits>
#include <optional>
#include <unordered_map>
#include <windows.h>

namespace keepmd {
static void codepoint(std::wstring &out, unsigned cp) {
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        cp = 0xfffd;
    if (cp > 0xffff) {
        cp -= 0x10000;
        out.push_back((wchar_t)(0xd800 + (cp >> 10)));
        out.push_back((wchar_t)(0xdc00 + (cp & 1023)));
    } else
        out.push_back((wchar_t)cp);
}
std::wstring decode_entities(std::string_view text) {
    std::wstring out;
    size_t cursor = 0;
    while (cursor < text.size()) {
        auto pos = text.find('&', cursor);
        if (pos == std::string_view::npos) {
            out += wide(text.substr(cursor));
            break;
        }
        out += wide(text.substr(cursor, pos - cursor));
        auto end = text.find(';', pos + 1);
        if (end == std::string_view::npos || end - pos > 48) {
            out += L'&';
            cursor = pos + 1;
            continue;
        }
        auto entity = text.substr(pos, end - pos + 1);
        bool decoded = false;
        if (entity.size() > 3 && entity[1] == '#') {
            unsigned value = 0, base = 10;
            size_t digit = 2;
            if (entity[digit] == 'x' || entity[digit] == 'X') {
                base = 16;
                ++digit;
            }
            decoded = digit < entity.size() - 1;
            for (; digit < entity.size() - 1; ++digit) {
                char c = entity[digit];
                unsigned d = c >= '0' && c <= '9'   ? c - '0'
                             : c >= 'a' && c <= 'f' ? c - 'a' + 10
                             : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                    : 99;
                if (d >= base) {
                    decoded = false;
                    break;
                }
                value = std::min(0x110000u, value * base + d);
            }
            if (decoded)
                codepoint(out, value);
        } else if (const auto *value = entity_lookup(entity.data(), entity.size())) {
            codepoint(out, value->codepoints[0]);
            if (value->codepoints[1])
                codepoint(out, value->codepoints[1]);
            decoded = true;
        }
        if (!decoded)
            out += wide(entity);
        cursor = end + 1;
    }
    return out;
}
std::wstring slug(std::wstring_view text) {
    std::wstring result;
    for (auto c : text) {
        if (iswspace(c))
            result += L'-';
        else if (!iswpunct(c) || c == L'-' || c == L'_')
            result += (wchar_t)towlower(c);
    }
    return result;
}
namespace {
size_t scalar_start(std::wstring_view text, size_t pos) {
    if (pos < text.size() && pos > 0 && text[pos] >= 0xdc00 && text[pos] <= 0xdfff &&
        text[pos - 1] >= 0xd800 && text[pos - 1] <= 0xdbff)
        --pos;
    return pos;
}
unsigned scalar_at(std::wstring_view text, size_t pos) {
    if (pos >= text.size())
        return 0;
    unsigned cp = text[pos];
    if (cp >= 0xd800 && cp <= 0xdbff && pos + 1 < text.size() && text[pos + 1] >= 0xdc00 &&
        text[pos + 1] <= 0xdfff)
        cp = 0x10000 + ((cp - 0xd800) << 10) + (text[pos + 1] - 0xdc00);
    return cp;
}
bool extends_cluster(unsigned cp) {
    if ((cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef) ||
        (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0020 && cp <= 0xe007f))
        return true;
    if (cp <= 0xffff) {
        WORD type = 0;
        wchar_t c = (wchar_t)cp;
        GetStringTypeW(CT_CTYPE3, &c, 1, &type);
        return (type & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0;
    }
    return false;
}
size_t safe_fragment_end(std::wstring_view text, size_t begin, size_t proposed) {
    if (proposed >= text.size())
        return text.size();
    size_t end = scalar_start(text, proposed);
    while (end > begin) {
        size_t previous = scalar_start(text, end - 1);
        unsigned right = scalar_at(text, end), left = scalar_at(text, previous);
        if (extends_cluster(right) || right == 0x200d || left == 0x200d) {
            end = previous;
            continue;
        }
        if (right >= 0x1f1e6 && right <= 0x1f1ff) {
            size_t cursor = end, count = 0;
            while (cursor > begin) {
                cursor = scalar_start(text, cursor - 1);
                unsigned cp = scalar_at(text, cursor);
                if (cp < 0x1f1e6 || cp > 0x1f1ff)
                    break;
                ++count;
            }
            if (count % 2) {
                end = previous;
                continue;
            }
        }
        break;
    }
    // A deliberately enormous combining cluster stays intact. It is rare and
    // should not be broken into invalid surrogate/emoji fragments.
    if (end == begin) {
        end = scalar_start(text, proposed);
        while (end < text.size() &&
               (extends_cluster(scalar_at(text, end)) || scalar_at(text, end) == 0x200d ||
                scalar_at(text, scalar_start(text, end - 1)) == 0x200d)) {
            unsigned cp = scalar_at(text, end);
            end += cp > 0xffff ? 2 : 1;
        }
    }
    return end;
}
struct Builder {
    Document &doc;
    const std::atomic_bool *cancel;
    std::optional<Block> current;
    struct List {
        bool ordered;
        unsigned next;
    };
    struct Item {
        std::wstring marker;
        bool used = false;
    };
    struct Format {
        uint32_t style;
        std::wstring link;
    };
    std::vector<List> lists;
    std::vector<Item> items;
    std::vector<Format> formats;
    Format format{};
    uint32_t quote = 0;
    bool tableHeader = false;
    int cell = -1, imageDepth = 0;
    Block image;
    bool imageInCell = false;
    bool stopped() const {
        return cancel && cancel->load(std::memory_order_relaxed);
    }
    void flush() {
        if (!current)
            return;
        auto b = std::move(*current);
        current.reset();
        cell = -1;
        if (b.kind == Kind::Diagram) {
            size_t encoded = 0;
            for (size_t i = 0; i < b.text.value.size() && encoded <= 64 * 1024; ++i) {
                unsigned cp = b.text.value[i];
                if (cp < 0x80)
                    ++encoded;
                else if (cp < 0x800)
                    encoded += 2;
                else if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < b.text.value.size()) {
                    encoded += 4;
                    ++i;
                } else
                    encoded += 3;
            }
            if (encoded > 64 * 1024) {
                Block message;
                message.text.value = L"流程图源码超过 64 KiB，以下显示原始代码。";
                doc.blocks.push_back(std::move(message));
                b.kind = Kind::Code;
            }
        }
        if (b.kind == Kind::Code || b.kind == Kind::Html) {
            // Line blocks keep huge code fences bounded in the text layout cache.
            size_t start = 0;
            while (start < b.text.value.size()) {
                size_t end = b.text.value.find(L'\n', start);
                if (end == std::wstring::npos)
                    end = b.text.value.size();
                size_t contentEnd = end;
                if (contentEnd > start && b.text.value[contentEnd - 1] == L'\r')
                    --contentEnd;
                size_t fragment = start;
                do {
                    size_t fragmentEnd =
                        safe_fragment_end(std::wstring_view(b.text.value).substr(0, contentEnd), fragment,
                                          std::min(fragment + 8192, contentEnd));
                    Block line;
                    line.kind = b.kind;
                    line.indent = b.indent;
                    line.quote = b.quote;
                    line.source = b.source;
                    line.language = b.language;
                    line.joins_next = fragmentEnd < contentEnd;
                    line.text.value = b.text.value.substr(fragment, fragmentEnd - fragment);
                    doc.blocks.push_back(std::move(line));
                    fragment = fragmentEnd;
                } while (fragment < contentEnd);
                start = end + 1;
            }
            if (b.text.value.empty())
                doc.blocks.push_back(std::move(b));
        } else if (b.kind == Kind::Paragraph && b.text.value.size() > 8192) {
            size_t start = 0;
            while (start < b.text.value.size()) {
                size_t end = std::min(start + 8192, b.text.value.size());
                if (end < b.text.value.size()) {
                    // Break at a Unicode space when available; preserve that space in
                    // semantic text so search/copy do not acquire synthetic newlines.
                    size_t boundary = end;
                    while (boundary > start + 4096 && !iswspace(b.text.value[boundary - 1]))
                        --boundary;
                    if (boundary > start + 4096)
                        end = boundary;
                    else {
                        end = safe_fragment_end(b.text.value, start, end);
                    }
                }
                Block chunk;
                chunk.kind = b.kind;
                chunk.indent = b.indent;
                chunk.quote = b.quote;
                chunk.source = b.source;
                chunk.joins_next = end < b.text.value.size();
                if (start == 0)
                    chunk.marker = b.marker;
                chunk.text.value = b.text.value.substr(start, end - start);
                for (const auto &span : b.text.spans) {
                    size_t a = std::max(start, (size_t)span.start),
                           z = std::min(end, (size_t)span.start + span.length);
                    if (a < z)
                        chunk.text.spans.push_back(
                            {(uint32_t)(a - start), (uint32_t)(z - a), span.style, span.link});
                }
                doc.blocks.push_back(std::move(chunk));
                start = end;
            }
        } else if (!b.text.value.empty() || b.kind != Kind::Paragraph) {
            doc.blocks.push_back(std::move(b));
        }
    }
    void begin(Kind kind) {
        flush();
        current.emplace();
        current->kind = kind;
        current->indent = (uint32_t)lists.size();
        current->quote = quote;
        if (!items.empty() && !items.back().used) {
            current->marker = items.back().marker;
            items.back().used = true;
        }
    }
    Text &target() {
        if (!current)
            begin(Kind::Paragraph);
        if (cell >= 0)
            return current->cells[(size_t)cell];
        return current->text;
    }
    void append(std::wstring value, const char *source) {
        if (imageDepth && !imageInCell) {
            image.text.value += value;
            return;
        }
        auto &text = target();
        auto address = reinterpret_cast<uintptr_t>(source),
             base = reinterpret_cast<uintptr_t>(doc.source.data());
        if (current->text.value.empty() && address >= base && address - base < doc.source.size())
            current->source = address - base;
        if (format.style || !format.link.empty()) {
            if (!text.spans.empty() && text.spans.back().style == format.style &&
                text.spans.back().link == format.link &&
                text.spans.back().start + text.spans.back().length == text.value.size())
                text.spans.back().length += (uint32_t)value.size();
            else
                text.spans.push_back(
                    {(uint32_t)text.value.size(), (uint32_t)value.size(), format.style, format.link});
        }
        text.value += value;
    }
};
int enter_block(MD_BLOCKTYPE type, void *detail, void *data) {
    auto &b = *static_cast<Builder *>(data);
    if (b.stopped())
        return 1;
    switch (type) {
    case MD_BLOCK_QUOTE:
        b.flush();
        ++b.quote;
        break;
    case MD_BLOCK_UL:
        b.flush();
        b.lists.push_back({false, 0});
        break;
    case MD_BLOCK_OL:
        b.flush();
        b.lists.push_back({true, static_cast<MD_BLOCK_OL_DETAIL *>(detail)->start});
        break;
    case MD_BLOCK_LI: {
        b.flush();
        auto *li = static_cast<MD_BLOCK_LI_DETAIL *>(detail);
        std::wstring marker = L"•";
        if (li->is_task)
            marker = li->task_mark == ' ' ? L"☐" : L"☑";
        else if (!b.lists.empty() && b.lists.back().ordered)
            marker = std::to_wstring(b.lists.back().next++) + L".";
        b.items.push_back({std::move(marker)});
        break;
    }
    case MD_BLOCK_H:
        b.begin(Kind::Heading);
        b.current->level = static_cast<MD_BLOCK_H_DETAIL *>(detail)->level;
        break;
    case MD_BLOCK_P:
        b.begin(Kind::Paragraph);
        break;
    case MD_BLOCK_HR:
        b.begin(Kind::Rule);
        break;
    case MD_BLOCK_CODE: {
        auto *code = static_cast<MD_BLOCK_CODE_DETAIL *>(detail);
        auto language = std::string_view(code->lang.text ? code->lang.text : "", code->lang.size);
        b.begin(language == "mermaid" ? Kind::Diagram : Kind::Code);
        b.current->language = wide(language);
        break;
    }
    case MD_BLOCK_HTML:
        b.begin(Kind::Html);
        break;
    case MD_BLOCK_TABLE:
        b.flush();
        break;
    case MD_BLOCK_THEAD:
        b.tableHeader = true;
        break;
    case MD_BLOCK_TBODY:
        b.tableHeader = false;
        break;
    case MD_BLOCK_TR:
        b.begin(Kind::TableRow);
        b.current->header = b.tableHeader;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (!b.current)
            b.begin(Kind::TableRow);
        b.current->cells.emplace_back();
        b.cell = (int)b.current->cells.size() - 1;
        b.current->align.push_back((int)static_cast<MD_BLOCK_TD_DETAIL *>(detail)->align);
        break;
    default:
        break;
    }
    return 0;
}
int leave_block(MD_BLOCKTYPE type, void *, void *data) {
    auto &b = *static_cast<Builder *>(data);
    switch (type) {
    case MD_BLOCK_QUOTE:
        b.flush();
        if (b.quote)
            --b.quote;
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        b.flush();
        if (!b.lists.empty())
            b.lists.pop_back();
        break;
    case MD_BLOCK_LI:
        b.flush();
        if (!b.items.empty())
            b.items.pop_back();
        break;
    case MD_BLOCK_P:
    case MD_BLOCK_H:
    case MD_BLOCK_CODE:
    case MD_BLOCK_HTML:
    case MD_BLOCK_HR:
    case MD_BLOCK_TR:
    case MD_BLOCK_DOC:
        b.flush();
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        b.cell = -1;
        break;
    default:
        break;
    }
    return b.stopped() ? 1 : 0;
}
int enter_span(MD_SPANTYPE type, void *detail, void *data) {
    auto &b = *static_cast<Builder *>(data);
    b.formats.push_back(b.format);
    switch (type) {
    case MD_SPAN_EM:
        b.format.style |= Italic;
        break;
    case MD_SPAN_STRONG:
        b.format.style |= Bold;
        break;
    case MD_SPAN_DEL:
        b.format.style |= Strike;
        break;
    case MD_SPAN_CODE:
        b.format.style |= Mono;
        break;
    case MD_SPAN_A: {
        auto &attr = static_cast<MD_SPAN_A_DETAIL *>(detail)->href;
        b.format.link = decode_entities({attr.text, attr.size});
        break;
    }
    case MD_SPAN_IMG:
        if (!b.imageDepth) {
            b.imageInCell = b.cell >= 0;
            if (!b.imageInCell) {
                b.flush();
                b.image = {};
                b.image.kind = Kind::Image;
                b.image.indent = (uint32_t)b.lists.size();
                b.image.quote = b.quote;
                auto &attr = static_cast<MD_SPAN_IMG_DETAIL *>(detail)->src;
                b.image.target = decode_entities({attr.text, attr.size});
            }
        }
        ++b.imageDepth;
        break;
    default:
        break;
    }
    return 0;
}
int leave_span(MD_SPANTYPE type, void *, void *data) {
    auto &b = *static_cast<Builder *>(data);
    if (type == MD_SPAN_IMG && b.imageDepth > 0 && --b.imageDepth == 0 && !b.imageInCell)
        b.doc.blocks.push_back(std::move(b.image));
    if (!b.formats.empty()) {
        b.format = std::move(b.formats.back());
        b.formats.pop_back();
    }
    return 0;
}
int text_callback(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *data) {
    auto &b = *static_cast<Builder *>(data);
    if (b.stopped())
        return 1;
    if (type == MD_TEXT_NULLCHAR)
        b.append(L"\ufffd", text);
    else if (type == MD_TEXT_BR)
        b.append(L"\n", text);
    else if (type == MD_TEXT_SOFTBR)
        b.append(L" ", text);
    else if (type == MD_TEXT_ENTITY)
        b.append(decode_entities({text, size}), text);
    else
        b.append(wide({text, size}), text);
    return 0;
}
} // namespace
std::shared_ptr<Document> parse_document(std::string source, const std::atomic_bool *cancel) {
    const auto start = std::chrono::steady_clock::now();
    auto doc = std::make_shared<Document>();
    doc->source = std::move(source);
    Builder builder{*doc, cancel};
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = enter_block;
    parser.leave_block = leave_block;
    parser.enter_span = enter_span;
    parser.leave_span = leave_span;
    parser.text = text_callback;
    if (doc->source.size() > UINT_MAX ||
        md_parse(doc->source.data(), (MD_SIZE)doc->source.size(), &parser, &builder)) {
        doc->error = cancel && cancel->load() ? L"已取消" : L"Markdown 解析失败";
    }
    std::unordered_map<std::wstring, unsigned> anchors;
    size_t offset = 0;
    for (size_t i = 0; i < doc->blocks.size(); ++i) {
        auto &b = doc->blocks[i];
        b.text_start = offset;
        offset += block_plain(b).size() + (b.joins_next ? 0 : 1);
        if (b.kind == Kind::Heading) {
            auto anchor = slug(b.text.value);
            auto count = anchors[anchor]++;
            if (count)
                anchor += L"-" + std::to_wstring(count);
            doc->headings.push_back({b.text.value, std::move(anchor), i, b.level});
        }
    }
    doc->text_size = offset;
    doc->parse_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return doc;
}
std::wstring block_plain(const Block &block) {
    if (block.kind != Kind::TableRow)
        return block.text.value;
    std::wstring result;
    for (size_t i = 0; i < block.cells.size(); ++i) {
        if (i)
            result += L'\t';
        result += block.cells[i].value;
    }
    return result;
}
std::wstring Document::plain_text() const {
    return text_range(0, text_size);
}
std::wstring Document::text_range(size_t begin, size_t end) const {
    if (begin > end)
        std::swap(begin, end);
    std::wstring result;
    if (begin >= end)
        return result;
    auto it = std::upper_bound(blocks.begin(), blocks.end(), begin,
                               [](size_t offset, const Block &block) { return offset < block.text_start; });
    if (it != blocks.begin())
        --it;
    for (; it != blocks.end() && it->text_start < end; ++it) {
        auto text = block_plain(*it);
        if (!it->joins_next)
            text += L'\n';
        const size_t from = begin > it->text_start ? begin - it->text_start : 0;
        const size_t to = std::min(text.size(), end - it->text_start);
        if (from < to)
            result.append(text, from, to - from);
    }
    return result;
}
std::vector<Match> search(const Document &doc, std::wstring_view query, size_t limit) {
    std::vector<Match> results;
    if (query.empty())
        return results;
    std::wstring lower(query);
    for (auto &c : lower)
        c = (wchar_t)towlower(c);
    for (size_t i = 0; i < doc.blocks.size() && results.size() < limit; ++i) {
        auto text = block_plain(doc.blocks[i]);
        const size_t ownLength = text.size();
        size_t remaining = query.size() - 1;
        for (size_t next = i + 1; remaining && next < doc.blocks.size() && doc.blocks[next - 1].joins_next;
             ++next) {
            auto extra = block_plain(doc.blocks[next]);
            size_t take = std::min(remaining, extra.size());
            text.append(extra, 0, take);
            remaining -= take;
        }
        for (auto &c : text)
            c = (wchar_t)towlower(c);
        size_t pos = 0;
        while ((pos = text.find(lower, pos)) != std::wstring::npos) {
            if (pos >= ownLength)
                break;
            results.push_back({i, (uint32_t)pos, (uint32_t)query.size(), doc.blocks[i].text_start + pos});
            if (results.size() >= limit)
                break;
            pos += query.size();
        }
    }
    return results;
}
void Heights::reset(const std::vector<float> &heights) {
    values_ = heights;
    tree_.assign(heights.size() + 1, 0);
    for (size_t i = 1; i < tree_.size(); ++i) {
        tree_[i] += heights[i - 1];
        size_t parent = i + (i & (~i + 1));
        if (parent < tree_.size())
            tree_[parent] += tree_[i];
    }
}
void Heights::set(size_t index, float height) {
    if (index >= values_.size())
        return;
    height = std::max(1.0f, height);
    float delta = height - values_[index];
    values_[index] = height;
    for (size_t i = index + 1; i < tree_.size(); i += i & (~i + 1))
        tree_[i] += delta;
}
float Heights::prefix(size_t count) const {
    float sum = 0;
    for (size_t i = std::min(count, values_.size()); i; i -= i & (~i + 1))
        sum += tree_[i];
    return sum;
}
size_t Heights::locate(float y) const {
    if (values_.empty())
        return 0;
    size_t index = 0, step = 1;
    while (step < tree_.size())
        step <<= 1;
    float sum = 0;
    for (; step; step >>= 1) {
        size_t next = index + step;
        if (next < tree_.size() && sum + tree_[next] <= y) {
            index = next;
            sum += tree_[next];
        }
    }
    return std::min(index, values_.size() - 1);
}
} // namespace keepmd
