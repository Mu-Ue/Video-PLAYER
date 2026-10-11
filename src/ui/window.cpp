#include "ui/window.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <mfmediaengine.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "media/player.h"
#include "media/subtitles.h"
#include "platform/file_dialog.h"
#include "ui/overlay.h"

using Microsoft::WRL::ComPtr;

namespace {

enum : UINT_PTR { kSubtitleTimer = 1, kOsdTimer, kIdleTimer, kShowTimer };
enum : UINT { kCmdOpen = 1, kCmdExit };
// The button on the menu bar, then the controls: buttons left to right, the seek bar, and two labels.
enum Control { kNone, kFile, kPlay, kBack, kForward, kVolume, kSubtitles, kFullscreen, kSeek, kTime, kTitle };
constexpr UINT kShowFallbackMs = 1000;  // show the window even if the video's size isn't known by then
constexpr float kFadeSeconds = 0.2f;    // of the controls

struct Window {
    HWND hwnd = nullptr;
    int showCmd = SW_SHOWNORMAL;
    // Bottom to top: black background, video, subtitles at the bottom and top, seek / volume feedback, the
    // controls over the bottom of the video, and the menu bar, which the video doesn't overlap.
    ComPtr<ID3D11Device> device;  // for composition and the overlay
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, background, video, subtitleBottom, subtitleTop, osd, controls, menuBar;
    ComPtr<IDCompositionEffectGroup> controlsFade;
    ComPtr<IDXGISwapChain1> black;  // 8 x 8, stretched over the window
    bool attached = false;          // the engine's swap chain is the content of `video`
    SIZE videoSize{};               // swap chain size: the video fitted to the monitor
    RECT videoRect{};               // where the video is, in client pixels

    bool sized = false;  // the window has been sized to a video
    Subtitles subs;
    bool subsOn = true;
    std::vector<size_t> shown;  // indices of the cues on screen
    float subtitleHeight = 0;   // of the bottom ones
    std::wstring osdText;
    Control hot = kNone;    // under the mouse
    bool menuOpen = false;  // the File menu is showing
    std::wstring title;     // the video's file name
    bool controlsShown = false;
    Controls drawn{};  // what the controls show, at drawnSize and drawnScale
    SIZE drawnSize{};
    float drawnScale = 0;
    bool seeking = false;  // dragging along the seek bar
    // The engine's state, kept here so drawing never waits on the engine. playing changes as soon as it is
    // asked to, then follows the engine's events.
    bool playing = false;
    double duration = NAN;
    double volume = 1;
    bool muted = false;
    // One seek at a time is in flight; the latest one asked for meanwhile waits for it to finish.
    bool seekBusy = false, seekQueued = false, seekFast = false;
    double seekTarget = 0;  // of the latest seek
    bool tracking = false;  // for WM_MOUSELEAVE

    bool fullscreen = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};  // of the window before it went fullscreen
    bool cursorHidden = false;  // while playing with the mouse idle
    LPARAM lastMouse = -1;
} g;

