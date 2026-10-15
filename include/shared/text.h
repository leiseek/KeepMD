#pragma once
#include <string>
#include <string_view>

// Product-neutral foundation shared by KeepMD and KeepPrompt. It never refers to either application.
namespace keep {
std::wstring wide(std::string_view text);
std::string utf8(std::wstring_view text);
} // namespace keep
