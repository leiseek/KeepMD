#pragma once
#include "keepprompt.h"
#include "prompt_model.h"
#include <memory>
#include <windows.h>
namespace keepprompt {
// Posted by the prompt window to its message-only host window.
constexpr UINT WM_PROMPT_HOST = WM_APP + 30;
constexpr WPARAM PromptExit = 2; // Explicit Quit: flush the draft, then end the process.
class PromptWindow {
  public:
    PromptWindow(HWND host, const PromptProfile &profile, bool forceDark);
    ~PromptWindow();
    // Returns false when another process owns this profile (and receives the request).
    bool start(bool forceResident, bool show);
    void show();
    bool resident() const;
    bool flush();
    bool translate(MSG &message);

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace keepprompt
