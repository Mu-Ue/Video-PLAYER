#pragma once

// What the player keeps between runs, for the current user. --unregister clears it.
struct Settings {
    double volume = 1;  // 0..1
    bool muted = false;
};

Settings LoadSettings();
void SaveSettings(const Settings& settings);