std::wstring FileName(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

bool IsSubtitleFile(const std::wstring& path) {
    return path.size() >= 4 && CompareStringOrdinal(path.c_str() + path.size() - 4, 4, L".srt", 4, TRUE) == CSTR_EQUAL;
}

// "1:05:09" or "5:09".
std::wstring FormatTime(double seconds) {
    const long long s = std::isfinite(seconds) ? std::max(0LL, std::llround(std::floor(seconds))) : 0;
    wchar_t text[32];
    if (s >= 3600) {
        swprintf_s(text, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    } else {
        swprintf_s(text, L"%lld:%02lld", s / 60, s % 60);
    }
    return text;
}

// Height of a 16:9 frame as wide as the video, or the video's height if less: text sized from it fits
// portrait and widescreen video alike.
float TextScale() {
    const float w = float(g.videoRect.right - g.videoRect.left), h = float(g.videoRect.bottom - g.videoRect.top);
    return std::min(h, w * 9 / 16);
}

int Px(int dips) { return MulDiv(dips, GetDpiForWindow(g.hwnd), 96); }

// The menu bar is hidden in fullscreen.
int MenuBarHeight() { return g.fullscreen ? 0 : Px(26); }
int ControlsHeight() { return Px(150); }

// Where a control is, in client pixels: the seek bar with the time right of it, and below them a row of buttons
// left and right of the centered title.
RECT ControlRect(Control c) {
    RECT client;
    GetClientRect(g.hwnd, &client);
    if (c == kFile) return {0, 0, Px(40), MenuBarHeight()};
    const int margin = Px(16), step = Px(56), time = Px(64);
    const int seekY = client.bottom - Px(80), rowY = client.bottom - Px(34), half = Px(24);
    switch (c) {
        case kSeek: return {margin, seekY - Px(10), client.right - margin - time - Px(12), seekY + Px(10)};
        case kTime: return {client.right - margin - time, seekY - Px(10), client.right - margin, seekY + Px(10)};
        case kTitle: {
            const int center = int(client.right) / 2;
            const int w = std::max(0, center - margin - 4 * step);  // clear of the four buttons on the left
            return {center - w, rowY - half, center + w, rowY + half};
        }
        case kSubtitles:
        case kFullscreen: {
            const int x = client.right - margin - (kFullscreen - c + 1) * step;
            return {x, rowY - half, x + step, rowY + half};
        }
        default: {
            const int x = margin + (c - kPlay) * step;
            return {x, rowY - half, x + step, rowY + half};
        }
    }
}

// The controls take clicks from the top of the seek bar down, while they show.
bool IsOnControls(POINT pt) { return g.controlsShown && pt.y >= ControlRect(kSeek).top; }

bool IsOnBar(POINT pt) { return pt.y < MenuBarHeight() || IsOnControls(pt); }

Control HitTest(POINT pt) {
    for (const Control c : {kFile, kPlay, kBack, kForward, kVolume, kSubtitles, kFullscreen, kSeek}) {
        const RECT r = ControlRect(c);
        if (PtInRect(&r, pt) && (c == kFile || g.controlsShown)) return c;
    }
    return kNone;
}

void DrawMenuBar() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const BarItem file{ControlRect(kFile), L"File", 0, g.hot == kFile || g.menuOpen};
    DrawBar(g.dcomp.Get(), g.menuBar.Get(), client.right, MenuBarHeight(), GetDpiForWindow(g.hwnd) / 96.0f, {file});
}

const wchar_t* VolumeGlyph() {
    const double volume = g.volume;
    if (g.muted) return L"\xE74F";
    return volume == 0 ? L"\xE992" : volume < 1.0 / 3 ? L"\xE993" : volume < 2.0 / 3 ? L"\xE994" : L"\xE995";
}

// Where the video is, or will be once the seeks in flight are done.
double ShownTime() { return g.seekBusy || g.seeking ? g.seekTarget : PlaybackTime(); }

// Redraws the controls, unless they would look the same, and returns whether it did.
bool DrawControls() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const int h = std::min(ControlsHeight(), int(client.bottom)), top = client.bottom - h;
    const float scale = GetDpiForWindow(g.hwnd) / 96.0f;
    const bool off = !HasVideo();
    const auto rect = [&](Control c) {
        RECT r = ControlRect(c);
        OffsetRect(&r, 0, -top);
        return r;
    };
    const auto button = [&](Control c, std::wstring text, bool dim, int replay = 0) {
        return BarItem{rect(c), std::move(text), replay, g.hot == c && !dim, dim};
    };
    const double now = ShownTime(), duration = g.duration;
    const std::wstring step = std::to_wstring(std::lround(kSeekStep));
    // Progress to the nearest pixel of the track, so it only changes when the drawing would.
    const RECT seek = rect(kSeek);
    const double track = std::max(1L, seek.right - seek.left);
    Controls controls{
        seek,
        duration > 0 ? std::round(std::clamp(now / duration, 0.0, 1.0) * track) / track : 0.0,
        !off && (g.hot == kSeek || g.seeking),
        rect(kTime),
        rect(kTitle),
        off ? L"" : FormatTime(std::isfinite(duration) ? duration - now : now),
        g.title,
        {button(kPlay, g.playing ? L"\xE769" : L"\xF5B0", off), button(kBack, step, off, -1),
         button(kForward, step, off, 1), button(kVolume, VolumeGlyph(), false),
         button(kSubtitles, L"\xE7F0", off || g.subs.cues.empty()),
         button(kFullscreen, g.fullscreen ? L"\xE73F" : L"\xE740", false)}};
    g.controls->SetOffsetY(float(top));
    const SIZE size{client.right, h};
    if (controls == g.drawn && size.cx == g.drawnSize.cx && size.cy == g.drawnSize.cy && scale == g.drawnScale) {
        return false;
    }
    DrawControls(g.dcomp.Get(), g.controls.Get(), size.cx, size.cy, scale, controls);
    g.drawn = std::move(controls);
    g.drawnSize = size;
    g.drawnScale = scale;
    return true;
}

