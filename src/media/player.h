#pragma once

#include <windows.h>

#include <string>

// Posted to the window for each media engine event: wParam is the MF_MEDIA_ENGINE_EVENT, lParam its first
// parameter (the MF_MEDIA_ENGINE_ERR code for an error).
constexpr UINT WM_APP_MEDIA = WM_APP + 1;

// Creates a GPU device of its own and Media Foundation's media engine, which decodes on the GPU and presents
// frames itself, at the right time, into a swap chain for DirectComposition. Its events are posted to hwnd.
bool InitPlayer(HWND hwnd);
void ShutdownPlayer();

// Starts loading and playing a local file; the outcome arrives as events. Fails if the file can't be opened.
HRESULT OpenVideo(const std::wstring& path);
bool HasVideo();
SIZE VideoSize();  // display size in pixels, or 0 x 0
// Sizes the video's swap chain; the first call creates it.
void SetVideoSize(int width, int height);
// A new handle to the swap chain, for IDCompositionDevice::CreateSurfaceFromHandle, or null before SetVideoSize.
// The caller closes it.
HANDLE VideoSurface();

bool IsPlaying();
void SetPlaying(bool playing);  // playing from the start if it has ended
double PlaybackTime();  // seconds
double Duration();      // seconds; NaN if unknown
// Starts seeking to a time within the video; SEEKED follows. A fast seek lands on the nearest key frame, for
// dragging along the seek bar.
void SeekTo(double seconds, bool fast);
double Volume();  // 0..1
bool IsMuted();
// Changes the volume by delta (0..1 scale), unmuting when raising it, and returns the new volume.
double ChangeVolume(double delta);
bool ToggleMute();  // returns true if now muted
