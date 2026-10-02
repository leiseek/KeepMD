#pragma once
#include <filesystem>
#include <memory>
#include <windows.h>
namespace keepmd {
constexpr UINT WM_PROMPT_OWNER = WM_APP + 30; // wParam: 1 = show reader, 2 = explicit exit
class PromptWindow {
  public:
    PromptWindow(HWND owner, const std::filesystem::path &readerConfig, bool dark);
    ~PromptWindow();
    // Returns false when another process owns this profile (and receives the request).
    bool start(bool forceResident, bool show);
    void show();
    bool resident() const;
    bool flush();
    void theme(bool dark);
    bool translate(MSG &message);

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace keepmd
