#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>
namespace keepmd {
struct RegistryValue {
    std::wstring key, name, value;
    DWORD type = REG_SZ;
};
std::vector<RegistryValue> registration_plan(const std::filesystem::path &executable,
                                             const std::wstring &softwarePrefix = L"Software");
bool apply_registration(const std::vector<RegistryValue> &plan, std::wstring &error);
} // namespace keepmd
