#pragma once

#include <string>

// What the player keeps between runs, for the current user. --unregister clears it.
struct Settings {
    double volume = 1;  // 0..kMaxVolume
    bool muted = false;
};

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// Where a video was left, in seconds; 0 if it wasn't, or was watched to the end.
double LoadPosition(const std::wstring& path);
// Keeps where a video was left for the next time it opens, among the last kMaxPositions; 0 forgets it.
void SavePosition(const std::wstring& path, double seconds);
