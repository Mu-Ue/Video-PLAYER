#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Creates the main window and plays the first video in files, with the first .srt in files as its subtitles.
// The window shows once the video's size is known. Returns false if the window or the media stack can't start.
bool StartPlayer(HINSTANCE instance, const std::vector<std::wstring>& files, int showCmd);
