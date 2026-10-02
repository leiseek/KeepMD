#pragma once
#include "mermaid.h"
#include <string>
#include <string_view>
namespace keepmd {
struct DiagramResult {
    mermaid::Diagram graph;
    std::wstring error;
    bool ok = false;
};
DiagramResult parse_diagram(std::string_view source);
} // namespace keepmd
