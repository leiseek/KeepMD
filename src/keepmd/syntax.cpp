#include "syntax.h"
#include <cwctype>

namespace keepmd {
namespace {
// One scanner driven by per-language rules; the rules stay coarse on purpose. A token class is
// only a colour, so a word the rules cannot place is simply left plain instead of guessed at.
struct Grammar {
    const wchar_t *lineComment = L""; // L"" = none
    const wchar_t *blockOpen = L"", *blockClose = L"";
    const wchar_t *quotes = L"\"'";
    wchar_t escape = L'\\'; // 0 = escapes off
    bool caseInsensitive = false; // keyword matching (SQL)
    bool wordComment = false;     // '#'/';' comments only at a word boundary
    bool tripleQuotes = false;    // Python
    bool stringPrefixes = false;  // Python r"" f"" b"" u""
    bool lifetimes = false;       // Rust 'a is a lifetime, not an unterminated char
    bool sigilVars = false;       // $name variables
    bool numberUnits = false;     // CSS: 12px, 50%
    bool numberSuffix = false;    // C-family: 42u, 1.5f, 0u32
    bool hexColors = false;       // CSS: #abc123
    bool atWords = false;         // CSS: @media
    bool stringKeys = false;      // JSON/YAML: "key": colours the string as a key
    bool yamlKeys = false;
    bool iniKeys = false;
    bool markup = false; // HTML/XML state machine instead of the generic scanner
    const wchar_t *const *keywords = nullptr;
    const wchar_t *const *builtins = nullptr;
};

const wchar_t *const cKeywords[] = {L"break",   L"case",     L"continue", L"default", L"do",
                                    L"else",    L"for",      L"goto",     L"if",      L"return",
                                    L"sizeof",  L"switch",   L"while",    L"static",  nullptr};
const wchar_t *const cBuiltins[] = {L"bool",    L"char",     L"double",  L"float",   L"int",
                                    L"long",    L"short",    L"signed",  L"unsigned", L"void",
                                    L"volatile", L"const",   L"enum",    L"extern",  L"struct",
                                    L"typedef", L"union",    L"register", L"inline",  L"size_t",
                                    L"uint8_t", L"uint16_t", L"uint32_t", L"uint64_t", L"int8_t",
                                    L"int16_t", L"int32_t",  L"int64_t",  nullptr};
const wchar_t *const cppKeywords[] = {
    L"alignas", L"alignof", L"and",      L"auto",     L"break",     L"case",     L"catch",
    L"class",   L"concept", L"const_cast", L"constexpr", L"continue", L"decltype", L"default",
    L"delete",  L"do",      L"dynamic_cast", L"else",   L"explicit",  L"export",  L"false",
    L"final",   L"for",     L"friend",   L"goto",     L"if",       L"inline",   L"mutable",
    L"namespace", L"new",   L"noexcept", L"nullptr",  L"operator",  L"override", L"private",
    L"protected", L"public", L"register", L"reinterpret_cast", L"requires", L"return", L"sizeof",
    L"static",  L"static_cast", L"struct", L"switch",  L"template",  L"this",    L"throw",
    L"true",    L"try",     L"typedef",  L"typeid",   L"typename",  L"union",   L"using",
    L"virtual", L"volatile", L"while",   L"co_await", L"co_return", L"co_yield", nullptr};
const wchar_t *const cppBuiltins[] = {
    L"bool",   L"char",   L"double",  L"float",  L"int",      L"long",     L"short",
    L"signed", L"unsigned", L"void",  L"size_t", L"std",      L"int8_t",   L"int16_t",
    L"int32_t", L"int64_t", L"uint8_t", L"uint16_t", L"uint32_t", L"uint64_t", nullptr};
const wchar_t *const csharpKeywords[] = {
    L"abstract", L"as",       L"async",   L"await",  L"base",     L"break",     L"case",
    L"catch",    L"checked",  L"class",   L"const",  L"default",  L"delegate",  L"do",
    L"double",   L"else",     L"enum",    L"event",  L"explicit", L"extern",    L"false",
    L"finally",  L"fixed",    L"float",   L"foreach", L"get",     L"goto",      L"implicit",
    L"in",       L"init",     L"int",     L"interface", L"internal", L"is",     L"lock",
    L"long",     L"namespace", L"new",    L"null",   L"object",   L"operator",  L"out",
    L"override", L"params",   L"partial", L"private", L"protected", L"public",  L"readonly",
    L"record",   L"ref",      L"required", L"return", L"sealed",  L"set",       L"sizeof",
    L"stackalloc", L"static", L"string",  L"struct", L"switch",   L"this",      L"throw",
    L"true",     L"try",      L"typeof",  L"uint",   L"ulong",    L"unchecked", L"unsafe",
    L"ushort",   L"using",    L"var",     L"virtual", L"void",    L"volatile",  L"when",
    L"where",    L"while",    L"yield",   nullptr};
const wchar_t *const javaKeywords[] = {
    L"abstract", L"assert",   L"boolean", L"break",     L"byte",      L"case",     L"catch",
    L"char",     L"class",    L"const",   L"continue",  L"default",   L"do",       L"double",
    L"else",     L"enum",     L"extends", L"final",     L"finally",   L"float",    L"for",
    L"goto",     L"if",       L"implements", L"import", L"instanceof", L"int",     L"interface",
    L"long",     L"native",   L"new",     L"package",   L"private",   L"protected", L"public",
    L"record",   L"return",   L"sealed",  L"short",     L"static",    L"strictfp", L"super",
    L"switch",   L"synchronized", L"this", L"throw",    L"throws",    L"transient", L"try",
    L"var",      L"void",     L"volatile", L"while",    L"yield",     nullptr};
const wchar_t *const javaBuiltins[] = {L"String", L"Object",    L"Integer",  L"Boolean",  L"Double",
                                       L"Float",  L"Character", L"Byte",     L"Short",    L"Long",
                                       L"List",   L"Map",       L"Set",      L"ArrayList", L"HashMap",
                                       nullptr};
const wchar_t *const jsKeywords[] = {
    L"async",  L"await",  L"break",  L"case",     L"catch",   L"class",   L"const",   L"continue",
    L"debugger", L"default", L"delete", L"do",    L"else",    L"export",  L"extends", L"false",
    L"finally", L"from",  L"function", L"get",   L"if",      L"import",  L"in",      L"instanceof",
    L"let",    L"new",    L"null",   L"of",       L"return",  L"set",     L"static",  L"super",
    L"switch", L"this",   L"throw",  L"true",     L"try",     L"typeof",  L"undefined", L"var",
    L"void",   L"while",  L"with",   L"yield",    nullptr};
const wchar_t *const jsBuiltins[] = {L"console", L"window", L"document", L"Math",   L"JSON",
                                     L"Promise", L"Array",  L"Object",   L"Number", L"Symbol",
                                     L"Map",     L"Set",    L"require",  L"module", L"exports",
                                     L"process", nullptr};
const wchar_t *const tsKeywords[] = {
    L"abstract", L"any",      L"as",       L"asserts",  L"async",    L"await",  L"bigint",
    L"boolean",  L"break",    L"case",     L"catch",    L"class",    L"const",  L"constructor",
    L"continue", L"debugger", L"declare",  L"default",  L"delete",   L"do",     L"else",
    L"enum",     L"export",   L"extends",  L"false",    L"finally",  L"for",    L"from",
    L"function", L"get",      L"if",       L"implements", L"import", L"in",     L"infer",
    L"instanceof", L"interface", L"is",    L"keyof",    L"let",      L"module", L"namespace",
    L"never",    L"new",      L"null",     L"number",   L"object",   L"of",     L"private",
    L"protected", L"public",  L"readonly", L"return",   L"satisfies", L"set",   L"static",
    L"string",   L"super",    L"switch",   L"symbol",   L"this",     L"throw",  L"true",
    L"try",      L"type",     L"typeof",   L"undefined", L"union",   L"unknown", L"var",
    L"void",     L"while",    L"with",     L"yield",    nullptr};
const wchar_t *const pythonKeywords[] = {
    L"and",   L"as",     L"assert", L"async",  L"await", L"break", L"class",  L"continue",
    L"def",   L"del",    L"elif",   L"else",   L"except", L"for",  L"from",   L"global",
    L"if",    L"import", L"in",     L"is",     L"lambda", L"match", L"case",  L"nonlocal",
    L"not",   L"or",     L"pass",   L"raise",  L"return", L"try",  L"while",  L"with",
    L"yield", nullptr};
const wchar_t *const pythonBuiltins[] = {
    L"True",  L"False", L"None",  L"self",  L"cls",   L"print", L"len",    L"range",
    L"str",   L"int",   L"float", L"list",  L"dict",  L"set",   L"tuple",  L"bool",
    L"type",  L"isinstance", L"super", L"open", L"enumerate", L"zip", L"map",
    L"filter", L"sorted", L"sum", L"min", L"max", L"abs",  L"repr",  L"format",
    L"any",   L"all",   L"round", L"input", L"Exception", L"ValueError", L"TypeError",
    L"KeyError", L"StopIteration", nullptr};
const wchar_t *const rustKeywords[] = {
    L"as",     L"async",  L"await", L"break",  L"const",   L"continue", L"crate",  L"dyn",
    L"else",   L"enum",   L"extern", L"false", L"fn",      L"for",      L"if",     L"impl",
    L"in",     L"let",    L"loop",  L"match",  L"mod",     L"move",     L"mut",    L"pub",
    L"ref",    L"return", L"self",  L"Self",  L"static",  L"struct",   L"super",  L"trait",
    L"true",   L"type",   L"union", L"unsafe", L"use",     L"where",    L"while",  nullptr};
const wchar_t *const rustBuiltins[] = {
    L"i8",    L"i16",   L"i32",    L"i64",     L"i128",   L"isize", L"u8",     L"u16",
    L"u32",   L"u64",   L"u128",   L"usize",   L"f32",    L"f64",   L"str",    L"bool",
    L"char",  L"String", L"Vec",   L"Option",  L"Some",   L"None",  L"Result", L"Ok",
    L"Err",   L"Box",   L"Rc",     L"Arc",     L"HashMap", L"BTreeMap", nullptr};
const wchar_t *const goKeywords[] = {
    L"break",  L"case",  L"chan",   L"const",    L"continue", L"default", L"defer",
    L"else",   L"fallthrough", L"for", L"func",  L"go",       L"goto",    L"if",
    L"import", L"interface", L"map", L"package", L"range",    L"return",  L"select",
    L"struct", L"switch", L"type",  L"var",      nullptr};
const wchar_t *const goBuiltins[] = {
    L"nil",    L"true",  L"false", L"iota",  L"append", L"bool",    L"byte",   L"cap",
    L"close",  L"complex", L"copy", L"delete", L"error", L"float32", L"float64", L"imag",
    L"int",    L"int8",  L"int16", L"int32", L"int64", L"len",     L"make",   L"new",
    L"panic",  L"print", L"println", L"real", L"recover", L"rune",  L"string", L"uint",
    L"uint8",  L"uint16", L"uint32", L"uint64", L"uintptr", L"any", L"comparable", nullptr};
const wchar_t *const bashKeywords[] = {
    L"if",     L"then",   L"else",  L"elif",  L"fi",      L"do",     L"done",  L"for",
    L"while",  L"until",  L"case",  L"esac",  L"in",      L"function", L"select", L"time",
    L"coproc", L"return", L"break", L"continue", L"local", L"export", L"readonly", L"declare",
    L"typeset", L"unset", L"shift", L"source", L"alias",  L"echo",   L"exit",  L"eval",
    L"exec",   L"trap",   L"set",   L"printf", L"cd",     L"read",   L"test",  L"true",
    L"false",  L"pushd",  L"popd",  nullptr};
const wchar_t *const powershellKeywords[] = {
    L"begin",  L"break",  L"catch", L"class",  L"continue", L"data",    L"define", L"do",
    L"dynamicparam", L"else", L"elseif", L"end", L"enum",    L"exit",   L"filter", L"finally",
    L"for",    L"foreach", L"from", L"function", L"hidden", L"if",     L"in",     L"param",
    L"process", L"return", L"static", L"switch", L"throw",  L"trap",   L"try",    L"until",
    L"using",  L"var",    L"while", L"workflow", nullptr};
const wchar_t *const powershellBuiltins[] = {L"$true",    L"$false",   L"$null",    L"$args",
                                             L"$input",   L"$HOME",    L"$PROFILE", L"$ErrorActionPreference",
                                             nullptr};
const wchar_t *const sqlKeywords[] = {
    L"add",     L"all",    L"alter",  L"and",     L"any",     L"as",      L"asc",     L"begin",
    L"between", L"by",     L"case",   L"cast",    L"column",  L"commit",  L"constraint", L"create",
    L"cross",   L"database", L"default", L"delete", L"desc",  L"distinct", L"drop",    L"else",
    L"end",     L"exists", L"foreign", L"from",   L"full",    L"group",   L"having",  L"in",
    L"index",   L"inner",  L"insert", L"into",    L"is",      L"join",    L"key",     L"left",
    L"like",    L"limit",  L"not",    L"null",    L"offset",  L"on",      L"or",      L"order",
    L"outer",   L"primary", L"references", L"right", L"rollback", L"select", L"set",   L"table",
    L"then",    L"transaction", L"union", L"unique", L"update", L"values", L"view",   L"when",
    L"where",   L"with",   nullptr};
const wchar_t *const sqlBuiltins[] = {
    L"int",     L"integer", L"smallint", L"bigint",  L"decimal", L"numeric", L"float",
    L"real",    L"double",  L"char",     L"varchar", L"text",    L"date",    L"time",
    L"timestamp", L"boolean", L"blob",   L"count",   L"sum",     L"avg",     L"min",
    L"max",     L"coalesce", L"nullif",  nullptr};
const wchar_t *const literalKeywords[] = {L"true", L"false", L"null", nullptr};

// clang-format off
const Grammar grammars[] = {
    /* c */       {L"//", L"/*", L"*/", L"\"'", L'\\', false, false, false, false, false, false, true, false, false, false, false, false, false, false, cKeywords, cBuiltins},
    /* cpp */     {L"//", L"/*", L"*/", L"\"'", L'\\', false, false, false, false, false, false, true, false, false, false, false, false, false, false, cppKeywords, cppBuiltins},
    /* csharp */  {L"//", L"/*", L"*/", L"\"'", L'\\', false, false, false, false, false, false, false, true, false, false, false, false, false, false, csharpKeywords, nullptr},
    /* java */    {L"//", L"/*", L"*/", L"\"'", L'\\', false, false, false, false, false, false, true, false, false, false, false, false, false, false, javaKeywords, javaBuiltins},
    /* js */      {L"//", L"/*", L"*/", L"\"'`", L'\\', false, false, false, false, false, false, true, false, false, false, false, false, false, false, jsKeywords, jsBuiltins},
    /* ts */      {L"//", L"/*", L"*/", L"\"'`", L'\\', false, false, false, false, false, false, true, false, false, false, false, false, false, false, tsKeywords, jsBuiltins},
    /* python */  {L"#", L"", L"", L"\"'", L'\\', false, true, true, true, false, false, false, false, false, false, false, false, false, false, pythonKeywords, pythonBuiltins},
    /* rust */    {L"//", L"/*", L"*/", L"\"", L'\\', false, false, false, false, true, false, false, true, false, false, false, false, false, false, rustKeywords, rustBuiltins},
    /* go */      {L"//", L"/*", L"*/", L"\"`", L'\\', false, false, false, false, false, false, false, false, false, false, false, false, false, false, goKeywords, goBuiltins},
    /* json */    {L"//", L"/*", L"*/", L"\"", L'\\', false, false, false, false, false, false, false, false, false, false, true, false, false, false, literalKeywords, nullptr},
    /* yaml */    {L"#", L"", L"", L"\"'", L'\\', false, true, false, false, false, false, false, false, false, false, true, true, false, false, literalKeywords, nullptr},
    /* bash */    {L"#", L"", L"", L"\"'`", L'\\', false, true, false, false, false, true, false, false, false, false, false, false, false, false, bashKeywords, nullptr},
    /* pwsh */    {L"#", L"<#", L"#>", L"\"'", L'`', false, true, false, false, false, true, false, false, false, false, false, false, false, false, powershellKeywords, powershellBuiltins},
    /* sql */     {L"--", L"/*", L"*/", L"\"'", L'\\', true, false, false, false, false, false, false, false, false, false, false, false, false, false, sqlKeywords, sqlBuiltins},
    /* css */     {L"", L"/*", L"*/", L"\"'", L'\\', false, false, false, false, false, false, true, false, true, true, false, false, false, false, nullptr, nullptr},
    /* markup */  {L"", L"", L"", L"", 0, false, false, false, false, false, false, false, false, false, false, false, false, false, true, nullptr, nullptr},
    /* ini */     {L"#", L"", L"", L"\"'", 0, false, true, false, false, false, false, false, false, false, false, true, false, true, false, literalKeywords, nullptr},
};
// clang-format on
enum : size_t {
    kC, kCpp, kCsharp, kJava, kJs, kTs, kPython, kRust, kGo, kJson, kYaml, kBash, kPwsh, kSql, kCss,
    kMarkup, kIni
};
struct Alias {
    const wchar_t *name;
    size_t grammar;
};
const Alias aliases[] = {
    {L"c", kC},         {L"h", kC},        {L"cpp", kCpp},     {L"c++", kCpp},  {L"cc", kCpp},
    {L"cxx", kCpp},     {L"hpp", kCpp},    {L"hh", kCpp},      {L"csharp", kCsharp}, {L"c#", kCsharp},
    {L"cs", kCsharp},   {L"java", kJava},  {L"javascript", kJs}, {L"js", kJs},  {L"jsx", kJs},
    {L"mjs", kJs},      {L"cjs", kJs},     {L"typescript", kTs}, {L"ts", kTs},  {L"tsx", kTs},
    {L"mts", kTs},      {L"python", kPython}, {L"py", kPython}, {L"python3", kPython}, {L"rust", kRust},
    {L"rs", kRust},     {L"go", kGo},      {L"golang", kGo},   {L"json", kJson}, {L"jsonc", kJson},
    {L"yaml", kYaml},   {L"yml", kYaml},   {L"bash", kBash},   {L"sh", kBash},  {L"shell", kBash},
    {L"zsh", kBash},    {L"console", kBash}, {L"powershell", kPwsh}, {L"pwsh", kPwsh}, {L"ps1", kPwsh},
    {L"ps", kPwsh},     {L"sql", kSql},    {L"mysql", kSql},   {L"psql", kSql}, {L"plsql", kSql},
    {L"css", kCss},     {L"scss", kCss},   {L"less", kCss},    {L"html", kMarkup}, {L"xml", kMarkup},
    {L"svg", kMarkup},  {L"xhtml", kMarkup}, {L"xaml", kMarkup}, {L"ini", kIni}, {L"toml", kIni},
    {L"cfg", kIni},     {L"conf", kIni},   {L"properties", kIni}, {L"dotenv", kIni}, {L"env", kIni},
};

bool ident_start(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_';
}
bool ident_char(wchar_t c) {
    return ident_start(c) || (c >= L'0' && c <= L'9');
}
bool digit(wchar_t c) {
    return c >= L'0' && c <= L'9';
}
bool hex_digit(wchar_t c) {
    return digit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}
bool quote_of(const Grammar &g, wchar_t c) {
    return c && g.quotes && wcschr(g.quotes, c) != nullptr;
}
bool space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}
size_t word_end(std::wstring_view text, size_t i) {
    while (i < text.size() && ident_char(text[i]))
        ++i;
    return i;
}
bool starts_with(std::wstring_view text, size_t i, const wchar_t *prefix) {
    for (size_t p = 0; prefix[p]; ++p)
        if (i + p >= text.size() || text[i + p] != prefix[p])
            return false;
    return true;
}
bool matches_word(const wchar_t *const *words, std::wstring_view word, bool caseInsensitive) {
    if (!words)
        return false;
    wchar_t lower[32];
    if (word.size() >= sizeof(lower) / sizeof(lower[0]))
        return false;
    for (size_t i = 0; i < word.size(); ++i) {
        wchar_t c = word[i];
        if (caseInsensitive && c >= L'A' && c <= L'Z')
            c = c - L'A' + L'a';
        lower[i] = c;
    }
    for (size_t i = 0; words[i]; ++i) {
        size_t j = 0;
        while (j < word.size() && lower[j] == words[i][j])
            ++j;
        if (j == word.size() && words[i][j] == 0)
            return true;
    }
    return false;
}
void emit(std::vector<Highlight> &out, size_t start, size_t end, Token token) {
    if (end > start)
        out.push_back({(uint32_t)start, (uint32_t)(end - start), token});
}
// A quoted string starting at `i` (text[i] is the quote). Single and double quotes stop at
// their own quote or the end of the line; backticks span lines like block strings do.
size_t string_end(std::wstring_view text, size_t i, wchar_t quote, wchar_t escape, bool triple) {
    const size_t n = text.size();
    size_t j = i + (triple ? 3 : 1);
    while (j < n) {
        wchar_t c = text[j];
        if (escape && c == escape && j + 1 < n && !triple) {
            j += 2;
            continue;
        }
        if (c == quote) {
            if (!triple)
                return j + 1;
            if (j + 2 < n && text[j + 1] == quote && text[j + 2] == quote)
                return j + 3;
        }
        if (!triple && quote != L'`' && (c == L'\n' || c == L'\r'))
            return j;
        ++j;
    }
    return n;
}
size_t number_end(std::wstring_view text, size_t i, const Grammar &g) {
    const size_t n = text.size();
    size_t j = i;
    if (text[j] == L'0' && j + 1 < n && (text[j + 1] == L'x' || text[j + 1] == L'X')) {
        j += 2;
        while (j < n && (hex_digit(text[j]) || text[j] == L'_'))
            ++j;
    } else {
        while (j < n && (digit(text[j]) || text[j] == L'_'))
            ++j;
        if (j < n && text[j] == L'.' && j + 1 < n && digit(text[j + 1])) {
            ++j;
            while (j < n && (digit(text[j]) || text[j] == L'_'))
                ++j;
        }
        if (j < n && (text[j] == L'e' || text[j] == L'E')) {
            size_t k = j + 1;
            if (k < n && (text[k] == L'+' || text[k] == L'-'))
                ++k;
            if (k < n && digit(text[k]))
                j = k;
            while (j < n && digit(text[j]))
                ++j;
        }
    }
    if (g.numberUnits) {
        while (j < n && ((text[j] >= L'a' && text[j] <= L'z') || (text[j] >= L'A' && text[j] <= L'Z') ||
                         text[j] == L'%'))
            ++j;
    } else if (g.numberSuffix && j < n && wcschr(L"uUlLfFzZiIsS", text[j])) {
        // C-family and Rust numeric suffixes: 42u, 1.5f, 10LL, 0u32, 1usize.
        size_t k = j;
        while (k < n && k - j < 6 && ident_char(text[k]))
            ++k;
        j = k;
    }
    return j;
}
// A string may be a JSON/YAML key: the next non-space character after it is a colon.
bool string_is_key(std::wstring_view text, size_t end) {
    for (size_t j = end; j < text.size(); ++j) {
        wchar_t c = text[j];
        if (c == L' ' || c == L'\t' || c == L'\r')
            continue;
        return c == L':';
    }
    return false;
}
// The word's next non-space character opens a call: name( or macro!(.
bool calls_function(std::wstring_view text, size_t end) {
    size_t j = end;
    if (j < text.size() && text[j] == L'!')
        ++j;
    while (j < text.size() && (text[j] == L' ' || text[j] == L'\t'))
        ++j;
    return j < text.size() && text[j] == L'(';
}
void emit_string(std::vector<Highlight> &out, std::wstring_view text, size_t i, const Grammar &g,
                 size_t end) {
    bool key = g.stringKeys && string_is_key(text, end);
    emit(out, i, end, key ? Token::Key : Token::String);
}
void scan_markup(std::wstring_view text, std::vector<Highlight> &out) {
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        if (text[i] != L'<') {
            ++i;
            continue;
        }
        size_t scan = text.find(L'<', i + 1); // Plain text up to the next tag.
        size_t next = scan == std::wstring_view::npos ? n : scan;
        if (starts_with(text, i, L"<!--")) {
            size_t close = text.find(L"-->", i + 4);
            size_t end = close == std::wstring_view::npos ? n : close + 3;
            emit(out, i, end, Token::Comment);
            i = end;
        } else if (starts_with(text, i, L"<?")) {
            size_t close = text.find(L"?>", i + 2);
            size_t end = close == std::wstring_view::npos ? n : close + 2;
            emit(out, i, end, Token::Comment);
            i = end;
        } else if (i + 1 < n && (ident_start(text[i + 1]) ||
                                 (text[i + 1] == L'/' && i + 2 < n && ident_start(text[i + 2])))) {
            size_t j = i + 1;
            if (text[j] == L'/')
                ++j;
            size_t name = word_end(text, j);
            emit(out, j, name, Token::Keyword);
            while (name < n && text[name] != L'>') {
                wchar_t c = text[name];
                if (c == L'"' || c == L'\'') {
                    size_t end = string_end(text, name, c, L'\\', false);
                    emit(out, name, end, Token::String);
                    name = end;
                } else if (ident_start(c) || c == L'-') {
                    size_t end = name;
                    while (end < n && (ident_char(text[end]) || text[end] == L'-'))
                        ++end;
                    emit(out, name, end, Token::Builtin);
                    name = end;
                } else
                    ++name;
            }
            i = name < n ? name + 1 : n;
        } else if (i + 1 < n && text[i + 1] == L'!') {
            size_t close = text.find(L'>', i); // DOCTYPE and friends.
            size_t end = close == std::wstring_view::npos ? n : close + 1;
            emit(out, i, end, Token::Keyword);
            i = end;
        } else
            i = next;
    }
}
void scan(const Grammar &g, std::wstring_view text, std::vector<Highlight> &out) {
    const size_t n = text.size();
    size_t i = 0;
    bool lineStart = true; // Only whitespace since the last newline.
    while (i < n) {
        wchar_t c = text[i];
        if (g.lineComment[0] && starts_with(text, i, g.lineComment) &&
            (!g.wordComment || i == 0 || space(text[i - 1]))) {
            size_t end = text.find_first_of(L"\r\n", i);
            if (end == std::wstring_view::npos)
                end = n;
            emit(out, i, end, Token::Comment);
            i = end;
            continue;
        }
        if (g.blockOpen[0] && starts_with(text, i, g.blockOpen)) {
            size_t close = text.find(g.blockClose, i + wcslen(g.blockOpen));
            size_t end = close == std::wstring_view::npos ? n : close + wcslen(g.blockClose);
            emit(out, i, end, Token::Comment);
            i = end;
            lineStart = false;
            continue;
        }
        if (g.atWords && c == L'@' && i + 1 < n && ident_start(text[i + 1])) {
            size_t end = word_end(text, i + 1);
            emit(out, i, end, Token::Keyword);
            i = end;
            lineStart = false;
            continue;
        }
        if (g.sigilVars && c == L'$' && i + 1 < n && ident_start(text[i + 1])) {
            size_t end = word_end(text, i + 1);
            emit(out, i, end, Token::Builtin);
            i = end;
            lineStart = false;
            continue;
        }
        if (g.yamlKeys && lineStart) {
            size_t j = i;
            if (text[j] == L'-' && j + 1 < n && space(text[j + 1])) {
                ++j;
                while (j < n && (text[j] == L' ' || text[j] == L'\t'))
                    ++j;
            }
            if (j < n && (ident_start(text[j]) || digit(text[j]))) {
                size_t w = j;
                while (w < n && (ident_char(text[w]) || text[w] == L'.' || text[w] == L'-'))
                    ++w;
                if (w < n && text[w] == L':' && (w + 1 >= n || space(text[w + 1]))) {
                    emit(out, j, w, Token::Key);
                    i = w;
                    lineStart = false;
                    continue;
                }
            }
        }
        if (g.iniKeys && lineStart) {
            if (c == L'[') {
                size_t close = text.find(starts_with(text, i, L"[[") ? L"]]" : L"]", i + 1);
                if (close != std::wstring_view::npos) {
                    emit(out, i + 1, close, Token::Keyword);
                    i = close + (starts_with(text, i, L"[[") ? 2 : 1);
                    lineStart = false;
                    continue;
                }
            } else {
                size_t w = i;
                while (w < n && (ident_char(text[w]) || text[w] == L'.' || text[w] == L'-'))
                    ++w;
                size_t after = w;
                while (after < n && (text[after] == L' ' || text[after] == L'\t'))
                    ++after;
                if (w > i && after < n && text[after] == L'=') {
                    emit(out, i, w, Token::Key);
                    i = w;
                    lineStart = false;
                    continue;
                }
            }
        }
        if (quote_of(g, c)) {
            bool triple = g.tripleQuotes && i + 2 < n && text[i + 1] == c && text[i + 2] == c;
            size_t end = string_end(text, i, c, g.escape, triple);
            emit_string(out, text, i, g, end);
            i = end;
            lineStart = false;
            continue;
        }
        if (g.lifetimes && c == L'\'' && i + 1 < n && ident_start(text[i + 1])) {
            size_t w = word_end(text, i + 1);
            if (w >= n || text[w] != L'\'') { // 'a is a lifetime; 'a' scans as a string below.
                emit(out, i, w, Token::Builtin);
                i = w;
                lineStart = false;
                continue;
            }
        }
        if (g.hexColors && c == L'#' && i + 1 < n && hex_digit(text[i + 1])) {
            size_t j = i + 1;
            while (j < n && hex_digit(text[j]))
                ++j;
            emit(out, i, j, Token::Number);
            i = j;
            lineStart = false;
            continue;
        }
        if (digit(c) || (c == L'.' && i + 1 < n && digit(text[i + 1]))) {
            size_t end = number_end(text, i, g);
            emit(out, i, end, Token::Number);
            i = end;
            lineStart = false;
            continue;
        }
        if (ident_start(c)) {
            size_t w = word_end(text, i);
            if (g.stringPrefixes && w - i <= 2 && w < n && quote_of(g, text[w])) {
                bool prefixed = true;
                for (size_t p = i; p < w; ++p) {
                    wchar_t p2 = text[p];
                    if (!(p2 == L'r' || p2 == L'R' || p2 == L'f' || p2 == L'F' || p2 == L'b' ||
                          p2 == L'B' || p2 == L'u' || p2 == L'U'))
                        prefixed = false;
                }
                if (prefixed) {
                    wchar_t q = text[w];
                    bool triple =
                        g.tripleQuotes && w + 2 < n && text[w + 1] == q && text[w + 2] == q;
                    size_t end = string_end(text, w, q, g.escape, triple);
                    emit_string(out, text, i, g, end);
                    i = end;
                    lineStart = false;
                    continue;
                }
            }
            auto word = text.substr(i, w - i);
            if (matches_word(g.keywords, word, g.caseInsensitive))
                emit(out, i, w, Token::Keyword);
            else if (matches_word(g.builtins, word, g.caseInsensitive))
                emit(out, i, w, Token::Builtin);
            else if (calls_function(text, w))
                emit(out, i, w, Token::Function);
            i = w;
            lineStart = false;
            continue;
        }
        lineStart = c == L'\n' ? true : space(c) ? lineStart : false;
        ++i;
    }
}
const Grammar *grammar_for(std::wstring_view language) {
    if (language.empty() || language.size() > 24)
        return nullptr;
    wchar_t key[25];
    for (size_t i = 0; i < language.size(); ++i) {
        wchar_t c = language[i];
        key[i] = c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c;
    }
    key[language.size()] = 0;
    for (const auto &alias : aliases)
        if (wcscmp(key, alias.name) == 0)
            return &grammars[alias.grammar];
    return nullptr;
}
} // namespace

std::vector<Highlight> highlight(std::wstring_view language, std::wstring_view code) {
    std::vector<Highlight> result;
    const Grammar *g = grammar_for(language);
    if (!g || code.empty())
        return result;
    // The reader caches one text layout per line block; a pathological fence must not turn into
    // an unbounded highlight vector. Everything past the bound stays plain.
    constexpr size_t kMaxTokens = 200000;
    try {
        if (g->markup)
            scan_markup(code, result);
        else
            scan(*g, code, result);
    } catch (...) {
        result.clear();
    }
    if (result.size() > kMaxTokens)
        result.resize(kMaxTokens);
    return result;
}
} // namespace keepmd
