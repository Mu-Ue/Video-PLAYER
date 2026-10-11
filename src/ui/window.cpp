#include "ui/window.h"

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

enum : UINT_PTR { kSubtitleTimer = 1, kOsdTimer, kCursorTimer, kShowTimer };
enum : UINT { kCmdOpen = 1, kCmdExit };
enum Control { kNone, kFile, kBack, kPlay, kForward };  // the buttons on the menu and control bars
constexpr UINT kShowFallbackMs = 1000;  // show the window even if the video's size isn't known by then

struct Window {
    HWND hwnd = nullptr;
    int showCmd = SW_SHOWNORMAL;
    // Bottom to top: black background, video, subtitles at the bottom and top, seek / volume feedback, and the
    // menu and control bars, which the video doesn't overlap.
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, background, video, subtitleBottom, subtitleTop, osd, menuBar, controlBar;
    ComPtr<IDXGISwapChain1> black;  // 8 x 8, stretched over the window
    bool attached = false;          // the engine's swap chain is the content of `video`
    SIZE videoSize{};               // swap chain size
    RECT videoRect{};               // where the video is, in client pixels

    bool sized = false;  // the window has been sized to a video
    Subtitles subs;
    bool subsOn = true;
    std::vector<size_t> shown;  // indices of the cues on screen
    std::wstring osdText;
    Control hot = kNone;    // under the mouse
    bool menuOpen = false;  // the File menu is showing
    std::wstring barTime;   // the time and play state the control bar shows
    bool barPlaying = false;

    bool fullscreen = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};  // of the window before it went fullscreen
    bool cursorHidden = false;
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

// The bars are hidden in fullscreen.
int MenuBarHeight() { return g.fullscreen ? 0 : MulDiv(26, GetDpiForWindow(g.hwnd), 96); }
int ControlBarHeight() { return g.fullscreen ? 0 : MulDiv(40, GetDpiForWindow(g.hwnd), 96); }

// Where a button is, in client pixels.
RECT ControlRect(Control c) {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const UINT dpi = GetDpiForWindow(g.hwnd);
    const int w = MulDiv(40, dpi, 96);
    if (c == kFile) return {0, 0, w, MenuBarHeight()};
    const int x = MulDiv(4, dpi, 96) + (c - kBack) * w;
    return {x, client.bottom - ControlBarHeight(), x + w, client.bottom};
}

Control HitTest(POINT pt) {
    for (const Control c : {kFile, kBack, kPlay, kForward}) {
        const RECT r = ControlRect(c);
        if (PtInRect(&r, pt)) return c;
    }
    return kNone;
}

bool IsOnBar(POINT pt) {
    RECT client;
    GetClientRect(g.hwnd, &client);
    return pt.y < MenuBarHeight() || pt.y >= client.bottom - ControlBarHeight();
}

BarItem Button(Control c, std::wstring text, bool icon, bool dim = false) {
    const RECT r = ControlRect(c);
    return {float(r.left), float(r.right - r.left), std::move(text), icon, false, g.hot == c && !dim, dim};
}

void DrawMenuBar() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    BarItem file = Button(kFile, L"File", false);
    file.hot |= g.menuOpen;
    DrawBar(g.dcomp.Get(), g.menuBar.Get(), client.right, MenuBarHeight(), GetDpiForWindow(g.hwnd) / 96.0f, {file});
}

std::wstring BarTime() { return HasVideo() ? FormatTime(PlaybackTime()) + L" / " + FormatTime(Duration()) : L""; }

// Seek back, play / pause and seek forward buttons, then the time.
void DrawControlBar() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const UINT dpi = GetDpiForWindow(g.hwnd);
    const int h = ControlBarHeight();
    const bool off = !HasVideo();
    g.barPlaying = IsPlaying();
    g.barTime = BarTime();
    const float x = float(ControlRect(kForward).right + MulDiv(8, dpi, 96));
    DrawBar(g.dcomp.Get(), g.controlBar.Get(), client.right, h, dpi / 96.0f,
            {Button(kBack, L"\xEB9E", true, off), Button(kPlay, g.barPlaying ? L"\xE769" : L"\xE768", true, off),
             Button(kForward, L"\xEB9D", true, off), {x, std::max(0.0f, client.right - x), g.barTime, false, true}});
    g.controlBar->SetOffsetY(float(client.bottom - h));
}

