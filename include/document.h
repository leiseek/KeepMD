#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace keepmd {
std::wstring wide(std::string_view text);
std::string utf8(std::wstring_view text);
std::wstring decode_entities(std::string_view text);
std::wstring slug(std::wstring_view text);
enum Style : uint32_t { Bold = 1, Italic = 2, Strike = 4, Mono = 8, Underline = 16 };
struct Span {
    uint32_t start = 0, length = 0, style = 0;
    std::wstring link;
};
struct Text {
    std::wstring value;
    std::vector<Span> spans;
};
enum class Kind { Paragraph, Heading, Code, Rule, Image, TableRow, Diagram, Html };
struct Block {
    Kind kind = Kind::Paragraph;
    Text text;
    std::vector<Text> cells;
    std::vector<int> align;
    std::wstring target, language, marker;
    uint32_t level = 0, indent = 0, quote = 0;
    uint32_t image_width = 0, image_height = 0;
    size_t source = 0, text_start = 0;
    bool header = false;
    bool joins_next = false; // Physical layout chunks of one logical paragraph.
};
struct Heading {
    std::wstring text, anchor;
    size_t block = 0;
    unsigned level = 0;
};
struct Document {
    std::string source;
    std::vector<Block> blocks;
    std::vector<Heading> headings;
    size_t text_size = 0;
    double parse_ms = 0;
    std::wstring error;
    std::wstring plain_text() const;
    std::wstring text_range(size_t begin, size_t end) const;
};
std::shared_ptr<Document> parse_document(std::string source, const std::atomic_bool *cancel = nullptr);
std::wstring block_plain(const Block &block);
struct Match {
    size_t block = 0;
    uint32_t offset = 0, length = 0;
    size_t absolute = 0;
};
std::vector<Match> search(const Document &doc, std::wstring_view query, size_t limit = 10000);

// Prefix sums keep block lookup logarithmic without retaining all text layouts.
class Heights {
  public:
    void reset(const std::vector<float> &heights);
    void set(size_t index, float height);
    float at(size_t index) const {
        return values_.at(index);
    }
    float prefix(size_t count) const;
    float total() const {
        return prefix(values_.size());
    }
    size_t locate(float y) const;
    size_t size() const {
        return values_.size();
    }

  private:
    std::vector<float> values_, tree_;
};
} // namespace keepmd
