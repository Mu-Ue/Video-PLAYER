<p align="center"><img src="docs/icon.png" width="128" alt="Video VIEWER icon"></p>

# Video VIEWER

A minimal, fast native video player for Windows (Win32 + Media Foundation + DirectComposition) with hardware decoding and `.srt` subtitles.

## Build

Requires Visual Studio (or Build Tools) with the C++ workload. Run `build.bat`, or from a Developer Command Prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build\Video VIEWER.exe` (single file, no dependencies).

## Use

```
"Video VIEWER.exe" [video.mp4] [subtitles.srt]
```

Plays the formats Windows supports (MP4, MOV, MKV, AVI, WMV, …). Drop a video or an `.srt` file on the window to open it.

| Input                  | Action                  |
| ---------------------- | ----------------------- |
| `Space` / click        | Play / pause            |
| `←` / `→`              | Back / forward 5 s      |
| `↑` / `↓`              | Volume up / down        |
| `M`                    | Mute                    |
| `S`                    | Subtitles on / off      |
| `F11` / double-click   | Fullscreen              |
| `Ctrl+O`               | Open video or subtitles |
| `Esc`                  | Exit fullscreen / close |
