#include "prompt_window.h"
#include "ui.h"
#include <commctrl.h>
#include <ole2.h>
#include <shellapi.h>

namespace keepprompt {
namespace {
constexpr wchar_t HostClass[] = L"KeepPrompt.Host";
struct Host {
    HWND hwnd = nullptr;
    std::unique_ptr<PromptWindow> prompt;
};
Host *host_of(HWND hwnd) {
    return reinterpret_cast<Host *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}
std::filesystem::path beside_executable(const wchar_t *name) {
    wchar_t buffer[32768]{};
    GetModuleFileNameW(nullptr, buffer, (DWORD)std::size(buffer));
    return std::filesystem::path(buffer).parent_path() / name;
}
LRESULT CALLBACK host_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto *host = host_of(hwnd);
    if (message == WM_NCCREATE) {
        host = static_cast<Host *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        host->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (!host)
        return DefWindowProcW(hwnd, message, w, l);
    if (message == WM_PROMPT_HOST) {
        if (w == PromptExit && host->prompt->flush())
            PostQuitMessage(0);
        return 0;
    }
    if (message == WM_QUERYENDSESSION)
        return host->prompt && host->prompt->flush();
    if (message == WM_CLOSE) {
        if (!host->prompt || host->prompt->flush())
            DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_DESTROY) {
        host->prompt.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}
} // namespace
} // namespace keepprompt

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace keepprompt;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    ui::set_brand(L"KeepPrompt", ui::Accent::Violet);
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance;
    cls.lpfnWndProc = host_proc;
    cls.lpszClassName = HostClass;
    RegisterClassExW(&cls);
    Host host;
    bool show = true, resident = false, dark = false, given = false;
    // A portable profile beside the executable; the draft is kept next to it as KeepPrompt.draft.md.
    auto config = beside_executable(L"KeepPrompt.ini");
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--config" && i + 1 < argc) {
            config = std::filesystem::absolute(argv[++i]);
            given = true;
        } else if (arg == L"--resident") {
            resident = true;
            show = false;
        } else if (arg == L"--show" || arg == L"--prompt")
            show = true;
        else if (arg == L"--dark")
            dark = true;
    }
    LocalFree(argv);
    host.hwnd =
        CreateWindowExW(0, HostClass, L"KeepPrompt", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, &host);
    if (!host.hwnd) {
        OleUninitialize();
        return 1;
    }
    host.prompt = std::make_unique<PromptWindow>(host.hwnd, prompt_profile(config, given), dark);
    if (!host.prompt->start(resident, show)) {
        host.prompt.reset();
        DestroyWindow(host.hwnd);
        OleUninitialize();
        return 0;
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (ui::caption_translate(message))
            continue;
        if (host.prompt && host.prompt->translate(message))
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    host.prompt.reset();
    if (host.hwnd)
        DestroyWindow(host.hwnd);
    OleUninitialize();
    return 0;
}
