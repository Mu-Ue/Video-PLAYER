#include "platform/settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "config.h"

namespace {

const std::wstring kKey = std::wstring(L"Software\\") + kAppId;

bool GetDword(const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kKey.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) ==
           ERROR_SUCCESS;
}

void SetDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey.c_str(), name, REG_DWORD, &value, sizeof(value));
}

}  // namespace

Settings LoadSettings() {
    Settings s;
    DWORD value;
    if (GetDword(L"Volume", value)) s.volume = std::min<DWORD>(value, 100) / 100.0;
    if (GetDword(L"Muted", value)) s.muted = value != 0;
    return s;
}

void SaveSettings(const Settings& s) {
    SetDword(L"Volume", DWORD(std::lround(std::clamp(s.volume, 0.0, 1.0) * 100)));
    SetDword(L"Muted", s.muted);
}
