#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace keepmd {
// Token classes the reader colours inside fenced code blocks. Plain text is not emitted.
enum class Token : uint8_t { Keyword, Builtin, String, Comment, Number, Function, Key };
struct Highlight {
    uint32_t start = 0, length = 0;
    Token token = Token::Keyword;
};
// Whole-block scanner for the fenced languages KeepMD recognizes. Unknown languages, empty
// input and scanning bounds all degrade to "no highlights": code stays plain, never mangled.
std::vector<Highlight> highlight(std::wstring_view language, std::wstring_view code);
}