// Redraws the controls if they show and changed, for a new time, play state or volume.
void UpdateControls() {
    if (g.controlsShown && DrawControls()) g.dcomp->Commit();
}

void SetHot(Control c) {
    if (c == g.hot) return;
    const bool menu = c == kFile || g.hot == kFile;
    g.hot = c;
    if (menu) DrawMenuBar();
    if (g.controlsShown) DrawControls();
    g.dcomp->Commit();
}

// Keeps the bottom subtitles above the controls while they show.
void PlaceBottomSubtitles() {
    const RECT& v = g.videoRect;
    float y = v.bottom - (v.bottom - v.top) * 0.06f - g.subtitleHeight;
    if (g.controlsShown) y = std::min(y, float(ControlRect(kSeek).top) - g.subtitleHeight);
    g.subtitleBottom->SetOffsetY(std::round(std::max(float(v.top), y)));
}

// Fades the controls in or out.
void SetControlsShown(bool shown) {
    if (shown == g.controlsShown) return;
    g.controlsShown = shown;
    if (shown) {
        DrawControls();
    } else if (g.hot != kFile) {
        g.hot = kNone;
    }
    const float from = shown ? 0.0f : 1.0f, to = 1 - from;
    ComPtr<IDCompositionAnimation> fade;
    if (SUCCEEDED(g.dcomp->CreateAnimation(&fade))) {
        fade->AddCubic(0, from, (to - from) / kFadeSeconds, 0, 0);
        fade->End(kFadeSeconds, to);
        g.controlsFade->SetOpacity(fade.Get());
    } else {
        g.controlsFade->SetOpacity(to);
    }
    PlaceBottomSubtitles();
    g.dcomp->Commit();
}

void SetCursorHidden(bool hidden) {
    if (g.cursorHidden == hidden) return;
    g.cursorHidden = hidden;
    POINT pt;
    GetCursorPos(&pt);
    SetCursorPos(pt.x, pt.y);  // makes Windows send WM_SETCURSOR now
}

// Shows the controls and the cursor until the mouse has been idle for a while.
void Wake() {
    SetCursorHidden(false);
    SetControlsShown(true);
    SetTimer(g.hwnd, kIdleTimer, kIdleMs, nullptr);
}

// Hides the controls and the cursor while playing, unless the mouse is on the controls or the menu bar.
void OnIdle() {
    KillTimer(g.hwnd, kIdleTimer);
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g.hwnd, &pt);
    if (!g.playing || g.seeking || IsOnBar(pt)) return;
    SetControlsShown(false);
    SetCursorHidden(true);
}

void DrawSubtitles() {
    std::wstring text[2];  // bottom, top
    std::vector<StyleSpan> spans[2];
    for (const size_t i : g.shown) {
        const Cue& cue = g.subs.cues[i];
        std::wstring& t = text[cue.top];
        if (!t.empty()) t += L'\n';
        const UINT32 at = UINT32(t.size());
        t += cue.text;
        for (StyleSpan s : cue.spans) {
            s.start += at;
            spans[cue.top].push_back(s);
        }
    }
    const RECT& v = g.videoRect;
    const float videoW = float(v.right - v.left);
    const float fontSize = std::max(12.0f, TextScale() * 0.055f), margin = (v.bottom - v.top) * 0.06f;
    for (int top = 0; top < 2; ++top) {
        IDCompositionVisual* visual = top ? g.subtitleTop.Get() : g.subtitleBottom.Get();
        const SIZE s = DrawOutlinedText(g.dcomp.Get(), visual, text[top], spans[top], fontSize, videoW * 0.9f, true);
        visual->SetOffsetX(std::round(v.left + (videoW - float(s.cx)) / 2));
        if (top) {
            visual->SetOffsetY(std::round(v.top + margin));
        } else {
            g.subtitleHeight = float(s.cy);
            PlaceBottomSubtitles();
        }
    }
}

