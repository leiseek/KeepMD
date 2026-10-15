#include "remote_image.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <memory>
#include <windows.h>
#include <winhttp.h>

namespace keepmd {
std::optional<RemoteImageUrl> remote_image_url(std::wstring_view source) {
    if (source.empty() || source.size() > 8192 ||
        std::any_of(source.begin(), source.end(), [](wchar_t c) {
            return c <= L' ' || c == 127 || c == L'\\' || c == L'"' || c == L'<' || c == L'>';
        }))
        return {};
    std::wstring value = source.starts_with(L"//") ? L"https:" + std::wstring(source) : std::wstring(source);
    if (auto fragment = value.find(L'#'); fragment != value.npos)
        value.resize(fragment);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    parts.dwUserNameLength = parts.dwPasswordLength = (DWORD)-1;
    if (!WinHttpCrackUrl(value.c_str(), (DWORD)value.size(), 0, &parts) || !parts.dwHostNameLength ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        parts.dwUserNameLength || parts.dwPasswordLength || !parts.nPort)
        return {};
    auto authority = value.find(L"://");
    if (authority == value.npos)
        return {};
    auto end = value.find_first_of(L"/?", authority + 3);
    if (value.substr(authority + 3, end == value.npos ? end : end - authority - 3).find(L'@') != value.npos)
        return {};
    RemoteImageUrl result;
    result.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    result.resource = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength)
        result.resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    result.port = parts.nPort;
    result.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    std::transform(result.host.begin(), result.host.end(), result.host.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    auto host = result.host;
    if (host.find(L':') != host.npos && !host.starts_with(L"[")) host = L"[" + host + L"]";
    result.url = (result.secure ? L"https://" : L"http://") + host;
    if (result.port != (result.secure ? 443 : 80)) result.url += L":" + std::to_wstring(result.port);
    result.url += result.resource;
    return result;
}
namespace {
struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    explicit InternetHandle(HINTERNET h) : value(h) {}
    InternetHandle(const InternetHandle &) = delete;
    InternetHandle &operator=(const InternetHandle &) = delete;
};
struct AsyncState {
    std::atomic_uint refs{1};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<DWORD> completed{0}, error{0}, count{0};
    std::array<uint8_t, 16384> buffer{};
    ~AsyncState() { if (event) CloseHandle(event); }
    void release() { if (refs.fetch_sub(1) == 1) delete this; }
};
void CALLBACK completed(HINTERNET, DWORD_PTR context, DWORD status, void *data, DWORD length) {
    auto state = reinterpret_cast<AsyncState *>(context);
    if (!state)
        return;
    // The request owns a reference until its last callback. In particular the read buffer must outlive
    // cancellation: https://learn.microsoft.com/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        state->release();
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        auto info = static_cast<WINHTTP_ASYNC_RESULT *>(data);
        state->error.store(info ? info->dwError : ERROR_WINHTTP_INTERNAL_ERROR);
    } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
        state->count.store(length);
    } else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
               status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)
        return;
    state->completed.store(status);
    SetEvent(state->event);
}
std::wstring network_error(DWORD error) {
    if (error == ERROR_WINHTTP_TIMEOUT)
        return L"下载超时";
    if (error == ERROR_WINHTTP_SECURE_FAILURE)
        return L"无法验证站点的安全证书";
    if (error == ERROR_WINHTTP_REDIRECT_FAILED)
        return L"重定向过多或不安全";
    if (error == ERROR_WINHTTP_OPERATION_CANCELLED)
        return L"下载已取消";
    return L"网络请求失败（" + std::to_wstring(error) + L"）";
}
} // namespace
RemoteImageData fetch_remote_image(const RemoteImageUrl &url, const std::atomic_bool &cancel) {
    RemoteImageData result;
    auto fail = [&](std::wstring error) {
        result.bytes.clear();
        result.error = std::move(error);
        return std::move(result);
    };
    if (cancel.load())
        return fail(L"下载已取消");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    InternetHandle session(WinHttpOpen(L"KeepMD/0.4", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000))
        return fail(network_error(GetLastError()));
    InternetHandle connection(WinHttpConnect(session.value, url.host.c_str(), url.port, 0));
    if (!connection.value)
        return fail(network_error(GetLastError()));
    const wchar_t *accept[] = {L"image/*", L"application/octet-stream", nullptr};
    InternetHandle request(WinHttpOpenRequest(connection.value, L"GET", url.resource.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, accept, url.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.value)
        return fail(network_error(GetLastError()));
    DWORD disabled = WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP, redirects = 5;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects)))
        return fail(network_error(GetLastError()));

    auto release = [](AsyncState *state) { state->release(); };
    std::unique_ptr<AsyncState, decltype(release)> state(new AsyncState, release);
    if (!state->event)
        return fail(L"无法创建下载任务");
    if (WinHttpSetStatusCallback(request.value, completed, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                                WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        return fail(network_error(GetLastError()));
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state.get());
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        return fail(network_error(GetLastError()));
    state->refs.fetch_add(1); // Released by HANDLE_CLOSING even if the caller stops waiting.
    auto wait = [&](DWORD expected) {
        while (true) {
            if (cancel.load()) { result.error = L"下载已取消"; return false; }
            if (std::chrono::steady_clock::now() >= deadline) { result.error = L"下载超时"; return false; }
            if (auto error = state->error.load()) { result.error = network_error(error); return false; }
            if (state->completed.load() == expected)
                return true;
            if (WaitForSingleObject(state->event, 50) == WAIT_FAILED) {
                result.error = L"无法等待下载任务";
                return false;
            }
        }
    };
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, context))
        return fail(network_error(GetLastError()));
    if (!wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE))
        return fail(result.error);
    state->completed.store(0);
    if (!WinHttpReceiveResponse(request.value, nullptr))
        return fail(network_error(GetLastError()));
    if (!wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE))
        return fail(result.error);
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return fail(network_error(GetLastError()));
    if (status != 200)
        return fail(L"服务器返回 HTTP " + std::to_wstring(status));
    wchar_t length[32]{};
    size = sizeof(length);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            length, &size, WINHTTP_NO_HEADER_INDEX)) {
        uint64_t total = 0;
        for (wchar_t c : std::wstring_view(length)) {
            if (c < L'0' || c > L'9')
                return fail(L"图片响应长度无效");
            total = total * 10 + (c - L'0');
            if (total > RemoteImageLimit)
                return fail(L"图片下载超过 8 MiB 上限");
        }
    } else if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND)
        return fail(L"图片响应长度无效");
    for (;;) {
        state->completed.store(0);
        if (!WinHttpReadData(request.value, state->buffer.data(), (DWORD)state->buffer.size(), nullptr))
            return fail(network_error(GetLastError()));
        if (!wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE))
            return fail(result.error);
        auto count = state->count.load();
        if (!count)
            break;
        if (count > RemoteImageLimit - result.bytes.size())
            return fail(L"图片下载超过 8 MiB 上限");
        result.bytes.insert(result.bytes.end(), state->buffer.begin(), state->buffer.begin() + count);
    }
    if (result.bytes.empty())
        return fail(L"服务器返回了空图片");
    return result;
}
} // namespace keepmd
