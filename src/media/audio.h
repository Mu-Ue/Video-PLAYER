#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct Track;

// Plays a video's sound apart from the media engine, which plays just the picture: the engine restarts its sound,
// and holds the picture, on each change of playback speed. The sound follows the engine's clock, kept to the
// picture's pitch at any speed.

// Where the picture is, in seconds, at the performance counter time qpc (in 100 ns units), and whether it is
// playing: not paused, seeking, waiting or ended.
using AudioClock = bool (*)(double& seconds, LONGLONG& qpc);

// Starts the threads that decode and play the sound; tracks is posted to hwnd once a file's audio tracks are known.
// Call once.
void StartAudio(AudioClock clock, HWND hwnd, UINT tracks);
void StopAudio();
// Wakes the player to look at the clock again, after the engine's state changes.
void WakeAudio();
// Plays the sound of a local file from the clock, or none for an empty path.
void OpenAudio(const std::wstring& path);
// The file's audio tracks, once known; one is selected.
std::vector<Track> AudioTrackList();
void SelectAudio(size_t index);
void SetAudioRate(double rate);
void SetAudioVolume(double volume, bool muted);
