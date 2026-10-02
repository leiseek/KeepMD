#include "diagram.h"
#include "document.h"
#include "file_io.h"
#include "settings.h"
#include "shell_integration.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace keepmd;
int failures = 0, checks = 0;
void check(bool value, const char *name) {
    ++checks;
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}
int main() {
    auto doc = parse_document("# 标题\n\nHello **bold** *italic* ~~gone~~ `code` &amp; &#x1F600;.\n\n- "
                              "first\n- [x] done\n\n[reference][later]\n\n[later]: https://example.com\n\n| "
                              "A | B |\n|:--|--:|\n|one|two|\n\n```cpp\nint x;\nreturn x;\n```\n");
    check(doc->error.empty(), "markdown parse succeeds");
    check(doc->headings.size() == 1 && doc->headings[0].text == L"标题", "CJK heading");
    check(doc->plain_text().find(L"Hello bold italic gone code & 😀.") != std::wstring::npos,
          "inline styles and entities preserve text");
    bool bold = false, italic = false, strike = false, mono = false, ref = false, task = false, table = false;
    int code = 0;
    for (const auto &b : doc->blocks) {
        for (const auto &s : b.text.spans) {
            bold |= (s.style & Bold) != 0;
            italic |= (s.style & Italic) != 0;
            strike |= (s.style & Strike) != 0;
            mono |= (s.style & Mono) != 0;
            ref |= s.link == L"https://example.com";
        }
        task |= b.marker == L"☑";
        table |= b.kind == Kind::TableRow && b.cells.size() == 2;
        if (b.kind == Kind::Code)
            ++code;
    }
    check(bold && italic && strike && mono, "style spans");
    check(ref, "forward link references");
    check(task, "task list");
    check(table, "GFM tables");
    check(code == 2, "code lines remain individually layoutable");
    auto image = parse_document("before ![测试](image%20name.png) after\n");
    check(image->blocks.size() == 3, "inline image keeps surrounding text");
    check(image->plain_text().find(L"before") != std::wstring::npos &&
              image->plain_text().find(L"after") != std::wstring::npos,
          "inline image no text loss");
    auto headings = parse_document("# Repeat\n\n# Repeat\n");
    check(headings->headings[1].anchor == L"repeat-1", "duplicate heading anchor");
    check(search(*doc, L"BOLD").size() == 1, "case insensitive search");
    check(search(*doc, L"标题").size() == 1, "CJK search");
    check(doc->text_range(0, 2) == L"标题", "selection from semantic text");
    check(decode_entities("&lt;&gt;&quot;&unknown;&#0;") == L"<>\"&unknown;�", "entities fallback");
    check(wide(utf8(L"中文 😀 café")) == L"中文 😀 café", "Unicode round trip");
    Heights heights;
    heights.reset({10, 20, 30, 40});
    check(heights.total() == 100 && heights.locate(30) == 2, "height prefix lookup");
    heights.set(1, 35);
    check(heights.total() == 115 && heights.prefix(2) == 45 && heights.locate(44) == 1, "height updates");
    auto flow = parse_diagram("flowchart TD\nA[开始] -->|是| B{判断}\nB -.-> A\n");
    check(flow.ok && flow.graph.nodes.size() == 2 && flow.graph.edges.size() == 2,
          "native Chinese flowchart");
    if (flow.ok) {
        auto layout = mermaid::layout(flow.graph, {{100, 50}, {120, 60}}, 40, 70);
        check(layout.nodes.size() == 2 && std::isfinite(layout.height), "cyclic diagram terminates");
    }
    check(!parse_diagram("sequenceDiagram\nA->>B: hi").ok, "unsupported diagram reports failure");
    check(!parse_diagram("flowchart TD\nA@{ shape: rounded } --> B").ok,
          "unsupported syntax does not silently render");
    std::string huge = "graph LR\n";
    for (int i = 0; i < 205; ++i)
        huge += "N" + std::to_string(i) + "-->N" + std::to_string(i + 1) + "\n";
    check(!parse_diagram(huge).ok, "diagram node budget");
    auto temp = std::filesystem::temp_directory_path() / L"keepmd-core-tests";
    std::filesystem::create_directories(temp);
    std::wstring error;
    for (auto encoding : {Encoding::Utf8, Encoding::Utf8Bom, Encoding::Utf16LE, Encoding::Utf16BE}) {
        auto path = temp / (std::to_wstring((int)encoding) + L" 中文.md");
        check(save_file(path, L"# 中文\r\n😀\r\n", encoding, error), "atomic Unicode save");
        FileData data;
        check(load_file(path, data, error) && data.encoding == encoding && data.crlf &&
                  data.utf8 == "# 中文\r\n😀\r\n",
              "encoding preservation");
        check(save_file(path, L"changed\n", encoding, error), "replace existing file");
        std::filesystem::remove(path);
    }
    auto bad = temp / L"invalid.md";
    {
        std::ofstream out(bad, std::ios::binary);
        out << "\xffinvalid";
    }
    FileData data;
    check(!load_file(bad, data, error), "invalid UTF8 explicit failure");
    std::filesystem::remove(bad);
    auto locked = temp / L"locked.md";
    save_file(locked, L"original 中文", Encoding::Utf8, error);
    HANDLE lock =
        CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "failure test holds destination against replacement");
    check(!save_file(locked, L"replacement", Encoding::Utf8, error),
          "locked destination reports save failure");
    if (lock != INVALID_HANDLE_VALUE)
        CloseHandle(lock);
    check(load_file(locked, data, error) && data.utf8 == "original 中文",
          "failed save leaves original bytes readable");
    std::filesystem::remove(locked);
    std::atomic_bool cancel = true;
    check(!parse_document("# Cancel\n", &cancel)->error.empty(), "parse cancellation");
    std::string longText;
    for (int i = 0; i < 10000; ++i)
        longText += "word 中文 ";
    longText += "END";
    auto longDoc = parse_document(longText);
    check(longDoc->blocks.size() > 1, "long paragraph has bounded physical chunks");
    check(longDoc->plain_text() == wide(longText) + L"\n", "chunking preserves exact semantic text");
    check(search(*longDoc, L"word 中文").size() == 10000, "chunk boundaries preserve search matches");
    auto longCode = std::string(100000, 'x');
    auto codeDoc = parse_document("```\n" + longCode + "\n```\n");
    check(codeDoc->blocks.size() > 1 && codeDoc->plain_text() == wide(longCode) + L"\n",
          "oversized code line chunks preserve source copying");
    auto bigDiagram =
        parse_document("```mermaid\ngraph LR\n%%" + std::string(100000, 'x') + "\nA-->B\n```\n");
    check(std::none_of(bigDiagram->blocks.begin(), bigDiagram->blocks.end(),
                       [](const Block &block) { return block.kind == Kind::Diagram; }) &&
              bigDiagram->blocks.size() > 5,
          "oversized diagram fallback uses bounded code layouts");
    for (const std::wstring cluster : {L"é", L"👩‍💻", L"🇨🇳", L"👍🏽", L"1️⃣"})
        for (size_t inside = 1; inside < cluster.size(); ++inside) {
            auto text = std::wstring(8192 - inside, L'x') + cluster + std::wstring(9000, L'z');
            auto chunks = parse_document(utf8(text));
            check(chunks->plain_text() == text + L"\n" && chunks->blocks[0].text.value.back() == L'x',
                  "combining/emoji cluster preserved at fragment boundary");
        }
    for (const char *direction : {"TD", "TB", "BT", "LR", "RL"})
        for (const char *node :
             {"A[开始]", "A(开始)", "A{判断}", "A((圆形))", "A([结束])", "A[\"中文 label\"]"}) {
            auto f =
                parse_diagram(std::string("flowchart ") + direction + "\n" + node + " -->|标签| B[完成]\n");
            check(f.ok && f.graph.nodes.size() == 2 && f.graph.edges.size() == 1,
                  "flow direction/shape semantic matrix");
        }
    check(!parse_diagram("flowchart TD\nsubgraph A\nB-->C\n").ok, "unclosed subgraph fails explicitly");
    check(!parse_diagram("flowchart TD\nA-->B\nstyle A font-size:20px\n").ok,
          "unknown style reports unsupported");
    check(!parse_diagram("flowchart TD\nA-->B\nlinkStyle 0 stroke:red\n").ok,
          "unsupported edge style reports unsupported");
    auto grouped = parse_diagram(
        "graph LR\nsubgraph left [左组]\nA-->B\nend\nsubgraph right [右组]\nC-->D\nend\nB-->C\n");
    check(grouped.ok && grouped.graph.subgraphs.size() == 2 && grouped.graph.edges.size() == 3,
          "cross-group node connections");
    auto self = parse_diagram("graph TD\nA[自己]-->A\n");
    check(self.ok && self.graph.edges.size() == 1, "self loop retained");
    auto styled = parse_diagram("graph LR\nA[Start]:::chosen --> B[End]\nclassDef chosen "
                                "fill:#abc,color:#123456\nstyle A color:red\n");
    check(styled.ok && styled.graph.nodes[0].style.hasFill &&
              styled.graph.nodes[0].style.fill.rgb == 0xaabbcc &&
              styled.graph.nodes[0].style.text.rgb == 0xff0000,
          "late class definition resolves and direct style takes precedence");
    auto defaultStyle = parse_diagram("graph LR\nA-->B\nclassDef default fill:#eeeeee\n");
    check(defaultStyle.ok && defaultStyle.graph.nodes[1].style.fill.rgb == 0xeeeeee,
          "default graph class applies to unclassified nodes");
    ReaderSettings prefs;
    prefs.dark = true;
    prefs.zoom = 1.5f;
    prefs.reading_width = 700;
    for (int i = 0; i < 12; ++i)
        prefs.remember(temp / (L"文档😀" + std::to_wstring(i) + L".md"), i);
    check(prefs.recent.size() == 10, "recent history remains bounded");
    auto config = temp / L"settings.ini";
    check(store_settings(config, prefs), "preferences atomically saved");
    auto restored = load_settings(config);
    check(restored.dark && restored.zoom == 1.5f && restored.reading_width == 700 &&
              restored.recent[0].path == prefs.recent[0].path && restored.recent[0].block == 11,
          "Unicode recent path and position survive restart");
    std::filesystem::remove(config);
    auto prefix = L"Software\\KeepMD.IntegrationTests\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64());
    auto plan = registration_plan(L"C:\\Program Files\\KeepMD\\keepmd.exe", prefix);
    check(apply_registration(plan, error), "Open With registration writes only isolated test keys");
    wchar_t value[512]{};
    DWORD bytes = sizeof(value);
    auto key = prefix + L"\\Classes\\KeepMD.Markdown\\shell\\open\\command";
    check(RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes) ==
                  ERROR_SUCCESS &&
              std::wstring(value) == L"\"C:\\Program Files\\KeepMD\\keepmd.exe\" \"%1\"",
          "registered executable and argument are quoted");
    bytes = sizeof(value);
    check(RegGetValueW(HKEY_CURRENT_USER, (prefix + L"\\Classes\\.md").c_str(), nullptr, RRF_RT_ANY, nullptr,
                       value, &bytes) != ERROR_SUCCESS,
          "registration does not replace extension default");
    check(prefix.starts_with(L"Software\\KeepMD.IntegrationTests\\") &&
              RegDeleteTreeW(HKEY_CURRENT_USER, prefix.c_str()) == ERROR_SUCCESS,
          "isolated registry test values removed");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
