#include <windows.h>
#include <imm.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "platform/file_dialog.h"
#include "ui/window.h"

namespace {

// Loads DLLs only from System32 and never from network shares or low-integrity locations,
// so a DLL planted next to the exe or in the current folder is never picked up.
void HardenProcess() {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy{};
    policy.NoRemoteImages = 1;
    policy.NoLowMandatoryLabelImages = 1;
    policy.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &policy, sizeof(policy));
}

// Splits the command line into quoted or space-separated arguments. An unquoted line that names an existing
// file is one path, even with spaces in it.
std::vector<std::wstring> ParseArgs(const wchar_t* s) {
    std::wstring line(s ? s : L"");
    const auto first = line.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    line = line.substr(first, line.find_last_not_of(L" \t") - first + 1);
    if (line.front() != L'"' && GetFileAttributesW(line.c_str()) != INVALID_FILE_ATTRIBUTES) return {line};

    std::vector<std::wstring> args;
    for (size_t i = 0; i < line.size();) {
        if (line[i] == L' ' || line[i] == L'\t') {
            ++i;
        } else if (line[i] == L'"') {
            const size_t end = line.find(L'"', i + 1);
            args.push_back(line.substr(i + 1, end == std::wstring::npos ? end : end - i - 1));
            i = end == std::wstring::npos ? line.size() : end + 1;
        } else {
            const size_t end = line.find_first_of(L" \t", i);
            args.push_back(line.substr(i, end == std::wstring::npos ? end : end - i));
            i = end == std::wstring::npos ? line.size() : end;
        }
    }
    return args;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int showCmd) {
    HardenProcess();
    if (wcsncmp(cmdLine, L"--dialog ", 9) == 0) return DialogMain();

    // The player has no text input, so never load the IME / text services stack (the file dialog runs
    // in its own process and keeps it).
    ImmDisableIME(DWORD(-1));
    // Media Foundation objects are free-threaded; a multithreaded apartment calls them directly.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;

    if (!StartPlayer(instance, ParseArgs(cmdLine), showCmd)) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
