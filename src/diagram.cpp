#include "diagram.h"
#include "document.h"
#include <algorithm>
#include <cctype>
#include <vector>
namespace keepmd {
namespace {
std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace((unsigned char)value.front()))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace((unsigned char)value.back()))
        value.remove_suffix(1);
    return value;
}
std::vector<std::string_view> statements(std::string_view source) {
    std::vector<std::string_view> result;
    size_t start = 0;
    int nesting = 0;
    char quote = 0;
    bool escape = false;
    for (size_t i = 0; i < source.size(); ++i) {
        char c = source[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (quote) {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '[' || c == '(' || c == '{')
            ++nesting;
        else if (c == ']' || c == ')' || c == '}')
            nesting = std::max(0, nesting - 1);
        if (c == '\n' || (c == ';' && nesting == 0)) {
            result.push_back(trim(source.substr(start, i - start)));
            start = i + 1;
        }
    }
    result.push_back(trim(source.substr(start)));
    return result;
}
} // namespace
DiagramResult parse_diagram(std::string_view source) {
    DiagramResult result;
    if (source.size() > 64 * 1024) {
        result.error = L"流程图源码超过 64 KiB，显示源码。";
        return result;
    }
    int groups = 0;
    for (auto line : statements(source)) {
        if (line.starts_with("%%{")) {
            result.error = L"暂不支持 Mermaid 配置指令，显示源码。";
            return result;
        }
        if (line.starts_with("%%"))
            continue;
        if (line.starts_with("subgraph ")) {
            if (++groups > 1) {
                result.error = L"当前支持单层 subgraph，嵌套分组显示源码。";
                return result;
            }
        }
        if (line == "end" && --groups < 0) {
            result.error = L"流程图存在多余的 end。";
            return result;
        }
        if (line.starts_with("click ") || line.starts_with("direction ") || line.starts_with("linkStyle ")) {
            result.error = L"暂不支持该流程图指令：" + wide(line.substr(0, line.find(' ')));
            return result;
        }
        if (line.starts_with("style ") || line.starts_with("classDef ")) {
            auto pos = line.find(' ');
            line = trim(line.substr(pos + 1));
            pos = line.find(' ');
            if (pos == std::string_view::npos)
                continue;
            line = trim(line.substr(pos + 1));
            while (!line.empty()) {
                auto comma = line.find(',');
                auto attribute = trim(line.substr(0, comma));
                auto colon = attribute.find(':');
                auto key = trim(attribute.substr(0, colon));
                if (key != "fill" && key != "stroke" && key != "color" && key != "stroke-width") {
                    result.error = L"暂不支持流程图样式属性：" + wide(key);
                    return result;
                }
                if (comma == std::string_view::npos)
                    break;
                line = trim(line.substr(comma + 1));
            }
        }
    }
    if (groups) {
        result.error = L"subgraph 缺少 end，显示源码。";
        return result;
    }
    try {
        auto parsed = mermaid::parse(source);
        if (!parsed.success) {
            result.error = L"流程图第 " + std::to_wstring(parsed.errorLine) + L" 行：" + wide(parsed.error);
            return result;
        }
        if (parsed.diagram.nodes.size() > 200 || parsed.diagram.edges.size() > 400) {
            result.error = L"流程图超过 200 节点或 400 边，显示源码。";
            return result;
        }
        for (const auto &group : parsed.diagram.subgraphs)
            for (const auto &node : parsed.diagram.nodes)
                if (node.id == group.id) {
                    result.error = L"当前支持分组内节点间连接；直接连接分组容器暂显示源码。";
                    return result;
                }
        // The extracted parser retains class definitions separately. Resolve
        // them here, after all declarations, with direct node styles winning.
        auto merge = [](mermaid::Style &target, const mermaid::Style &source) {
            if (source.hasFill) {
                target.hasFill = true;
                target.fill = source.fill;
            }
            if (source.hasStroke) {
                target.hasStroke = true;
                target.stroke = source.stroke;
            }
            if (source.hasText) {
                target.hasText = true;
                target.text = source.text;
            }
            if (source.hasStrokeWidth) {
                target.hasStrokeWidth = true;
                target.strokeWidth = source.strokeWidth;
            }
        };
        for (auto &node : parsed.diagram.nodes) {
            mermaid::Style resolved;
            if (auto base = parsed.diagram.classStyles.find("default");
                base != parsed.diagram.classStyles.end())
                merge(resolved, base->second);
            if (!node.className.empty()) {
                auto style = parsed.diagram.classStyles.find(node.className);
                if (style == parsed.diagram.classStyles.end()) {
                    result.error = L"未定义的流程图样式类：" + wide(node.className);
                    return result;
                }
                merge(resolved, style->second);
            }
            merge(resolved, node.style);
            node.style = resolved;
        }
        result.graph = std::move(parsed.diagram);
        result.ok = true;
    } catch (...) {
        result.error = L"流程图解析失败，显示源码。";
    }
    return result;
}
} // namespace keepmd
