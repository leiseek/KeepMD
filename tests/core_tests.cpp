#include "diagram.h"
#include "document.h"
#include "file_io.h"
#include "prompt_model.h"
#include "settings.h"
#include "shell_integration.h"
#include "visual_markdown.h"
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
    PromptHotkey hotkey;
    check(parse_prompt_hotkey(L" ctrl + ALT + p ", hotkey, error) && hotkey.key == 'P' &&
              hotkey.modifiers == (MOD_CONTROL | MOD_ALT) && hotkey.label == L"Ctrl+Alt+P",
          "global shortcut normalizes a combination");
    for (const auto *invalid :
         {L"P", L"Shift+A", L"Ctrl+F12", L"Ctrl+P+Q", L"Ctrl++P", L"Ctrl+P+", L"Ctrl+Ctrl+P"})
        check(!parse_prompt_hotkey(invalid, hotkey, error), "unsafe or malformed shortcut rejected");
    check(parse_prompt_hotkey(L"LCtrl x2", hotkey, error) && hotkey.doubleCtrl,
          "legacy double Ctrl supported");
    CtrlTap tap;
    check(!tap.event(true, true, 1000) && !tap.event(true, false, 1020) && !tap.event(true, true, 1100) &&
              tap.event(true, false, 1120),
          "two completed clean Ctrl taps trigger");
    tap = {};
    tap.event(true, true, 1000);
    tap.event(false, true, 1010);
    tap.event(true, false, 1020);
    tap.event(true, true, 1050);
    check(!tap.event(true, false, 1080), "Ctrl+C followed by Ctrl cannot summon prompt");
    tap = {};
    tap.event(true, true, 1000);
    tap.event(true, true, 1010);
    tap.event(true, false, 1800);
    tap.event(true, true, 1820);
    check(!tap.event(true, false, 1840), "held Ctrl and repeats cannot summon prompt");
    tap = {};
    tap.event(true, true, 1000, true);
    tap.event(true, false, 1020, true);
    tap.event(true, true, 1100, true);
    check(!tap.event(true, false, 1120, true), "another held key prevents double Ctrl");
    tap.event(true, true, 1200);
    tap.event(true, false, 1220);
    tap.event(true, true, 1300);
    check(tap.event(true, false, 1320), "double Ctrl recovers after another held modifier is released");
    PromptSettings ps;
    ps.hotkey = L"Ctrl+Shift+F9";
    ps.resident = true;
    ps.opacity = 70;
    ps.preview = false;
    auto promptConfig = temp / L"prompts.ini";
    check(save_prompt_settings(promptConfig, ps, error), "prompt preferences saved atomically");
    auto ps2 = load_prompt_settings(promptConfig);
    check(ps2.hotkey == ps.hotkey && ps2.resident && ps2.opacity == 70 && !ps2.preview,
          "prompt preferences survive restart independently from reader");
    std::filesystem::remove(promptConfig);
    std::wstring draft = L"untouched";
    check(
        import_prompt_flow(
            R"({"Prompt":"# 标题\n\n  缩进\n\ud83d\ude00\n","Hotkey":"LCtrl x2","AlwaysOnTop":false,"Opacity":80,"Autostart":true})",
            ps2, draft, error) &&
            draft == L"# 标题\n\n  缩进\n😀\n" && ps2.hotkey == L"LCtrl x2" && !ps2.top && ps2.opacity == 80,
        "Prompt Flow import preserves Markdown whitespace, Chinese and escaped emoji");
    const auto baseline = draft;
    for (const char *invalid :
         {R"({"Prompt":"unterminated})", R"({"Prompt":"\ud800"})", R"({"Prompt":"\u0000"})",
          R"({"Prompt":"a","Prompt":"b"})", R"({"Prompt":"a",})", R"({"Opacity":70})",
          R"({"Prompt":"a","Opacity":500})", R"({"Prompt":{}})", R"({"Prompt":"a"}trailing)"})
        check(!import_prompt_flow(invalid, ps2, draft, error) && draft == baseline,
              "bad legacy input leaves draft unchanged");
    check(
        !import_prompt_flow("{\"Prompt\":\"" + std::string(PromptLimit + 1, 'a') + "\"}", ps2, draft, error),
        "oversized prompt import rejected");
    auto startup = prompt_startup_command(L"C:\\Apps with spaces\\KeepMD.exe", L"C:\\用户\\profile.ini");
    check(startup == L"\"C:\\Apps with spaces\\KeepMD.exe\" --resident --config \"C:\\用户\\profile.ini\"",
          "startup command quotes executable and profile path");
    auto startupKey = L"Software\\KeepMD.IntegrationTests\\Startup-" + std::to_wstring(GetCurrentProcessId());
    check(set_prompt_startup(L"test", startup, true, error, startupKey) &&
              prompt_startup_enabled(L"test", startup, startupKey),
          "startup registration uses isolated test key");
    check(set_prompt_startup(L"test", startup, false, error, startupKey) &&
              !prompt_startup_enabled(L"test", startup, startupKey),
          "startup registration can be removed");
    RegDeleteTreeW(HKEY_CURRENT_USER, startupKey.c_str());
    auto visual = visual_plan(L"# 标题 😀\n\n正文 **粗体** 和 *斜体* 与 `代码`。\n- 项目\n");
    check(visual.source == L"# 标题 😀\n\n正文 **粗体** 和 *斜体* 与 `代码`。\n- 项目\n",
          "visual mapping preserves original Markdown whitespace and emoji");
    check(visual.paragraphs[0].heading == 1 && (visual.styles[0] & VHidden) && !(visual.styles[2] & VHidden),
          "visual heading hides its syntax but keeps title editable");
    auto visualBold = visual.source.find(L"粗体"), visualItalic = visual.source.find(L"斜体"),
         visualCode = visual.source.find(L"代码");
    check((visual.styles[visualBold] & VBold) && (visual.styles[visualBold - 1] & VHidden) &&
              (visual.styles[visualItalic] & VItalic) && (visual.styles[visualCode] & VCode),
          "visual inline semantic formatting comes from Markdown parser");
    auto nested = visual_plan(L"***nested*** [link](https://example.test) \\*literal\\*\n");
    check((nested.styles[3] & (VBold | VItalic)) == (VBold | VItalic) &&
              (nested.styles[nested.source.find(L"link")] & VLink),
          "nested emphasis and link display mapped without rewriting targets");
    check(!(nested.styles[nested.source.find(L"*literal")] & VHidden),
          "escaped visible literal punctuation remains visible");
    auto flowVisual = visual_plan(L"前文\n\n```mermaid\nflowchart LR\nA-->B\n```\n\n后文\n");
    check(flowVisual.diagrams.size() == 1 && flowVisual.diagrams[0].source == L"flowchart LR\nA-->B\n" &&
              flowVisual.styles[flowVisual.source.find(L"A-->")] & VHidden,
          "visual diagram retains exact source behind native figure");
    auto incomplete = visual_plan(L"```mermaid\nflowchart LR\nA-->B\n");
    check(incomplete.diagrams.empty() && !(incomplete.styles[incomplete.source.find(L"A-->")] & VHidden),
          "incomplete diagram remains editable code until completed");
    std::wstring many;
    for (int i = 0; i < 20; ++i)
        many += L"```mermaid\ngraph TD\nA-->B\n```\n";
    check(visual_plan(many).diagrams.size() == 16, "visual inline picture count is bounded");
    check(visual_plan(L"```mermaid  \ngraph LR\nA-->B\n```\n").diagrams.size() == 1,
          "visual Mermaid language tolerates trailing whitespace");
    check(visual_plan(L"```mermaid\ngraph LR\nA-->B\n```not a closing fence\n").diagrams.empty(),
          "visual fence closing line must contain no info string");
    check(visual_plan(L"a\r\nb\rc\n").source == L"a\nb\nc\n",
          "visual import normalizes only paragraph line endings");
    check(rtf_escape(L"{\\}中文😀").find("\\u") != std::string::npos && rtf_escape(L"{\\}") == "\\{\\\\\\}",
          "RTF output escapes markup and Unicode scalars");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