// Redraws the control bar if the time or play state it shows changed.
void UpdateControlBar() {
    if (g.fullscreen || (IsPlaying() == g.barPlaying && BarTime() == g.barTime)) return;
    DrawControlBar();
    g.dcomp->Commit();
}

void SetHot(Control c) {
    if (c == g.hot) return;
    const bool menu = c == kFile || g.hot == kFile;
    g.hot = c;
    if (menu) DrawMenuBar();
    DrawControlBar();
    g.dcomp->Commit();
    if (c != kNone) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g.hwnd, 0};
        TrackMouseEvent(&tme);
    }
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
    const float videoW = float(v.right - v.left), videoH = float(v.bottom - v.top);
    const float fontSize = std::max(12.0f, TextScale() * 0.055f), margin = videoH * 0.06f;
    for (int top = 0; top < 2; ++top) {
        IDCompositionVisual* visual = top ? g.subtitleTop.Get() : g.subtitleBottom.Get();
        const SIZE s = DrawOutlinedText(g.dcomp.Get(), visual, text[top], spans[top], fontSize, videoW * 0.9f, true);
        visual->SetOffsetX(std::round(v.left + (videoW - float(s.cx)) / 2));
        visual->SetOffsetY(std::round(top ? v.top + margin : std::max(float(v.top), v.bottom - margin - float(s.cy))));
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
    if (IsPlaying() && std::isfinite(next)) {
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

// Fits the video between the bars, keeping its aspect ratio; the engine scales it on the GPU.
void Layout() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const int cw = client.right, ch = client.bottom;
    if (cw <= 0 || ch <= 0) return;  // minimized
    g.background->SetTransform(D2D_MATRIX_3X2_F{cw / 8.0f, 0, 0, ch / 8.0f, 0, 0});
    DrawMenuBar();
    DrawControlBar();
    const int top = MenuBarHeight(), ah = std::max(1, ch - top - ControlBarHeight());

    const SIZE v = VideoSize();
    if (v.cx > 0 && v.cy > 0) {
        const double scale = std::min(double(cw) / v.cx, double(ah) / v.cy);
        const int w = std::max(1, int(std::lround(v.cx * scale))), h = std::max(1, int(std::lround(v.cy * scale)));
        const int x = (cw - w) / 2, y = top + (ah - h) / 2;
        if (w != g.videoSize.cx || h != g.videoSize.cy || !g.attached) {
            SetVideoSize(w, h);
            g.videoSize = {w, h};
        }
        g.videoRect = {x, y, x + w, y + h};
        g.video->SetOffsetX(float(x));
        g.video->SetOffsetY(float(y));
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
    rc.bottom += MenuBarHeight() + ControlBarHeight();
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
    // Subtitles belong to the video they were loaded for.
    g.subs = {};
    UpdateSubtitles(false);
    HideOsd();
    SetWindowTextW(g.hwnd, FileName(path).c_str());
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
    DrawControlBar();
    g.dcomp->Commit();
    if (cmd == kCmdOpen) {
        Open();
    } else if (cmd == kCmdExit) {
        DestroyWindow(g.hwnd);
    }
}

void Seek(double seconds) {
    const double target = SeekBy(seconds);
    ShowOsd(FormatTime(target) + L" / " + FormatTime(Duration()));
}

void Press(Control c) {
    if (c == kFile) {
        ShowFileMenu();
    } else if (c != kNone && HasVideo()) {
        if (c == kPlay) {
            TogglePlay();
        } else {
            Seek(c == kBack ? -kSeekStep : kSeekStep);
        }
    }
}

void SetCursorHidden(bool hidden) {
    if (g.cursorHidden == hidden) return;
    g.cursorHidden = hidden;
    POINT pt;
    GetCursorPos(&pt);
    SetCursorPos(pt.x, pt.y);  // makes Windows send WM_SETCURSOR now
}

// Switches between the normal window and a borderless one covering its monitor.
void ToggleFullscreen() {
    g.fullscreen = !g.fullscreen;
    g.hot = kNone;
    const LONG_PTR style = GetWindowLongPtrW(g.hwnd, GWL_STYLE);
    if (g.fullscreen) {
        GetWindowPlacement(g.hwnd, &g.placement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(g.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        SetTimer(g.hwnd, kCursorTimer, kCursorHideMs, nullptr);
    } else {
        KillTimer(g.hwnd, kCursorTimer);
        SetCursorHidden(false);
        SetWindowLongPtrW(g.hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g.hwnd, &g.placement);
        SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

void OnMediaEvent(WPARAM event, LPARAM param) {
    switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            g.attached = false;  // a new source may bring a new swap chain
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
            // Keep the display on while playing.
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
            UpdateSubtitles(false);
            UpdateControlBar();
            break;
        case MF_MEDIA_ENGINE_EVENT_PAUSE:
        case MF_MEDIA_ENGINE_EVENT_ENDED:
            SetThreadExecutionState(ES_CONTINUOUS);
            UpdateSubtitles(false);
            UpdateControlBar();
            break;
        case MF_MEDIA_ENGINE_EVENT_SEEKED:
        case MF_MEDIA_ENGINE_EVENT_TIMEUPDATE:
            UpdateSubtitles(false);
            UpdateControlBar();
            break;
        case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
            UpdateControlBar();
            break;
        case MF_MEDIA_ENGINE_EVENT_ERROR: {
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
            const double volume = ChangeVolume(key == VK_UP ? kVolumeStep : -kVolumeStep);
            ShowOsd(L"Volume " + std::to_wstring(std::lround(volume * 100)) + L"%");
            break;
        }
        case 'M':
            if (!repeat) ShowOsd(ToggleMute() ? L"Muted" : L"Unmuted");
            break;
        case 'S':
            if (!repeat && !g.subs.cues.empty()) {
                g.subsOn = !g.subsOn;
                UpdateSubtitles(false);
                ShowOsd(g.subsOn ? L"Subtitles on" : L"Subtitles off");
            }
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
        case WM_TIMER:
            switch (wp) {
                case kSubtitleTimer: UpdateSubtitles(false); break;
                case kOsdTimer: HideOsd(); break;
                case kShowTimer: ShowMainWindow(); break;
                case kCursorTimer:
                    KillTimer(hwnd, kCursorTimer);
                    SetCursorHidden(true);
                    break;
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
            if (IsOnBar(pt)) {  // a double click there is just a second click
                Press(HitTest(pt));
            } else if (msg == WM_LBUTTONDOWN) {
                if (HasVideo()) TogglePlay();
            } else {  // the first click toggled playback; undo that
                if (HasVideo()) TogglePlay();
                ToggleFullscreen();
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g.fullscreen && lp != g.lastMouse) {  // not the move SetCursorPos fakes
                SetCursorHidden(false);
                SetTimer(hwnd, kCursorTimer, kCursorHideMs, nullptr);
            }
            g.lastMouse = lp;
            SetHot(HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            return 0;
        case WM_MOUSELEAVE:
            SetHot(kNone);
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
// redirection surface, so anything not covered would show through.
bool InitComposition() {
    ID3D11Device* device = GpuDevice();
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&g.dcomp));
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateTargetForHwnd(g.hwnd, TRUE, &g.target);
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(&g.root);
    for (IDCompositionVisual** v : {g.background.GetAddressOf(), g.video.GetAddressOf(),
                                    g.subtitleBottom.GetAddressOf(), g.subtitleTop.GetAddressOf(),
                                    g.osd.GetAddressOf(), g.menuBar.GetAddressOf(), g.controlBar.GetAddressOf()}) {
        if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(v);
        if (SUCCEEDED(hr)) hr = g.root->AddVisual(*v, FALSE, nullptr);  // in front of the previous ones
    }
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
        DeviceLock lock;
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
