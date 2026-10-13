#pragma once

#include <windows.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "media/player.h"

// Posted with a batch of a Matroska subtitle track's cues: wParam is the track's id, lParam a std::vector<Cue>*
// for the window to delete.
constexpr UINT WM_APP_SUBTITLE_CUES = WM_APP + 6;

// Lists the text subtitle tracks of a Matroska or WebM file, and readies reading their cues from what playback
// reads of it, as it reads it; they are posted to hwnd. Returns no tracks if it isn't one or a track can't be read.
std::vector<Track> StartMkvSubtitles(const std::wstring& path, HWND hwnd);
// Wraps the stream playback reads the file from, if StartMkvSubtitles found tracks in it.
void WrapMkvStream(Microsoft::WRL::ComPtr<IMFByteStream>& stream);
// Stops the reading, if any; no batch is posted after it returns.
void StopMkvSubtitles();
