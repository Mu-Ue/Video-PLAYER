#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "media/player.h"

// Posted with a batch of a Matroska subtitle track's cues: wParam is the track's id, lParam a std::vector<Cue>*
// for the window to delete. Each cue arrives once.
constexpr UINT WM_APP_SUBTITLE_CUES = WM_APP + 6;

// Lists the text subtitle tracks of a Matroska or WebM file and starts reading all their cues on a thread of its
// own, which posts them to hwnd. Returns no tracks, and reads nothing, if it isn't one or a track can't be read.
std::vector<Track> StartMkvSubtitles(const std::wstring& path, HWND hwnd);
// Has the reading go on from this playback position, so its cues arrive first.
void SeekMkvSubtitles(double seconds);
// Stops the reading, if any; no batch is posted after it returns.
void StopMkvSubtitles();
