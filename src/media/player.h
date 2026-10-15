#pragma once

#include <windows.h>
#include <unknwn.h>

#include <memory>
#include <string>
#include <vector>

struct Subtitles;

// Posted to the window for each media engine event: wParam is the MF_MEDIA_ENGINE_EVENT, lParam its first
// parameter (the MF_MEDIA_ENGINE_ERR code for an error).
constexpr UINT WM_APP_MEDIA = WM_APP + 1;
// Posted when the video turns out to have a subtitle track of its own.
constexpr UINT WM_APP_SUBTITLE_TRACK = WM_APP + 2;
// Posted as each cue of the selected subtitle track starts showing: wParam is the track's id, lParam a Cue* for
// the window to delete.
constexpr UINT WM_APP_SUBTITLE_CUE = WM_APP + 3;
// Posted once shortly before the next compositor frame after each RequestFrame.
constexpr UINT WM_APP_FRAME = WM_APP + 4;
// Posted when a seek bar preview picture DecodePreview asked for is ready for TakePreviewPicture.
constexpr UINT WM_APP_PREVIEW = WM_APP + 5;
// Posted once the video's audio tracks are known, after it opens.
constexpr UINT WM_APP_AUDIO_TRACKS = WM_APP + 8;

// Starts what opening the first video, if not empty, needs that can be done before InitPlayer, on threads of its
// own. Call once, first.
void PrepareVideo(const std::wstring& path);
// Creates a GPU device of its own, Media Foundation's media engine, which decodes on the GPU, and a thread that
// presents each new frame into a swap chain for DirectComposition, once per compositor frame. The engine's events
// are posted to hwnd.
bool InitPlayer(HWND hwnd);
void ShutdownPlayer();
// Has WM_APP_FRAME posted shortly before the next compositor frame, so the window can update once per frame
// shown, as late as it can.
void RequestFrame();

// Starts loading and playing a local file; the outcome arrives as events. Fails if the file can't be opened.
HRESULT OpenVideo(const std::wstring& path);
// Has a second parser of the video opened on a thread of its own, for where its key frames are, which fast seeks
// and the seek bar previews need, and for the previews; called once its first frame is ready, so that never holds
// the frame up.
void OpenSeekHelpers();
bool HasVideo();

// An audio or subtitle track of the video.
struct Track {
    std::wstring language;  // a language tag such as "en", "en-US" or "eng"; may be empty
    std::wstring name;      // may be empty
    DWORD id;               // of a subtitle track
    bool selected;          // the audio track playing
};
// The video's audio tracks, once WM_APP_AUDIO_TRACKS tells they are known.
std::vector<Track> AudioTracks();
// Plays the audio track at index in AudioTracks() instead of the others, from the source's next start: a seek.
void SelectAudioTrack(size_t index);
// The video's text subtitle tracks (SRT, SSA / ASS or WebVTT in MKV).
std::vector<Track> SubtitleTracks();
// Selects the subtitle track with the id instead of the others; its cues are then posted as they show.
bool SelectSubtitleTrack(DWORD id);
SIZE VideoSize();  // display size in pixels, or 0 x 0
// Sizes the video's swap chain; the first call creates it.
void SetVideoSize(int width, int height);
// Has frames drawn at the video's size on screen, at most the swap chain's, and stretched to the swap chain's,
// so the visual's scaling back down to that size shows each pixel as drawn.
void SetVideoTarget(int width, int height);
// The swap chain, as content for a DirectComposition visual, or null before SetVideoSize.
IUnknown* VideoSwapChain();

// Where the subtitles go, in pixels of the video on screen, which is width wide.
struct SubtitleLayout {
    float width, fontSize, maxWidth;
    float top;     // of the top cues
    float bottom;  // of the bottom cues
    bool operator==(const SubtitleLayout&) const = default;
};
// Has the cues of subs (null for none) drawn into each frame as it presents: the cues showing at the frame's own
// time, as if burned into it, so they change with the frame on screen and never apart from it.
void SetSubtitles(std::shared_ptr<const Subtitles> subs);
void SetSubtitleLayout(const SubtitleLayout& layout);

// The seek bar preview shows one picture for each 1/200 of the video (1 second at least): the key frame nearest
// the span's middle, which needs no decoding up to it. Returns that key frame for a time, in 100 ns units.
LONGLONG PreviewKeyFrame(double seconds, double duration);
// The same for the span spans away from the one at the time; -1 past either end of the video.
LONGLONG PreviewKeyFrameNear(double seconds, double duration, int spans);
// Has a thread of its own decode a key frame from PreviewKeyFrame, alone and shrunk to PreviewSize, then post
// WM_APP_PREVIEW. If it is still busy, only the latest asked is decoded next.
void DecodePreview(LONGLONG time);
// The picture WM_APP_PREVIEW announced and its key frame: a PreviewSize texture shared through this handle, whose
// keyed mutex hands it over with key 1, valid until the next call; null if none is waiting.
HANDLE TakePreviewPicture(LONGLONG& time);
// The preview pictures' size; 0 x 0 until the video has loaded, or if it has no previews.
SIZE PreviewSize();

bool IsPlaying();
bool HasEnded();
void SetPlaying(bool playing);  // playing from the start if it has ended
double PlaybackTime();  // seconds
double Duration();      // seconds; NaN if unknown
// Where a seek to a time within the video lands, as far as known. A fast seek lands on the nearest key frame, for
// dragging along the seek bar and stepping.
double SeekLanding(double seconds, bool fast);
// Starts seeking to a landing from SeekLanding; SEEKED follows.
void SeekTo(double landing, bool fast);
// The time of the frame on screen, in seconds, and the shortest time between the video's frames yet (a guess
// until two have shown), for stepping a frame by exact seeks.
double FrameTime();
double FrameGap();
void SetPlaybackRate(double rate);  // 1 for normal speed
double Volume();  // 0..kMaxVolume, 1 for full volume
bool IsMuted();
// Changes the volume by delta (0..1 scale), unmuting when raising it, and returns the new volume.
double ChangeVolume(double delta);
// Sets the volume (0..kMaxVolume), unmuting unless it is 0, and returns it.
double SetVolume(double volume);
bool ToggleMute();  // returns true if now muted
