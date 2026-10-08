#include "prompt_window.h"
#include "ui.h"
#include <commctrl.h>
#include <ole2.h>
#include <shellapi.h>
#include <vector>

namespace keepmd {
namespace {
constexpr wchar_t HostClass[] = L"KeepPrompt.Host";
struct Host {
    HWND hwnd = nullptr;
    std::filesystem::path config;
    std::unique_ptr<PromptWindow> prompt;
};
Host *host_of(HWND hwnd) {
    return reinterpret_cast<Host *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}
std::filesystem::path sibling(const wchar_t *name) {
    wchar_t buffer[32768]{};
    GetModuleFileNameW(nullptr, buffer, (DWORD)std::size(buffer));
    return std::filesystem::path(buffer).parent_path() / name;
}
std::wstring quote(const std::wstring &value) {
    std::wstring result = L"\"";
    for (wchar_t c : value) {
        if (c == L'\"')
            result += L'\\';
        result += c;
    }
    result += L"\"";
    return result;
}
void open_reader(Host &host) {
    if (auto reader = FindWindowW(L"KeepMD.Window", nullptr)) {
        ShowWindow(reader, SW_SHOWNORMAL);
        SetForegroundWindow(reader);
        return;
    }
    auto executable = sibling(L"KeepMD.exe");
    std::error_code ec;
    if (!std::filesystem::exists(executable, ec))
        return;
    auto commandLine = quote(executable.wstring()) + L" --config " + quote(host.config.wstring());
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                       CREATE_UNICODE_ENVIRONMENT, nullptr, executable.parent_path().c_str(), &startup,
                       &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
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
    if (message == WM_PROMPT_OWNER) {
        if (w == 1)
            open_reader(*host);
        else if (w == 2) {
            if (host->prompt->flush())
                PostQuitMessage(0);
        } else if (w == 3)
            host->prompt->theme(l != 0);
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
} // namespace keepmd

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace keepmd;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = instance;
    cls.lpfnWndProc = host_proc;
    cls.lpszClassName = HostClass;
    RegisterClassExW(&cls);
    Host host;
    bool show = true, resident = false, dark = false;
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    host.config = sibling(L"keepmd.ini");
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--config" && i + 1 < argc)
            host.config = std::filesystem::absolute(argv[++i]);
        else if (arg == L"--resident") {
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
    host.prompt = std::make_unique<PromptWindow>(host.hwnd, host.config, dark);
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
