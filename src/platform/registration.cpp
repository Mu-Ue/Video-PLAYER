#include "platform/registration.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

#include "config.h"

namespace {

const std::wstring kClasses = L"Software\\Classes\\";
const std::wstring kProgId = std::wstring(kAppId) + L".Video";
const std::wstring kAppKey = std::wstring(L"Software\\") + kAppId;
const std::wstring kCapabilities = kAppKey + L"\\Capabilities";
constexpr wchar_t kRegisteredApps[] = L"Software\\RegisteredApplications";

bool SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

}  // namespace

bool RegisterApp() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (len == 0 || len == ARRAYSIZE(exe)) return false;
    const std::wstring quoted = L"\"" + std::wstring(exe) + L"\"";
    const std::wstring icon = quoted + L",0";

    bool ok = SetString(kClasses + kProgId, nullptr, L"Video file") &&
              SetString(kClasses + kProgId + L"\\DefaultIcon", nullptr, icon) &&
              SetString(kClasses + kProgId + L"\\shell\\open\\command", nullptr, quoted + L" \"%1\"") &&
              SetString(kCapabilities, L"ApplicationName", kAppName) &&
              SetString(kCapabilities, L"ApplicationDescription", L"A minimal, fast video player") &&
              SetString(kCapabilities, L"ApplicationIcon", icon);
    for (const wchar_t* ext : kVideoExtensions) {
        ok = ok && SetString(kCapabilities + L"\\FileAssociations", ext, kProgId) &&
             RegSetKeyValueW(HKEY_CURRENT_USER, (kClasses + ext + L"\\OpenWithProgids").c_str(), kProgId.c_str(),
                             REG_NONE, nullptr, 0) == ERROR_SUCCESS;
    }
    // Listed last, so Windows only sees the app once everything it points to is in place.
    ok = ok && SetString(kRegisteredApps, kAppId, kCapabilities);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (!ok) return false;

    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        const std::wstring page = std::wstring(L"ms-settings:defaultapps?registeredAppUser=") + kAppId;
        ShellExecuteW(nullptr, nullptr, page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }
    return true;
}

void UnregisterApp() {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegisteredApps, kAppId);
    for (const wchar_t* ext : kVideoExtensions) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, (kClasses + ext + L"\\OpenWithProgids").c_str(), kProgId.c_str());
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + kProgId).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
