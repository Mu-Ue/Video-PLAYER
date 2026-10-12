<p align="center"><img src="docs/icon.png" width="128" alt="Video-PLAYER icon"></p>

# Video-PLAYER

A minimal, fast native video player for Windows (Win32 + Media Foundation + DirectComposition) with hardware decoding and subtitles (`.srt` files or built into the video).

## Build

Requires Visual Studio (or Build Tools) with the C++ workload. Run `build.bat`, or from a Developer Command Prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build\Video-PLAYER.exe` (single file, no dependencies).

## Use

```
"Video-PLAYER.exe" [video.mp4] [subtitles.srt]
```

Run `Video-PLAYER.exe --register` once to list the player under Settings → Default apps (run again after moving the exe; `--unregister` removes it).

Plays the formats Windows supports (MP4, MOV, MKV, AVI, WMV, …). Drop a video or an `.srt` file on the window, use File → Open, or add subtitles to the playing video with Subtitles → Add subtitle file. Text subtitles built into an MKV, and `.srt` files next to the video named like it (`Movie.en.srt`), are detected and start off; press `S` to show them, or pick an audio or subtitle track with the subtitles button. Moving the mouse shows the controls over the bottom of the video: a seek bar that previews the frame under the mouse, play / pause, seek back / forward, volume, subtitles and fullscreen.

| Input                  | Action                  |
| ---------------------- | ----------------------- |
| `Space` / click        | Play / pause            |
| `←` / `→`              | Back / forward 5 s      |
| `↑` / `↓` / scroll     | Volume up / down        |
| `M`                    | Mute                    |
| `S`                    | Subtitles on / off      |
| `F11` / double-click   | Fullscreen              |
| `Ctrl+O`               | Open video or subtitles |
| `Alt+F`                | File menu               |
| `Alt+S`                | Subtitles menu          |
| `Esc`                  | Exit fullscreen / close |
