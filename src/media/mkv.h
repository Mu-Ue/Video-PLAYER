#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "media/player.h"

// Posted with a batch of a Matroska subtitle track's cues: wParam is the track's id, lParam a std::vector<Cue>*
// for the window to delete. Each cue arrives once.
constexpr UINT WM_APP_SUBTITLE_CUES = WM_APP + 6;
// Posted once the cues around a seek are posted: wParam is the seek's number.
constexpr UINT WM_APP_SUBTITLES_READY = WM_APP + 7;

// Lists the text subtitle tracks of a Matroska or WebM file and starts reading all their cues on a thread of its
// own, which posts them to hwnd. Returns no tracks, and reads nothing, if it isn't one or a track can't be read.
std::vector<Track> StartMkvSubtitles(const std::wstring& path, HWND hwnd);
// Has the reading go on from this playback position, so its cues arrive first. Returns whether
// WM_APP_SUBTITLES_READY will follow with this number once they have, which it does until all cues are read.
bool SeekMkvSubtitles(double seconds, UINT number);
// Stops the reading, if any; no batch is posted after it returns.
void StopMkvSubtitles();

// The CodecDelay of each audio track of a Matroska or WebM file, in track order, in 100 ns units; empty if it isn't
// one. Media Foundation plays that much encoder delay (such as AAC's priming) at the start of each track.
std::vector<LONGLONG> MkvAudioDelays(const wchar_t* path);
// The times of a Matroska or WebM file's video key frames that its index lists, sorted, in 100 ns units; empty if it
// isn't one or has no index.
std::vector<LONGLONG> MkvKeyFrames(const wchar_t* path);
