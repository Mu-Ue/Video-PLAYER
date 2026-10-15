#include "platform/settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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

// A video's position, by a hash of its full path, so the registry holds no file names.
struct Position {
    UINT64 hash;
    double seconds;
};

UINT64 PathHash(const std::wstring& path) {
    std::wstring full(MAX_PATH, L'\0');
    DWORD len = GetFullPathNameW(path.c_str(), DWORD(full.size()), full.data(), nullptr);
    if (len > full.size()) {
        full.resize(len);
        len = GetFullPathNameW(path.c_str(), len, full.data(), nullptr);
    }
    full.resize(len && len < full.size() ? len : 0);
    if (full.empty()) full = path;
    CharLowerBuffW(full.data(), DWORD(full.size()));
    UINT64 hash = 14695981039346656037ull;  // FNV-1a
    for (const wchar_t c : full) {
        hash = (hash ^ (c & 0xFF)) * 1099511628211ull;
        hash = (hash ^ (c >> 8)) * 1099511628211ull;
    }
    return hash;
}

// Most recent first.
std::vector<Position> ReadPositions() {
    std::vector<Position> positions(kMaxPositions);
    DWORD size = DWORD(positions.size() * sizeof(Position));
    if (RegGetValueW(HKEY_CURRENT_USER, kKey.c_str(), L"Positions", RRF_RT_REG_BINARY, nullptr, positions.data(),
                     &size) != ERROR_SUCCESS) {
        size = 0;
    }
    positions.resize(size / sizeof(Position));
    return positions;
}

}  // namespace

Settings LoadSettings() {
    Settings s;
    DWORD value;
    if (GetDword(L"Volume", value)) s.volume = std::min<DWORD>(value, DWORD(kMaxVolume * 100)) / 100.0;
    if (GetDword(L"Muted", value)) s.muted = value != 0;
    return s;
}

void SaveSettings(const Settings& s) {
    SetDword(L"Volume", DWORD(std::lround(std::clamp(s.volume, 0.0, kMaxVolume) * 100)));
    SetDword(L"Muted", s.muted);
}

double LoadPosition(const std::wstring& path) {
    const UINT64 hash = PathHash(path);
    for (const Position& p : ReadPositions()) {
        if (p.hash == hash) return std::isfinite(p.seconds) ? std::max(p.seconds, 0.0) : 0;
    }
    return 0;
}

void SavePosition(const std::wstring& path, double seconds) {
    const UINT64 hash = PathHash(path);
    std::vector<Position> positions = ReadPositions();
    std::erase_if(positions, [&](const Position& p) { return p.hash == hash; });
    if (seconds > 0) positions.insert(positions.begin(), {hash, seconds});
    positions.resize(std::min(positions.size(), kMaxPositions));
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey.c_str(), L"Positions", REG_BINARY, positions.data(),
                    DWORD(positions.size() * sizeof(Position)));
}
