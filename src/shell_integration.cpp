#include "shell_integration.h"
namespace keepmd {
std::vector<RegistryValue> registration_plan(const std::filesystem::path &executable,
                                             const std::wstring &prefix) {
    const auto classes = prefix + L"\\Classes", capabilities = prefix + L"\\KeepMD\\Capabilities";
    const auto command = L"\"" + executable.wstring() + L"\" \"%1\"",
               icon = L"\"" + executable.wstring() + L"\",0";
    std::vector<RegistryValue> values = {
        {classes + L"\\KeepMD.Markdown", L"", L"Markdown document"},
        {classes + L"\\KeepMD.Markdown\\DefaultIcon", L"", icon},
        {classes + L"\\KeepMD.Markdown\\shell\\open\\command", L"", command},
        {classes + L"\\Applications\\keepmd.exe", L"FriendlyAppName", L"KeepMD"},
        {classes + L"\\Applications\\keepmd.exe\\shell\\open\\command", L"", command},
        {capabilities, L"ApplicationName", L"KeepMD"},
        {capabilities, L"ApplicationDescription", L"Native Markdown reader and source editor"},
        {prefix + L"\\RegisteredApplications", L"KeepMD", capabilities}};
    for (const auto *extension : {L".md", L".markdown", L".mmd"}) {
        values.push_back(
            {classes + L"\\" + extension + L"\\OpenWithProgids", L"KeepMD.Markdown", L"", REG_NONE});
        values.push_back({classes + L"\\Applications\\keepmd.exe\\SupportedTypes", extension, L""});
        values.push_back({capabilities + L"\\FileAssociations", extension, L"KeepMD.Markdown"});
    }
    return values;
}
bool apply_registration(const std::vector<RegistryValue> &plan, std::wstring &error) {
    for (const auto &value : plan) {
        HKEY key = nullptr;
        LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, value.key.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                         nullptr, &key, nullptr);
        if (result == ERROR_SUCCESS) {
            result = RegSetValueExW(
                key, value.name.empty() ? nullptr : value.name.c_str(), 0, value.type,
                reinterpret_cast<const BYTE *>(value.value.c_str()),
                value.type == REG_NONE ? 0 : (DWORD)((value.value.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
        if (result != ERROR_SUCCESS) {
            error = L"无法添加打开方式，Windows 错误码：" + std::to_wstring(result);
            return false;
        }
    }
    return true;
}
} // namespace keepmd
