#pragma once

#include <windows.h>

constexpr wchar_t kAppId[] = L"VideoPLAYER";
constexpr wchar_t kAppName[] = L"VideoPLAYER";
constexpr wchar_t kFileFilter[] =
    L"Videos and subtitles\0*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.3gp;*.ts;*.m2ts;*.srt\0"
    L"Videos\0*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.3gp;*.ts;*.m2ts\0"
    L"Subtitles\0*.srt\0";
constexpr wchar_t kSubtitleFilter[] = L"Subtitles\0*.srt\0";
// The types --register offers the player as the default app for; keep in step with kFileFilter.
constexpr const wchar_t* kVideoExtensions[] = {L".mp4", L".m4v", L".mov", L".mkv", L".avi",
                                               L".wmv", L".webm", L".3gp", L".ts", L".m2ts"};

constexpr double kSeekStep = 5.0;     // seconds
constexpr double kLongSeekStep = 60.0;  // seconds, with Ctrl
constexpr double kVolumeStep = 0.05;  // of full volume
constexpr double kMaxVolume = 2.0;   // of full volume, boosted past it
constexpr double kMinRate = 0.5, kMaxRate = 2.0, kRateStep = 0.25;  // playback speeds the speed slider stops at
constexpr UINT kOsdMs = 1500;          // how long seek / volume feedback stays up
constexpr UINT kDoubleClickMs = 300;  // max gap between the clicks of a fullscreen double click
constexpr UINT kIdleMs = 2500;        // idle time before the controls and cursor hide while playing
constexpr LONGLONG kMaxSubtitleBytes = 16 << 20;
constexpr size_t kMaxCueChars = 2048;
// Of a read while walking an MKV's clusters for subtitles: on slow drives, larger reads make a seek wait on more
// bytes than it needs, smaller ones on more reads.
constexpr size_t kScanBytes = 512 << 10;
constexpr size_t kMaxCues = 100000;  // per subtitle file; a film has a few thousand
constexpr int kMaxSidecarSubtitles = 32;  // .srt files picked up next to a video
constexpr size_t kMaxPositions = 50;      // videos whose position is kept for resuming
