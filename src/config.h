#pragma once

#include <windows.h>

constexpr wchar_t kAppId[] = L"VideoVIEWER";
constexpr wchar_t kAppName[] = L"Video VIEWER";
constexpr wchar_t kFileFilter[] =
    L"Videos and subtitles\0*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.3gp;*.ts;*.m2ts;*.srt\0"
    L"Videos\0*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.3gp;*.ts;*.m2ts\0"
    L"Subtitles\0*.srt\0";

constexpr double kSeekStep = 5.0;     // seconds
constexpr double kVolumeStep = 0.05;  // of full volume
constexpr UINT kOsdMs = 1500;          // how long seek / volume feedback stays up
constexpr UINT kCursorHideMs = 2000;   // idle time before the cursor hides in fullscreen
constexpr LONGLONG kMaxSubtitleBytes = 16 << 20;
constexpr size_t kMaxCueChars = 2048;
