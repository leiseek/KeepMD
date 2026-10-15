#pragma once
#include "keepmd.h"
#include <filesystem>
#include <string>

namespace keepmd {
// Resolves a decoded link or image path against the document folder. Like GitHub, a leading "/" names the
// repository root: the nearest folder at or above `base` holding .git, else `base` itself.
std::filesystem::path linked_path(const std::filesystem::path &base, const std::wstring &target);
} // namespace keepmd