// Shows the cues for the current time and sets a timer for the next change. TIMEUPDATE events (4 a second
// while playing) also call this, so a late timer never leaves a cue up long.
void UpdateSubtitles(bool redraw) {
    KillTimer(g.hwnd, kSubtitleTimer);
    std::vector<size_t> active;
    double next = INFINITY;
    const double now = PlaybackTime();
    if (g.subsOn && HasVideo()) next = ActiveCues(g.subs, now, active);
    if (redraw || active != g.shown) {
        g.shown = std::move(active);
        DrawSubtitles();
        g.dcomp->Commit();
    }
    if (g.playing && std::isfinite(next)) {
        SetTimer(g.hwnd, kSubtitleTimer, UINT(std::clamp(std::ceil((next - now) * 1000) + 1, 1.0, 3600000.0)),
                 nullptr);
    }
}

void DrawOsd() {
    const float fontSize =
        std::max(12.0f, (HasVideo() ? TextScale() : float(g.videoRect.bottom - g.videoRect.top)) * 0.035f);
    const float margin = std::round(fontSize * 0.8f);
    DrawOutlinedText(g.dcomp.Get(), g.osd.Get(), g.osdText, {}, fontSize, float(g.videoRect.right), false);
    g.osd->SetOffsetX(margin);
    g.osd->SetOffsetY(float(MenuBarHeight()) + margin);
}

// Shows text in the top left for a moment, or until replaced if it stays.
void ShowOsd(std::wstring text, bool stays = false) {
    g.osdText = std::move(text);
    DrawOsd();
    g.dcomp->Commit();
    if (stays) {
        KillTimer(g.hwnd, kOsdTimer);
    } else {
        SetTimer(g.hwnd, kOsdTimer, kOsdMs, nullptr);
    }
}

void HideOsd() {
    KillTimer(g.hwnd, kOsdTimer);
    if (g.osdText.empty()) return;
    g.osdText.clear();
    g.osd->SetContent(nullptr);
    g.dcomp->Commit();
}

// v scaled to fit w x h, keeping its aspect ratio.
SIZE Fit(SIZE v, int w, int h) {
    const double scale = std::min(double(w) / v.cx, double(h) / v.cy);
    return {std::max(1L, LONG(std::lround(v.cx * scale))), std::max(1L, LONG(std::lround(v.cy * scale)))};
}

// Fits the video below the menu bar, keeping its aspect ratio. The engine renders it at the size it fills the
// monitor at, which only changes with the video or the monitor, and composition scales that to the window, so
// resizing and fullscreen take effect in the same frame as the window, with no wait on the engine.
void Layout() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const int cw = client.right, ch = client.bottom;
    if (cw <= 0 || ch <= 0) return;  // minimized
    g.background->SetTransform(D2D_MATRIX_3X2_F{cw / 8.0f, 0, 0, ch / 8.0f, 0, 0});
    DrawMenuBar();
    if (g.controlsShown) DrawControls();
    const int top = MenuBarHeight(), ah = std::max(1, ch - top);

    const SIZE v = VideoSize();
    if (v.cx > 0 && v.cy > 0) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const SIZE chain = Fit(v, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
        if (chain.cx != g.videoSize.cx || chain.cy != g.videoSize.cy || !g.attached) {
            SetVideoSize(chain.cx, chain.cy);
            g.videoSize = chain;
        }
        const SIZE s = Fit(v, cw, ah);
        const int x = (cw - s.cx) / 2, y = top + (ah - s.cy) / 2;
        g.videoRect = {x, y, x + s.cx, y + s.cy};
        g.video->SetTransform(D2D_MATRIX_3X2_F{float(s.cx) / chain.cx, 0, 0, float(s.cy) / chain.cy, float(x),
                                               float(y)});
        // The swap chain exists once the engine has a size for it.
        if (!g.attached) {
            if (const HANDLE surface = VideoSurface()) {
                ComPtr<IUnknown> content;
                g.attached = SUCCEEDED(g.dcomp->CreateSurfaceFromHandle(surface, &content));
                if (g.attached) g.video->SetContent(content.Get());
                CloseHandle(surface);
            }
        }
    } else {
        g.videoRect = {0, top, cw, top + ah};
    }
    UpdateSubtitles(true);
    if (!g.osdText.empty()) DrawOsd();
    g.dcomp->Commit();
}

