#pragma once

#include <windows.h>
#include <unknwn.h>

#include <string>
#include <vector>

// Posted to the window for each media engine event: wParam is the MF_MEDIA_ENGINE_EVENT, lParam its first
// parameter (the MF_MEDIA_ENGINE_ERR code for an error).
constexpr UINT WM_APP_MEDIA = WM_APP + 1;
// Posted when the video turns out to have a subtitle track of its own.
constexpr UINT WM_APP_SUBTITLE_TRACK = WM_APP + 2;
// Posted as each cue of the selected subtitle track starts showing: wParam is the track's id, lParam a Cue* for
// the window to delete.
constexpr UINT WM_APP_SUBTITLE_CUE = WM_APP + 3;
// Posted once at the next compositor frame after each RequestFrame.
constexpr UINT WM_APP_FRAME = WM_APP + 4;
// Posted when a seek bar preview picture DecodePreview asked for is ready for TakePreviewPicture.
constexpr UINT WM_APP_PREVIEW = WM_APP + 5;

// Creates a GPU device of its own, Media Foundation's media engine, which decodes on the GPU, and a thread that
// presents each new frame into a swap chain for DirectComposition, once per compositor frame. The engine's events
// are posted to hwnd.
bool InitPlayer(HWND hwnd);
void ShutdownPlayer();
// Has WM_APP_FRAME posted at the next compositor frame, so the window can update once per frame shown.
void RequestFrame();

// Starts loading and playing a local file; the outcome arrives as events. Fails if the file can't be opened.
HRESULT OpenVideo(const std::wstring& path);
bool HasVideo();

// An audio or subtitle track of the video.
struct Track {
    std::wstring language;  // a language tag such as "en", "en-US" or "eng"; may be empty
    std::wstring name;      // may be empty
    DWORD id;               // of a subtitle track
    bool selected;          // the audio track playing
};
// The video's audio tracks, once its metadata has loaded.
std::vector<Track> AudioTracks();
// Plays the audio track at index in AudioTracks() instead of the others.
void SelectAudioTrack(size_t index);
// The video's text subtitle tracks (SRT, SSA / ASS or WebVTT in MKV).
std::vector<Track> SubtitleTracks();
// Selects the subtitle track with the id instead of the others; its cues are then posted as they show.
bool SelectSubtitleTrack(DWORD id);
SIZE VideoSize();  // display size in pixels, or 0 x 0
// Sizes the video's swap chain; the first call creates it.
void SetVideoSize(int width, int height);
// The swap chain, as content for a DirectComposition visual, or null before SetVideoSize.
IUnknown* VideoSwapChain();

// The seek bar preview shows one picture for each 1/200 of the video (1 second at least): the key frame nearest
// the span's middle, which needs no decoding up to it. Returns that key frame for a time, in 100 ns units.
LONGLONG PreviewKeyFrame(double seconds, double duration);
// Has a thread of its own decode a key frame from PreviewKeyFrame, alone and shrunk to PreviewSize, then post
// WM_APP_PREVIEW. If it is still busy, only the latest asked is decoded next.
void DecodePreview(LONGLONG time);
// The picture WM_APP_PREVIEW announced and its key frame: a PreviewSize texture shared through this handle, whose
// keyed mutex hands it over with key 1, valid until the next call; null if none is waiting.
HANDLE TakePreviewPicture(LONGLONG& time);
// The preview pictures' size; 0 x 0 until the video has loaded, or if it has no previews.
SIZE PreviewSize();

bool IsPlaying();
void SetPlaying(bool playing);  // playing from the start if it has ended
double PlaybackTime();  // seconds
double Duration();      // seconds; NaN if unknown
// Starts seeking to a time within the video; SEEKED follows. A fast seek lands on the nearest key frame, for
// dragging along the seek bar and stepping.
void SeekTo(double seconds, bool fast);
double Volume();  // 0..1
bool IsMuted();
// Changes the volume by delta (0..1 scale), unmuting when raising it, and returns the new volume.
double ChangeVolume(double delta);
// Sets the volume (0..1), unmuting unless it is 0, and returns it.
double SetVolume(double volume);
bool ToggleMute();  // returns true if now muted
