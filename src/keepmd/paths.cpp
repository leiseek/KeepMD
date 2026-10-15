#include "paths.h"

namespace keepmd {
std::filesystem::path linked_path(const std::filesystem::path &base, const std::wstring &target) {
    bool rooted = !target.empty() && (target[0] == L'/' || target[0] == L'\\') &&
                  (target.size() < 2 || (target[1] != L'/' && target[1] != L'\\'));
    if (!rooted)
        return base / target; // Network paths keep their own root.
    auto root = base;
    std::error_code ec;
    for (auto folder = base; !folder.empty(); folder = folder.parent_path()) {
        if (std::filesystem::exists(folder / L".git", ec)) {
            root = folder;
            break;
        }
        if (folder == folder.parent_path())
            break;
    }
    return root / std::filesystem::path(target).relative_path();
}
} // namespace keepmd