// Sizes the window to the video (capped to the work area) or a default size, centered on the active monitor.
void PlaceWindow() {
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const double workW = work.right - work.left, workH = work.bottom - work.top;

    const UINT dpi = GetDpiForWindow(g.hwnd);
    RECT rc{0, 0, MulDiv(960, dpi, 96), MulDiv(540, dpi, 96)};
    if (const SIZE v = VideoSize(); v.cx > 0 && v.cy > 0) {
        const double scale = std::min({1.0, workW * 0.85 / v.cx, workH * 0.85 / v.cy});
        rc.right = std::max(LONG(std::lround(v.cx * scale)), LONG(MulDiv(320, dpi, 96)));
        rc.bottom = std::max(LONG(std::lround(v.cy * scale)), LONG(MulDiv(180, dpi, 96)));
    }
    rc.bottom += MenuBarHeight();
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(g.hwnd, nullptr, work.left + (int(workW) - w) / 2, work.top + (int(workH) - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void ShowMainWindow() {
    KillTimer(g.hwnd, kShowTimer);
    if (IsWindowVisible(g.hwnd)) return;
    if (!g.sized) PlaceWindow();
    ShowWindow(g.hwnd, g.showCmd);
    Wake();
}

void ShowError(const std::wstring& message) {
    MessageBoxW(IsWindowVisible(g.hwnd) ? g.hwnd : nullptr, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

void ShowVideoError(LPARAM code) {
    std::wstring message = L"Can't play this video.";
    switch (code) {
        case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED:
            message += L"\n\nIts format isn't supported. Codecs for HEVC, AV1 and VP9 video are available from "
                       L"the Microsoft Store.";
            break;
        case MF_MEDIA_ENGINE_ERR_DECODE: message += L"\n\nThe file is damaged or uses an unsupported codec."; break;
        case MF_MEDIA_ENGINE_ERR_ENCRYPTED: message += L"\n\nIt is copy-protected."; break;
    }
    ShowError(message);
}

void LoadSubtitleFile(const std::wstring& path) {
    if (!LoadSubtitles(path, g.subs)) {
        ShowError(L"Can't open the subtitles \"" + FileName(path) + L"\".\n\nIt isn't a readable SRT file.");
        return;
    }
    g.subsOn = true;
    UpdateSubtitles(true);
    UpdateControls();
    if (IsWindowVisible(g.hwnd)) ShowOsd(L"Subtitles: " + FileName(path));
}

bool OpenVideoFile(const std::wstring& path) {
    const HRESULT hr = OpenVideo(path);
    if (FAILED(hr)) {
        const bool missing = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                             hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        ShowError(L"Can't open \"" + FileName(path) + L"\"." + (missing ? L"\n\nThe file doesn't exist." : L""));
        return false;
    }
    // Seeks and subtitles belong to the video they were for.
    g.duration = NAN;
    g.seekBusy = g.seekQueued = false;
    g.subs = {};
    UpdateSubtitles(false);
    HideOsd();
    SetWindowTextW(g.hwnd, FileName(path).c_str());
    g.title = FileName(path);
    g.title = g.title.substr(0, g.title.find_last_of(L'.'));
    UpdateControls();
    return true;
}

// Plays the first video among paths, with the first subtitle file.
bool OpenFiles(const std::vector<std::wstring>& paths) {
    const auto video = std::find_if_not(paths.begin(), paths.end(), IsSubtitleFile);
    const auto subs = std::find_if(paths.begin(), paths.end(), IsSubtitleFile);
    if (video != paths.end() && !OpenVideoFile(*video)) return false;
    if (subs != paths.end()) LoadSubtitleFile(*subs);
    return true;
}

void Open() {
    const std::wstring path = RunFileDialog(g.hwnd);
    if (!path.empty()) OpenFiles({path});
}

void ShowFileMenu() {
    if (g.fullscreen) return;
    const HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdOpen, L"&Open...\tCtrl+O");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"E&xit");
    const RECT r = ControlRect(kFile);
    POINT pt{r.left, r.bottom};
    ClientToScreen(g.hwnd, &pt);
    g.menuOpen = true;
    DrawMenuBar();
    g.dcomp->Commit();
    const UINT cmd = UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, g.hwnd, nullptr));
    DestroyMenu(menu);
    // The menu had the mouse while it showed.
    g.menuOpen = false;
    GetCursorPos(&pt);
    ScreenToClient(g.hwnd, &pt);
    g.hot = HitTest(pt);
    DrawMenuBar();
    if (g.controlsShown) DrawControls();
    g.dcomp->Commit();
    if (cmd == kCmdOpen) {
        Open();
    } else if (cmd == kCmdExit) {
        DestroyWindow(g.hwnd);
    }
}

// Seeks to a time within the video and returns it. While a seek is in flight the new one waits, replacing any
// that was already waiting, so dragging or holding an arrow key never piles up seeks in the engine.
double RequestSeek(double seconds, bool fast) {
    g.seekTarget = std::max(0.0, std::isfinite(g.duration) ? std::min(seconds, g.duration) : seconds);
    g.seekFast = fast;
    if (g.seekBusy) {
        g.seekQueued = true;
    } else {
        g.seekBusy = true;
        SeekTo(g.seekTarget, fast);
    }
    UpdateControls();
    return g.seekTarget;
}

// A seek finished: start the waiting one, if any.
void OnSeeked() {
    if (g.seekQueued) {
        g.seekQueued = false;
        SeekTo(g.seekTarget, g.seekFast);
    } else {
        g.seekBusy = false;
    }
}

void Seek(double seconds) {
    const double target = RequestSeek(ShownTime() + seconds, false);
    ShowOsd(FormatTime(target) + L" / " + FormatTime(g.duration));
}

// Plays or pauses, showing it at once rather than when the engine reports it.
void TogglePlay() {
    g.playing = !g.playing;
    SetPlaying(g.playing);
    UpdateSubtitles(false);
    UpdateControls();
}

void ToggleMuted() {
    g.muted = ToggleMute();
    ShowOsd(g.muted ? L"Muted" : L"Unmuted");
}

void ToggleSubtitles() {
    if (g.subs.cues.empty()) return;
    g.subsOn = !g.subsOn;
    UpdateSubtitles(false);
    ShowOsd(g.subsOn ? L"Subtitles on" : L"Subtitles off");
}

// Seeks to the time under x on the seek bar.
void SeekToX(int x, bool fast) {
    const RECT r = ControlRect(kSeek);
    const double duration = g.duration;
    if (!(duration > 0)) return;
    RequestSeek(std::clamp(double(x - r.left) / std::max(1L, r.right - r.left), 0.0, 1.0) * duration, fast);
}

// Switches between the normal window and a borderless one covering its monitor.
void ToggleFullscreen() {
    g.fullscreen = !g.fullscreen;
    g.hot = kNone;
    g.seeking = false;
    const LONG_PTR style = GetWindowLongPtrW(g.hwnd, GWL_STYLE);
    if (g.fullscreen) {
        GetWindowPlacement(g.hwnd, &g.placement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(g.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g.hwnd, &g.placement);
        SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    Wake();
}

void Press(Control c) {
    if (c == kFile) {
        ShowFileMenu();
    } else if (c == kVolume) {
        ToggleMuted();
    } else if (c == kFullscreen) {
        ToggleFullscreen();
    } else if (c != kNone && c != kSeek && HasVideo()) {
        if (c == kPlay) {
            TogglePlay();
        } else if (c == kSubtitles) {
            ToggleSubtitles();
        } else {
            Seek(c == kBack ? -kSeekStep : kSeekStep);
        }
    }
}

void OnMediaEvent(WPARAM event, LPARAM param) {
    switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            g.attached = false;  // a new source may bring a new swap chain
            g.duration = Duration();
            if (!g.sized && HasVideo()) {
                PlaceWindow();
                g.sized = true;
            }
            ShowMainWindow();
            Layout();
            break;
        case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE:
            g.attached = false;
            Layout();
            break;
        case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
            Layout();
            break;
        case MF_MEDIA_ENGINE_EVENT_PLAYING:
            g.playing = IsPlaying();  // not if paused again since
            // Keep the display on while playing.
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
            UpdateSubtitles(false);
            UpdateControls();
            SetTimer(g.hwnd, kIdleTimer, kIdleMs, nullptr);
            break;
        case MF_MEDIA_ENGINE_EVENT_PAUSE:
        case MF_MEDIA_ENGINE_EVENT_ENDED:
            g.playing = IsPlaying();
            SetThreadExecutionState(ES_CONTINUOUS);
            UpdateSubtitles(false);
            Wake();
            UpdateControls();
            break;
        case MF_MEDIA_ENGINE_EVENT_SEEKED:
            OnSeeked();
            [[fallthrough]];
        case MF_MEDIA_ENGINE_EVENT_TIMEUPDATE:
            UpdateSubtitles(false);
            UpdateControls();
            break;
        case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
            g.duration = Duration();
            UpdateControls();
            break;
        case MF_MEDIA_ENGINE_EVENT_VOLUMECHANGE:
            g.volume = Volume();
            g.muted = IsMuted();
            UpdateControls();
            break;
        case MF_MEDIA_ENGINE_EVENT_ERROR: {
            g.playing = g.seekBusy = g.seekQueued = false;
            SetThreadExecutionState(ES_CONTINUOUS);
            // The video given on the command line failed before the window showed: like a failed start, close.
            const bool starting = !IsWindowVisible(g.hwnd);
            KillTimer(g.hwnd, kShowTimer);
            ShowVideoError(param);
            if (starting) DestroyWindow(g.hwnd);
            break;
        }
    }
}

void OnKey(WPARAM key, bool repeat) {
    if (GetKeyState(VK_CONTROL) < 0) {
        if (key == 'O' && !repeat) Open();
        return;
    }
    const bool video = HasVideo();
    switch (key) {
        case VK_SPACE:
            if (!repeat && video) TogglePlay();
            break;
        case VK_LEFT:
        case VK_RIGHT:
            if (video) Seek(key == VK_LEFT ? -kSeekStep : kSeekStep);
            break;
        case VK_UP:
        case VK_DOWN: {
            const double volume = g.volume = ChangeVolume(key == VK_UP ? kVolumeStep : -kVolumeStep);
            ShowOsd(L"Volume " + std::to_wstring(std::lround(volume * 100)) + L"%");
            break;
        }
        case 'M':
            if (!repeat) ToggleMuted();
            break;
        case 'S':
            if (!repeat) ToggleSubtitles();
            break;
        case VK_F11:
            if (!repeat) ToggleFullscreen();
            break;
        case VK_ESCAPE:
            if (repeat) break;
            if (g.fullscreen) {
                ToggleFullscreen();
            } else {
                DestroyWindow(g.hwnd);
            }
            break;
    }
}

void OnDrop(HDROP drop) {
    std::vector<std::wstring> paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        if (len && DragQueryFileW(drop, i, path.data(), len + 1) == len) paths.push_back(std::move(path));
    }
    DragFinish(drop);
    SetForegroundWindow(g.hwnd);
    OpenFiles(paths);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_MEDIA:
            OnMediaEvent(wp, lp);
            return 0;
        case WM_PAINT:  // everything is drawn by DirectComposition
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && g.dcomp) Layout();
            return 0;
        case WM_EXITSIZEMOVE:  // it may be on another monitor now
        case WM_DISPLAYCHANGE:
            if (!IsIconic(hwnd) && g.dcomp) Layout();
            return 0;
        case WM_TIMER:
            switch (wp) {
                case kSubtitleTimer: UpdateSubtitles(false); break;
                case kOsdTimer: HideOsd(); break;
                case kShowTimer: ShowMainWindow(); break;
                case kIdleTimer: OnIdle(); break;
            }
            return 0;
        case WM_KEYDOWN:
            OnKey(wp, (lp & (1 << 30)) != 0);
            return 0;
        case WM_SYSCHAR:  // Alt+F
            if (wp == 'f' || wp == 'F') {
                ShowFileMenu();
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Control c = HitTest(pt);
            if (c == kSeek && HasVideo()) {
                g.seeking = true;
                SetCapture(hwnd);
                SeekToX(pt.x, true);
            } else if (IsOnBar(pt)) {  // a double click there is just a second click
                Press(c);
            } else if (msg == WM_LBUTTONDOWN) {
                if (HasVideo()) TogglePlay();
            } else {  // the first click toggled playback; undo that
                if (HasVideo()) TogglePlay();
                ToggleFullscreen();
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g.seeking) {
                g.seeking = false;
                SeekToX(GET_X_LPARAM(lp), false);  // lands on the exact frame
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            g.seeking = false;
            return 0;
        case WM_MOUSEMOVE:
            if (lp != g.lastMouse) Wake();  // not the move SetCursorPos fakes
            g.lastMouse = lp;
            if (!g.tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                g.tracking = TrackMouseEvent(&tme);
            }
            if (g.seeking) {
                SeekToX(GET_X_LPARAM(lp), true);
            } else {
                SetHot(HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            }
            return 0;
        case WM_MOUSELEAVE:
            g.tracking = false;
            SetHot(kNone);
            if (g.playing && !g.seeking) SetControlsShown(false);
            return 0;
        case WM_SETCURSOR:
            if (g.cursorHidden && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_DROPFILES:
            OnDrop(reinterpret_cast<HDROP>(wp));
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            ShutdownPlayer();
            SetThreadExecutionState(ES_CONTINUOUS);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The visual tree, and an 8 x 8 black swap chain that is stretched behind the video. The window has no
// redirection surface, so anything not covered would show through. The UI draws on a device of its own, so it
// never waits on, or stalls, the engine's decoding and presenting.
bool InitComposition() {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &g.device, nullptr, nullptr);
    ID3D11Device* device = g.device.Get();
    if (SUCCEEDED(hr)) hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&g.dcomp));
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateTargetForHwnd(g.hwnd, TRUE, &g.target);
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(&g.root);
    for (IDCompositionVisual** v : {g.background.GetAddressOf(), g.video.GetAddressOf(),
                                    g.subtitleBottom.GetAddressOf(), g.subtitleTop.GetAddressOf(),
                                    g.osd.GetAddressOf(), g.controls.GetAddressOf(), g.menuBar.GetAddressOf()}) {
        if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(v);
        if (SUCCEEDED(hr)) hr = g.root->AddVisual(*v, FALSE, nullptr);  // in front of the previous ones
    }
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateEffectGroup(&g.controlsFade);
    if (SUCCEEDED(hr)) hr = g.controlsFade->SetOpacity(0.0f);
    if (SUCCEEDED(hr)) hr = g.controls->SetEffect(g.controlsFade.Get());
    if (SUCCEEDED(hr)) hr = g.target->SetRoot(g.root.Get());

    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 8;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForComposition(device, &desc, nullptr, &g.black);
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11Texture2D> buffer;
        ComPtr<ID3D11RenderTargetView> view;
        ComPtr<ID3D11DeviceContext> context;
        hr = g.black->GetBuffer(0, IID_PPV_ARGS(&buffer));
        if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(buffer.Get(), nullptr, &view);
        if (SUCCEEDED(hr)) {
            device->GetImmediateContext(&context);
            const float black[4] = {0, 0, 0, 1};
            context->ClearRenderTargetView(view.Get(), black);
            hr = g.black->Present(0, 0);
        }
    }
    if (SUCCEEDED(hr)) {
        g.background->SetContent(g.black.Get());
        g.background->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        g.background->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
        g.video->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        SetOverlayDevice(device);
    }
    return SUCCEEDED(hr);
}

HWND CreateMainWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kAppId;
    if (!RegisterClassExW(&wc)) return nullptr;

    // Accepting dropped files through the style avoids loading shell32 until something is dropped.
    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_ACCEPTFILES, kAppId, kAppName,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                nullptr, nullptr, instance, nullptr);
    if (hwnd) {
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    return hwnd;
}

}  // namespace

bool StartPlayer(HINSTANCE instance, const std::vector<std::wstring>& files, int showCmd) {
    g.showCmd = showCmd;
    g.hwnd = CreateMainWindow(instance);
    if (!g.hwnd) return false;
    if (!InitPlayer(g.hwnd) || !InitComposition()) {
        MessageBoxW(nullptr, L"Can't start: Direct3D 11 or Media Foundation isn't available.", kAppName,
                    MB_OK | MB_ICONERROR);
        return false;
    }
    if (std::none_of(files.begin(), files.end(), [](const std::wstring& f) { return !IsSubtitleFile(f); })) {
        ShowMainWindow();
        Layout();
        ShowOsd(L"Drop a video here or press Ctrl+O", true);
        return true;
    }
    if (!OpenFiles(files)) return false;
    // The window shows when the video's size is known (LOADEDMETADATA) so it opens at the right size.
    SetTimer(g.hwnd, kShowTimer, kShowFallbackMs, nullptr);
    return true;
}
