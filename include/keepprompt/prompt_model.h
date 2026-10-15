#pragma once
#include "keepprompt.h"
#include <filesystem>
#include <string>
#include "visual_editor.h"
#include <windows.h>

namespace keepprompt {
constexpr size_t PromptLimit = keep::VisualLimit; // UTF-16 units held by the visual editor.
struct PromptHotkey {
    bool doubleCtrl = false;
    UINT modifiers = 0, key = 0;
    std::wstring label;
};
bool parse_prompt_hotkey(std::wstring text, PromptHotkey &out, std::wstring &error);
struct CtrlTap {
    bool down = false, clean = false;
    uint64_t pressed = 0, released = 0;
    bool event(bool leftCtrl, bool keyDown, uint64_t now, bool otherHeld = false);
};
struct PromptSettings {
    std::wstring hotkey = L"Ctrl+Alt+Space";
    bool resident = false, top = true, preview = true, toolbar = true, dark = false;
    int opacity = 100;
};
PromptSettings load_prompt_settings(const std::filesystem::path &path);
bool save_prompt_settings(const std::filesystem::path &path, const PromptSettings &settings,
                          std::wstring &error);
bool import_prompt_flow(std::string_view json, PromptSettings &settings, std::wstring &draft,
                        std::wstring &error);
// The files of one KeepPrompt profile. Before the split, KeepMD 0.4.x kept prompts beside a KeepMD reader profile
// as <name>.prompt.ini and <name>.prompt.md. Such a profile is used in place so its draft, settings and startup
// entry survive the upgrade; the reader profile itself is never written.
struct PromptProfile {
    std::filesystem::path settings, draft;
    std::filesystem::path readerProfile; // The KeepMD profile a pre-split profile belongs to; empty otherwise.
};
// `config` is the --config argument (`given`), or KeepPrompt.ini beside the executable.
PromptProfile prompt_profile(const std::filesystem::path &config, bool given);
std::wstring prompt_profile_id(const std::filesystem::path &path);
std::wstring prompt_startup_command(const std::filesystem::path &exe, const std::filesystem::path &config);
bool prompt_startup_enabled(const std::wstring &value, const std::wstring &command,
                            const std::wstring &key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run");
bool set_prompt_startup(const std::wstring &value, const std::wstring &command, bool enable,
                        std::wstring &error,
                        const std::wstring &key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run");
} // namespace keepprompt
