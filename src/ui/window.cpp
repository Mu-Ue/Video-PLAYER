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
#include <climits>
#include <cmath>
#include <memory>
#include <unordered_map>

#include "config.h"
#include "media/mkv.h"
#include "media/player.h"
#include "media/subtitles.h"
#include "platform/file_dialog.h"
#include "ui/overlay.h"
#include "ui/popup_menu.h"

using Microsoft::WRL::ComPtr;

namespace {

enum : UINT_PTR { kOsdTimer = 1, kIdleTimer, kShowTimer };
enum : UINT { kCmdOpen = 1, kCmdAddSubtitles };
// The buttons on the menu bar, then the controls: buttons left to right, the seek bar, the volume slider, and two
// labels.
enum Control {
    kNone, kFile, kSubs, kPlay, kBack, kForward, kVolume, kSubtitles, kFullscreen, kSeek, kVolumeBar, kTime, kTitle
};
constexpr UINT kShowFallbackMs = 1000;  // show the window even if the video's size isn't known by then
constexpr float kFadeSeconds = 0.2f;    // of the controls
// The icon popped up in the middle: in by kFlashIn, out from kFlashOut to kFlashEnd, in seconds.
constexpr float kFlashIn = 0.15f, kFlashOut = 0.55f, kFlashEnd = 0.8f;

// A subtitle track: one of the video's own, whose cues are added as they are read or show, or an SRT file.
struct SubtitleTrack {
    std::wstring label;
    std::wstring path;  // of the file; empty for the video's own
    DWORD id = 0;       // of the video's own
    Subtitles subs;
    bool read = false;  // one of the video's own that the player reads from the file itself
};

// A column of the track panel.
struct PanelColumn {
    std::vector<std::wstring> rows;
    int width = 0;   // in pixels
    int scroll = 0;  // pixels scrolled off the top
};

// Where the track panel is, in client pixels.
struct PanelGeometry {
    RECT panel;
    RECT rows[2];  // of the audio and subtitle columns; empty if not shown
    int rowH, headerH;
};

struct Window {
    HWND hwnd = nullptr;
    int showCmd = SW_SHOWNORMAL;
    // Bottom to top: black background, video (with the subtitles drawn into each frame), seek / volume feedback,
    // the icon popped up in the middle, the controls over the bottom of the video, the track panel above them, and
    // the menu bar, which the video doesn't overlap. The seek bar preview is part of the controls.
    ComPtr<ID3D11Device> device;  // for composition and the overlay
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, background, video, osd, flash, controls, panel, menuBar;
    // The preview's shadow, its frame, its rounded clip and the scaled image in it, and its time below.
    ComPtr<IDCompositionVisual> previewShadow, previewFrame, previewImage, previewTime;
    ComPtr<IDCompositionRectangleClip> previewClip;
    bool previewShown = false;
    int previewX = INT_MIN;  // the seek bar position it shows
    std::wstring previewText;
    SIZE previewTextSize{};
    SIZE previewShadowSize{};  // the frame size its shadow is drawn for
    SIZE previewPictureSize{};  // the picture size the frame is scaled for
    // Each picture decoded for the preview, by key frame in 100 ns units, until the next file: a sweep over it again
    // decodes nothing. There are at most 200 of 320 px.
    std::unordered_map<LONGLONG, ComPtr<IDCompositionSurface>> previewPictures;
    LONGLONG previewWanted = -1, previewOnScreen = -1;  // key frames
    int previewShadowPad = -1;  // how far the shadow reaches past the frame, or -1 if not drawn
    ComPtr<IDCompositionEffectGroup> controlsFade, flashFade, osdFade;
    ComPtr<IDCompositionScaleTransform> flashScale;  // about the icon's center, then
    ComPtr<IDCompositionTranslateTransform> flashMove;  // to the middle of the video
    ComPtr<IDXGISwapChain1> black;  // 8 x 8, stretched over the window
    bool attached = false;          // the engine's swap chain is the content of `video`
    SIZE videoSize{};               // swap chain size: the video fitted to the monitor
    RECT videoRect{};               // where the video is, in client pixels

    bool sized = false;  // the window has been sized to a video
    std::vector<SubtitleTrack> tracks;  // the video's own first, then files
    int subtitle = -1;                  // the track shown, or -1 for off
    int lastSubtitle = -1;              // the last one shown, which S brings back
    bool ownTrackSelected = false;      // one of the video's own is selected in the engine, collecting cues
    std::vector<std::wstring> audio;    // labels of the video's audio tracks
    int audioTrack = -1;                // the one playing
    // The track panel: an audio column (with 2+ tracks) and a subtitle column, and the row under the mouse.
    bool panelOpen = false;
    PanelColumn columns[2];
    int hotColumn = -1, hotRow = -1;
    std::wstring osdText;
    bool osdCentered = false;  // at the top center rather than the top left
    Control hot = kNone;    // under the mouse
    Control menuOpen = kNone;  // kFile or kSubs while its menu is showing
    std::wstring title;     // the video's file name
    bool controlsShown = false;
    Controls drawn{};  // what the controls show, at drawnSize and drawnScale
    SIZE drawnSize{};
    float drawnScale = 0;
    Control drag = kNone;  // kSeek or kVolumeBar while dragging along it
    bool controlsChanged = false;  // by a drag; a WM_APP_FRAME redraws them
    // The engine's state, kept here so drawing never waits on the engine. playing changes as soon as it is
    // asked to, then follows the engine's events.
    bool playing = false;
    double duration = NAN;
    double volume = 1;
    bool muted = false;
    double restoreVolume = 1;  // what unmuting from 0% goes back to: the volume before it was turned down
    int wheel = 0;             // wheel delta short of a volume step, from touchpads and free-spinning wheels
    LONG lastWheelStep = 0;    // message time of the last volume step by the wheel
    // One seek at a time is in flight; the latest one asked for meanwhile waits for it to finish.
    bool seekBusy = false, seekQueued = false, seekFast = false;
    double seekTarget = 0;  // of the latest seek
    UINT subtitleSeek = 0;  // the number of the last seek the subtitle reader was told of
    // A key frame seek by a step, from seekFrom: redone exactly if it lands on the wrong side of seekFrom.
    bool seekCheck = false;
    double seekFrom = 0;
    bool tracking = false;  // for WM_MOUSELEAVE

    bool fullscreen = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};  // of the window before it went fullscreen
    bool cursorHidden = false;  // while playing with the mouse idle
    ULONGLONG lastWake = 0;     // GetTickCount64 at the last Wake
    bool idleTimerSet = false;
    LPARAM lastMouse = -1;
    LONG lastClick = 0;  // message time of the last click on the video
} g;

std::wstring FileName(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

bool IsMenuButton(Control c) { return c == kFile || c == kSubs; }

bool HasSubtitles() { return !g.tracks.empty(); }

// Whether the track panel has anything to choose from.
bool HasTrackChoice() { return HasSubtitles() || g.audio.size() >= 2; }

// Has the player draw the cues of the subtitle track shown, or none, into the frames from now on.
void ShowSubtitles() {
    SetSubtitles(g.subtitle >= 0 ? std::make_shared<const Subtitles>(g.tracks[size_t(g.subtitle)].subs) : nullptr);
}

bool IsSubtitleFile(const std::wstring& path) {
    return path.size() >= 4 && CompareStringOrdinal(path.c_str() + path.size() - 4, 4, L".srt", 4, TRUE) == CSTR_EQUAL;
}

// The language's name, such as "English" or "English (United States)", from a tag such as "en", "en-US" or the
// ISO 639-2 "eng"; empty if it isn't one.
std::wstring LanguageName(std::wstring code) {
    if (code.size() < 2 || code.size() > 16) return {};
    wchar_t name[128];
    if (code.size() != 3) {
        const LCID lcid = LocaleNameToLCID(code.c_str(), LOCALE_ALLOW_NEUTRAL_NAMES);
        return lcid && lcid != LOCALE_CUSTOM_UNSPECIFIED &&
                       GetLocaleInfoEx(code.c_str(), LOCALE_SLOCALIZEDDISPLAYNAME, name, int(std::size(name)))
                   ? name
                   : L"";
    }
    // Windows knows the terminology codes, so map the bibliographic ones that differ.
    static const wchar_t* const kBibliographic[][2] = {
        {L"alb", L"sqi"}, {L"arm", L"hye"}, {L"baq", L"eus"}, {L"bur", L"mya"}, {L"chi", L"zho"},
        {L"cze", L"ces"}, {L"dut", L"nld"}, {L"fre", L"fra"}, {L"geo", L"kat"}, {L"ger", L"deu"},
        {L"gre", L"ell"}, {L"ice", L"isl"}, {L"mac", L"mkd"}, {L"mao", L"mri"}, {L"may", L"msa"},
        {L"per", L"fas"}, {L"rum", L"ron"}, {L"slo", L"slk"}, {L"tib", L"bod"}, {L"wel", L"cym"}};
    for (const auto& b : kBibliographic) {
        if (CompareStringOrdinal(code.c_str(), 3, b[0], 3, TRUE) == CSTR_EQUAL) code = b[1];
    }
    struct Find {
        const wchar_t* code;
        wchar_t locale[LOCALE_NAME_MAX_LENGTH];
    } find{code.c_str(), L""};
    EnumSystemLocalesEx(
        [](LPWSTR locale, DWORD, LPARAM param) -> BOOL {
            auto& f = *reinterpret_cast<Find*>(param);
            wchar_t iso[9];
            if (GetLocaleInfoEx(locale, LOCALE_SISO639LANGNAME2, iso, int(std::size(iso))) &&
                CompareStringOrdinal(iso, -1, f.code, -1, TRUE) == CSTR_EQUAL) {
                wcscpy_s(f.locale, locale);
                return FALSE;
            }
            return TRUE;
        },
        LOCALE_NEUTRALDATA, LPARAM(&find), nullptr);
    return find.locale[0] && GetLocaleInfoEx(find.locale, LOCALE_SLOCALIZEDDISPLAYNAME, name, int(std::size(name)))
               ? name
               : L"";
}

// "English", "English – Commentary", "Commentary" or "Unknown".
std::wstring TrackLabel(const Track& track) {
    std::wstring label = LanguageName(track.language);
    const std::wstring& name = track.name;
    if (!name.empty() && CompareStringOrdinal(name.c_str(), int(name.size()), label.c_str(), int(label.size()),
                                              TRUE) != CSTR_EQUAL) {
        label = label.empty() ? name : label + L" \x2013 " + name;
    }
    return label.empty() ? L"Unknown" : label;
}

// The language of "Movie.en.srt", else the file name without its extension.
std::wstring SubtitleFileLabel(const std::wstring& path) {
    std::wstring stem = FileName(path);
    stem.resize(stem.size() - 4);
    if (const size_t dot = stem.find_last_of(L'.'); dot != std::wstring::npos) {
        if (std::wstring language = LanguageName(stem.substr(dot + 1)); !language.empty()) return language;
    }
    return stem;
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

// The top of the controls' visual, in client pixels.
int ControlsTop() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    return client.bottom - std::min(ControlsHeight(), int(client.bottom));
}

// The volume slider opens right of the volume button while the mouse is on either, or dragging the slider.
bool VolumeExpanded() { return g.hot == kVolume || g.hot == kVolumeBar || g.drag == kVolumeBar; }

// Where a control is, in client pixels: the seek bar across the window, and below it a row of buttons, the volume
// slider and time right of the left ones, and the centered title.
RECT ControlRect(Control c) {
    RECT client;
    GetClientRect(g.hwnd, &client);
    if (c == kFile) return {0, 0, Px(40), MenuBarHeight()};
    if (c == kSubs) return {Px(40), 0, Px(112), MenuBarHeight()};
    const int margin = Px(12), step = Px(56), slider = Px(64), time = Px(140);
    const int seekY = client.bottom - Px(80), rowY = client.bottom - Px(34), half = Px(24);
    const int sliderX = margin + 4 * step;  // right of the volume button
    switch (c) {
        case kSeek: return {margin, seekY - Px(16), client.right - margin, seekY + Px(16)};
        case kVolumeBar: return {sliderX, rowY - half, sliderX + (VolumeExpanded() ? slider : 0), rowY + half};
        case kTime: {
            const int x = int(ControlRect(kVolumeBar).right) + Px(8);
            return {x, rowY - half, x + time, rowY + half};
        }
        case kTitle: {
            const int center = int(client.right) / 2;
            // Clear of the time with the slider open, so the title doesn't move with it.
            const int w = std::max(0, center - (sliderX + slider + Px(8) + time));
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

// The volume slider's track, inset so the knob fits at either end; empty while the slider is closed.
RECT VolumeTrack() {
    RECT r = ControlRect(kVolumeBar);
    if (r.right > r.left) InflateRect(&r, -Px(8), 0);
    return r;
}

// The controls take clicks from the top of the seek bar down, while they show.
bool IsOnControls(POINT pt) { return g.controlsShown && pt.y >= ControlRect(kSeek).top; }

bool IsOnBar(POINT pt) { return pt.y < MenuBarHeight() || IsOnControls(pt); }

Control HitTest(POINT pt) {
    for (const Control c :
         {kFile, kSubs, kPlay, kBack, kForward, kVolume, kSubtitles, kFullscreen, kSeek, kVolumeBar}) {
        const RECT r = ControlRect(c);
        if (PtInRect(&r, pt) && (IsMenuButton(c) || g.controlsShown)) return c;
    }
    return kNone;
}

void DrawMenuBar() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const BarItem file{ControlRect(kFile), L"File", 0, g.hot == kFile || g.menuOpen == kFile};
    const BarItem subs{ControlRect(kSubs), L"Subtitles", 0, g.hot == kSubs || g.menuOpen == kSubs};
    DrawBar(g.dcomp.Get(), g.menuBar.Get(), client.right, MenuBarHeight(), GetDpiForWindow(g.hwnd) / 96.0f,
            {file, subs});
}

const wchar_t* VolumeGlyph() {
    const double volume = g.volume;
    if (g.muted || volume == 0) return L"\xE74F";
    return volume < 1.0 / 3 ? L"\xE993" : volume < 2.0 / 3 ? L"\xE994" : L"\xE995";
}

// Where the video is, or will be once the seeks in flight are done.
double ShownTime() { return g.seekBusy || g.drag == kSeek ? g.seekTarget : PlaybackTime(); }

// Redraws the controls, unless they would look the same, and returns whether it did.
bool DrawControls() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    const int top = ControlsTop(), h = client.bottom - top;
    const float scale = GetDpiForWindow(g.hwnd) / 96.0f;
    const bool off = !HasVideo();
    const auto rect = [&](Control c) {
        RECT r = ControlRect(c);
        OffsetRect(&r, 0, -top);
        return r;
    };
    const auto button = [&](Control c, std::wstring text, bool dim, int replay = 0) {
        return BarItem{rect(c), std::move(text), replay, g.hot == c && !dim, dim, c == kFullscreen};
    };
    const double now = ShownTime(), duration = g.duration;
    const std::wstring step = std::to_wstring(std::lround(kSeekStep));
    // Progress to the nearest pixel of the track, so it only changes when the drawing would.
    const RECT seek = rect(kSeek);
    const double track = std::max(1L, seek.right - seek.left);
    RECT volume = VolumeTrack();
    OffsetRect(&volume, 0, -top);
    Controls controls{
        seek,
        duration > 0 ? std::round(std::clamp(now / duration, 0.0, 1.0) * track) / track : 0.0,
        !off && (g.hot == kSeek || g.drag == kSeek),
        volume,
        g.muted ? 0.0 : g.volume,
        rect(kTime),
        rect(kTitle),
        off ? L"" : FormatTime(now) + (std::isfinite(duration) ? L" / " + FormatTime(duration) : L""),
        g.title,
        {button(kPlay, g.playing ? L"\xE769" : L"\xF5B0", off), button(kBack, step, off, -1),
         button(kForward, step, off, 1), button(kVolume, VolumeGlyph(), false),
         button(kSubtitles, L"\xE7F0", off || !HasTrackChoice()),
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

// UpdateControls just before the next frame, for a drag: a fast mouse reports far more often than frames are shown,
// and each redraw draws them all.
void UpdateControlsAtFrame() {
    g.controlsChanged = true;
    RequestFrame();
}

// The panel sits right above the seek bar at the right, as tall as its longest column, up to seven and a half rows
// (the half row hints that it scrolls) or the room above. A column that scrolls runs into the bottom edge, like
// Netflix's. Clamps the columns' scroll positions to it.
PanelGeometry PanelLayout() {
    RECT client;
    GetClientRect(g.hwnd, &client);
    // Netflix's spacing, at half the size of its panel on a 1.5x screen.
    const int top = Px(6), side = Px(10), gap = Px(8), headerH = Px(44), rowH = Px(41);
    const int bottom = int(ControlRect(kSeek).top) - Px(4), right = int(client.right) - Px(12);
    const int maxH = std::clamp(bottom - MenuBarHeight() - Px(8) - top - headerH, rowH, rowH * 15 / 2);
    int listH = 0, longest = 0, width = 2 * side - gap;
    for (const PanelColumn& c : g.columns) {
        if (c.rows.empty()) continue;
        longest = std::max(longest, int(c.rows.size()) * rowH);
        listH = std::min(maxH, longest);
        width += c.width + gap;
    }
    const int pad = longest > listH ? 0 : Px(6);
    PanelGeometry p{{right - width, bottom - top - headerH - listH - pad, right, bottom}, {}, rowH, headerH};
    int x = p.panel.left + side;
    for (int k = 0; k < 2; ++k) {
        PanelColumn& c = g.columns[k];
        if (c.rows.empty()) continue;
        const int rowsTop = p.panel.top + top + headerH;
        p.rows[k] = {x, rowsTop, x + c.width, rowsTop + listH};
        x += c.width + gap;
        c.scroll = std::clamp(c.scroll, 0, std::max(0, int(c.rows.size()) * rowH - listH));
    }
    return p;
}

// The panel column under pt, or -1, and the row under it, or -1 past its last row.
int PanelColumnAt(const PanelGeometry& p, POINT pt, int& row) {
    row = -1;
    for (int k = 0; k < 2; ++k) {
        if (!PtInRect(&p.rows[k], pt)) continue;
        row = (pt.y - p.rows[k].top + g.columns[k].scroll) / p.rowH;
        if (row >= int(g.columns[k].rows.size())) row = -1;
        return k;
    }
    return -1;
}

constexpr const wchar_t* kPanelTitles[] = {L"Audio", L"Subtitles"};

// Fills the columns' rows and fits their widths to them.
void FillPanel() {
    std::vector<std::wstring>& audio = g.columns[0].rows;
    std::vector<std::wstring>& subtitles = g.columns[1].rows;
    audio = g.audio;
    subtitles = {L"Off"};
    for (const SubtitleTrack& t : g.tracks) subtitles.push_back(t.label);
    const float scale = GetDpiForWindow(g.hwnd) / 96.0f;
    for (int k = 0; k < 2; ++k) {
        PanelColumn& c = g.columns[k];
        float w = TrackTextWidth(kPanelTitles[k], scale, true);
        for (const std::wstring& row : c.rows) w = std::max(w, TrackTextWidth(row, scale, false));
        // The check mark's column, the text, and room before the scrollbar.
        c.width = std::min(int(std::ceil(w)) + Px(30 + 34), Px(360));
    }
}

void DrawPanel() {
    if (!g.panelOpen) {
        g.panel->SetContent(nullptr);
        return;
    }
    const PanelGeometry p = PanelLayout();
    const int checked[2] = {g.audioTrack, g.subtitle + 1};
    const auto local = [&](RECT r) {
        OffsetRect(&r, -p.panel.left, -p.panel.top);
        return r;
    };
    std::vector<TrackColumn> columns;
    for (int k = 0; k < 2; ++k) {
        const PanelColumn& c = g.columns[k];
        const RECT rows = p.rows[k];
        if (c.rows.empty()) continue;
        TrackColumn column{local({rows.left, rows.top - p.headerH, rows.right, rows.top}), kPanelTitles[k],
                           local(rows), {}};
        const int listH = rows.bottom - rows.top, totalH = int(c.rows.size()) * p.rowH;
        column.more = totalH - c.scroll > listH;
        if (totalH > listH) {  // a scrollbar thumb along the right edge
            const int trackTop = rows.top + Px(5), track = listH - Px(10);
            const int y = trackTop + MulDiv(track, c.scroll, totalH);
            column.thumb = local({rows.right - Px(1), y, rows.right, y + MulDiv(track, listH, totalH)});
        }
        for (int i = c.scroll / p.rowH; i < int(c.rows.size()); ++i) {
            const int y = rows.top + i * p.rowH - c.scroll;
            if (y >= rows.bottom) break;
            BarItem item{local({rows.left, y, rows.right, y + p.rowH}), c.rows[size_t(i)]};
            item.hot = k == g.hotColumn && i == g.hotRow;
            item.checked = i == checked[k];
            column.items.push_back(std::move(item));
        }
        columns.push_back(std::move(column));
    }
    DrawTrackPanel(g.dcomp.Get(), g.panel.Get(), UINT(p.panel.right - p.panel.left),
                   UINT(p.panel.bottom - p.panel.top), GetDpiForWindow(g.hwnd) / 96.0f, columns);
    g.panel->SetOffsetX(float(p.panel.left));
    g.panel->SetOffsetY(float(p.panel.top));
}

// Redraws the panel, if open, for changed tracks.
void RefreshPanel() {
    if (!g.panelOpen) return;
    FillPanel();
    DrawPanel();
    g.dcomp->Commit();
}

void SetPanelOpen(bool open) {
    if (open == g.panelOpen) return;
    g.panelOpen = open;
    g.hotColumn = g.hotRow = -1;
    g.columns[0].scroll = g.columns[1].scroll = 0;
    if (open) FillPanel();
    DrawPanel();
    g.dcomp->Commit();
}

// Highlights the panel row under pt.
void SetPanelHot(POINT pt) {
    int row = -1;
    int column = g.panelOpen ? PanelColumnAt(PanelLayout(), pt, row) : -1;
    if (column < 0 || row < 0) column = row = -1;
    if (column == g.hotColumn && row == g.hotRow) return;
    g.hotColumn = column;
    g.hotRow = row;
    DrawPanel();
    g.dcomp->Commit();
}

void SetHot(Control c) {
    if (c == g.hot) return;
    const bool menu = IsMenuButton(c) || IsMenuButton(g.hot);
    g.hot = c;
    if (menu) DrawMenuBar();
    if (g.controlsShown) DrawControls();
    g.dcomp->Commit();
}

// Tells the player where the subtitles go: near the video's top and bottom edges, the bottom ones above the
// controls while they show.
void PlaceSubtitles() {
    const RECT& v = g.videoRect;
    const float w = float(v.right - v.left), h = float(v.bottom - v.top), margin = h * 0.06f;
    SubtitleLayout layout{w, std::max(12.0f, TextScale() * 0.05f), w * 0.9f, margin, h - margin};
    if (g.controlsShown) layout.bottom = std::min(layout.bottom, float(ControlRect(kSeek).top - v.top));
    SetSubtitleLayout(layout);
}

// Fades the controls in or out.
void SetControlsShown(bool shown) {
    if (shown == g.controlsShown) return;
    g.controlsShown = shown;
    if (shown) {
        DrawControls();
    } else if (!IsMenuButton(g.hot)) {
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
    PlaceSubtitles();
    g.dcomp->Commit();
}

void SetCursorHidden(bool hidden) {
    if (g.cursorHidden == hidden) return;
    g.cursorHidden = hidden;
    POINT pt;
    GetCursorPos(&pt);
    SetCursorPos(pt.x, pt.y);  // makes Windows send WM_SETCURSOR now
}

// Starts the idle timer over, to fire in ms.
void SetIdleTimer(UINT ms) {
    g.idleTimerSet = SetTimer(g.hwnd, kIdleTimer, ms, nullptr) != 0;
}

// Shows the controls and the cursor until the mouse has been idle for a while. A fast mouse wakes them often, so
// this only notes when; the idle timer, once set, checks it.
void Wake() {
    SetCursorHidden(false);
    SetControlsShown(true);
    g.lastWake = GetTickCount64();
    if (!g.idleTimerSet) SetIdleTimer(kIdleMs);
}

// Hides the controls and the cursor while playing, unless the mouse is on the controls or the menu bar.
void OnIdle() {
    KillTimer(g.hwnd, kIdleTimer);
    g.idleTimerSet = false;
    if (const ULONGLONG idle = GetTickCount64() - g.lastWake; idle < kIdleMs) {
        SetIdleTimer(UINT(kIdleMs - idle));
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g.hwnd, &pt);
    if (!g.playing || g.drag || g.panelOpen || IsOnBar(pt)) return;
    SetControlsShown(false);
    SetCursorHidden(true);
}

void DrawOsd() {
    const float fontSize =
        std::max(12.0f, (HasVideo() ? TextScale() : float(g.videoRect.bottom - g.videoRect.top)) * 0.035f);
    const float margin = std::round(fontSize * 0.8f);
    const RECT& v = g.videoRect;
    const SIZE s =
        DrawOutlinedText(g.dcomp.Get(), g.osd.Get(), g.osdText, {}, fontSize, float(g.videoRect.right), false);
    g.osd->SetOffsetX(g.osdCentered ? std::round((v.left + v.right - s.cx) / 2.0f) : margin);
    g.osd->SetOffsetY(float(MenuBarHeight()) + margin);
}

// Shows text in the top left for a moment.
void ShowOsd(std::wstring text) {
    g.osdText = std::move(text);
    g.osdCentered = false;
    g.osdFade->SetOpacity(1.0f);
    DrawOsd();
    g.dcomp->Commit();
    SetTimer(g.hwnd, kOsdTimer, kOsdMs, nullptr);
}

void HideOsd() {
    KillTimer(g.hwnd, kOsdTimer);
    if (g.osdText.empty()) return;
    g.osdText.clear();
    g.osd->SetContent(nullptr);
    g.dcomp->Commit();
}

// Pops an icon up in the middle of the video for a moment, like YouTube's and Netflix's: it grows as it fades in,
// then shrinks a little as it fades out. held (a key held down) shows it at once, so repeats don't pulse it. A
// caption shows at the top center, fading with the icon.
void ShowFlash(const wchar_t* glyph, bool slash = false, bool held = false, std::wstring caption = {}) {
    const RECT& v = g.videoRect;
    const float in = held ? 0 : kFlashIn, out = kFlashOut - kFlashIn + in, end = kFlashEnd - kFlashIn + in;
    if (!caption.empty()) {
        g.osdText = std::move(caption);
        g.osdCentered = true;
        DrawOsd();
        SetTimer(g.hwnd, kOsdTimer, UINT(end * 1000), nullptr);  // clears it once faded out
    } else if (g.osdCentered) {
        HideOsd();  // another one's caption
    }
    const float size = std::round(std::max(float(Px(56)), TextScale() * 0.1f));
    DrawFlash(g.dcomp.Get(), g.flash.Get(), UINT(size), glyph, slash);
    g.flashScale->SetCenterX(size / 2);
    g.flashScale->SetCenterY(size / 2);
    g.flashMove->SetOffsetX(std::round((v.left + v.right - size) / 2));
    g.flashMove->SetOffsetY(std::round((v.top + v.bottom - size) / 2));
    ComPtr<IDCompositionAnimation> fade, zoom;
    if (SUCCEEDED(g.dcomp->CreateAnimation(&fade)) && SUCCEEDED(g.dcomp->CreateAnimation(&zoom))) {
        constexpr float from = 0.7f, to = 0.85f;
        if (!held) {
            fade->AddCubic(0, 0, 1 / in, 0, 0);
            // Easing out as it grows.
            zoom->AddCubic(0, from, 2 * (1 - from) / in, -(1 - from) / (in * in), 0);
        }
        fade->AddCubic(in, 1, 0, 0, 0);
        fade->AddCubic(out, 1, -1 / (end - out), 0, 0);
        fade->End(end, 0);
        zoom->AddCubic(in, 1, 0, 0, 0);
        zoom->AddCubic(out, 1, -(1 - to) / (end - out), 0, 0);
        zoom->End(end, to);
        g.flashFade->SetOpacity(fade.Get());
        if (g.osdCentered) g.osdFade->SetOpacity(fade.Get());
        g.flashScale->SetScaleX(zoom.Get());
        g.flashScale->SetScaleY(zoom.Get());
    } else {
        g.flashFade->SetOpacity(0.0f);
        g.osdFade->SetOpacity(1.0f);
    }
    g.dcomp->Commit();
}

// v scaled to fit w x h, keeping its aspect ratio.
SIZE Fit(SIZE v, int w, int h) {
    const double scale = std::min(double(w) / v.cx, double(h) / v.cy);
    return {std::max(1L, LONG(std::lround(v.cx * scale))), std::max(1L, LONG(std::lround(v.cy * scale)))};
}

// Shows the picture of a key frame if it is kept, else has it decoded; WM_APP_PREVIEW brings it.
void WantPreviewPicture(LONGLONG time) {
    if (time == g.previewWanted) return;
    g.previewWanted = time;
    if (const auto it = g.previewPictures.find(time); it != g.previewPictures.end()) {
        g.previewImage->SetContent(it->second.Get());
        g.previewOnScreen = time;
    } else {
        DecodePreview(time);
    }
}

// Copies a picture the previewer shared into a surface of its own on the window's device; null if it can't.
ComPtr<IDCompositionSurface> CopyPreviewPicture(HANDLE handle) {
    ComPtr<ID3D11Texture2D> shared, target;
    ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(g.device->OpenSharedResource(handle, IID_PPV_ARGS(&shared))) || FAILED(shared.As(&mutex)) ||
        mutex->AcquireSync(1, 1000) != S_OK) {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC desc;
    shared->GetDesc(&desc);
    ComPtr<IDCompositionSurface> surface;
    POINT at;
    bool copied = SUCCEEDED(g.dcomp->CreateSurface(desc.Width, desc.Height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                   DXGI_ALPHA_MODE_IGNORE, &surface)) &&
                  SUCCEEDED(surface->BeginDraw(nullptr, IID_PPV_ARGS(&target), &at));
    if (copied) {
        ComPtr<ID3D11DeviceContext> context;
        g.device->GetImmediateContext(&context);
        context->CopySubresourceRegion(target.Get(), 0, at.x, at.y, 0, shared.Get(), 0, nullptr);
        target.Reset();
        copied = SUCCEEDED(surface->EndDraw());
    }
    mutex->ReleaseSync(0);
    return copied ? surface : nullptr;
}

// Keeps a decoded picture, and shows it unless the wanted one already shows: one the mouse moved on from while it
// decoded is still the nearest there is, so a fast sweep updates as often as a slow one.
void OnPreviewPicture() {
    LONGLONG time;
    const HANDLE handle = TakePreviewPicture(time);
    if (!handle) return;
    const ComPtr<IDCompositionSurface> surface = CopyPreviewPicture(handle);
    if (!surface) return;
    g.previewPictures.emplace(time, surface);
    if (g.previewOnScreen == g.previewWanted) return;
    g.previewImage->SetContent(surface.Get());
    g.previewOnScreen = time;
    if (g.previewShown) g.dcomp->Commit();
}

// While the mouse is on the seek bar, or dragging it, shows the frame and the time at pt's x above it, like
// YouTube's; else hides them. It moves with each mouse move: placing it only sets a few properties, and composition
// shows the latest of them once per frame.
void UpdatePreview(POINT pt) {
    const bool show = g.controlsShown && g.duration > 0 && (g.drag == kSeek || (!g.drag && g.hot == kSeek));
    if (!show) {
        if (g.previewShown) {
            g.controls->RemoveVisual(g.previewShadow.Get());
            g.controls->RemoveVisual(g.previewFrame.Get());
            g.controls->RemoveVisual(g.previewTime.Get());
            g.previewShown = false;
            g.dcomp->Commit();
        }
        return;
    }
    RECT seek = ControlRect(kSeek);
    OffsetRect(&seek, 0, -ControlsTop());
    const int left = seek.left, right = seek.right;
    const int x = std::clamp(int(pt.x), left, right);
    // Centered on x, but kept over the bar.
    const auto place = [&](int width) { return float(std::clamp(x - width / 2, left, std::max(left, right - width))); };
    const SIZE picture = PreviewSize();
    // A move along y only leaves it where it is.
    if (g.previewShown && x == g.previewX && picture.cx == g.previewPictureSize.cx &&
        picture.cy == g.previewPictureSize.cy) {
        return;
    }
    if (x != g.previewX) {
        g.previewX = x;
        const double seconds = double(x - left) / std::max(1, right - left) * g.duration;
        WantPreviewPicture(PreviewKeyFrame(seconds, g.duration));
        if (std::wstring text = FormatTime(seconds); text != g.previewText) {
            g.previewText = std::move(text);
            g.previewTextSize =
                DrawTimeLabel(g.dcomp.Get(), g.previewTime.Get(), GetDpiForWindow(g.hwnd) / 96.0f, g.previewText);
        }
    }
    // The time just above the track, and the frame above it.
    const SIZE t = g.previewTextSize;
    const int timeTop = int(seek.top + seek.bottom) / 2 - Px(14) - int(t.cy);
    g.previewTime->SetOffsetX(place(t.cx));
    g.previewTime->SetOffsetY(float(timeTop));
    if (picture.cx > 0 && picture.cy > 0) {
        const int side = std::clamp(int(TextScale() * 0.35f), Px(120), Px(320));
        const SIZE s = Fit(picture, side, side);
        // Its look only changes with its size, so a move just places it.
        if (s.cx != g.previewShadowSize.cx || s.cy != g.previewShadowSize.cy ||
            picture.cx != g.previewPictureSize.cx || picture.cy != g.previewPictureSize.cy) {
            g.previewPictureSize = picture;
            g.previewImage->SetTransform(
                D2D_MATRIX_3X2_F{float(s.cx) / picture.cx, 0, 0, float(s.cy) / picture.cy, 0, 0});
            // Rounded like the menus, over a soft shadow.
            const float r = float(Px(8));
            if (s.cx != g.previewShadowSize.cx || s.cy != g.previewShadowSize.cy) {
                g.previewShadowSize = s;
                g.previewShadowPad = DrawPreviewShadow(g.dcomp.Get(), g.previewShadow.Get(),
                                                       GetDpiForWindow(g.hwnd) / 96.0f, s, r);
            }
            IDCompositionRectangleClip* clip = g.previewClip.Get();
            clip->SetRight(float(s.cx));
            clip->SetBottom(float(s.cy));
            clip->SetTopLeftRadiusX(r);
            clip->SetTopLeftRadiusY(r);
            clip->SetTopRightRadiusX(r);
            clip->SetTopRightRadiusY(r);
            clip->SetBottomLeftRadiusX(r);
            clip->SetBottomLeftRadiusY(r);
            clip->SetBottomRightRadiusX(r);
            clip->SetBottomRightRadiusY(r);
        }
        const float frameX = place(s.cx), frameY = float(timeTop - Px(6) - int(s.cy));
        g.previewFrame->SetOffsetX(frameX);
        g.previewFrame->SetOffsetY(frameY);
        g.previewShadow->SetOffsetX(frameX - g.previewShadowPad);
        g.previewShadow->SetOffsetY(frameY - g.previewShadowPad);
    } else {  // audio only, or not loaded yet
        g.previewImage->SetContent(nullptr);
        g.previewShadow->SetContent(nullptr);
        g.previewShadowSize = {};
        g.previewPictureSize = picture;
    }
    if (!g.previewShown) {
        g.controls->AddVisual(g.previewShadow.Get(), TRUE, nullptr);
        g.controls->AddVisual(g.previewFrame.Get(), TRUE, g.previewShadow.Get());
        g.controls->AddVisual(g.previewTime.Get(), TRUE, nullptr);
        g.previewShown = true;
    }
    g.dcomp->Commit();
}

// Just before the frame RequestFrame asked for.
void OnFrame() {
    if (std::exchange(g.controlsChanged, false)) UpdateControls();
}

// The video's swap chain size: the video v fitted to the window's monitor.
SIZE ChainSize(SIZE v) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return Fit(v, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
}

// Fits the video below the menu bar, keeping its aspect ratio. Its swap chain is the size it fills the monitor at,
// which only changes with the video or the monitor, and composition scales that to the window, so resizing and
// fullscreen take effect in the same frame as the window, with no wait on the engine. The engine's next frame is
// then drawn at the size on screen, which composition shows pixel for pixel.
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
        const SIZE chain = ChainSize(v);
        if (chain.cx != g.videoSize.cx || chain.cy != g.videoSize.cy || !g.attached) {
            SetVideoSize(chain.cx, chain.cy);
            g.videoSize = chain;
        }
        const SIZE s = Fit(v, cw, ah);
        const int x = (cw - s.cx) / 2, y = top + (ah - s.cy) / 2;
        g.videoRect = {x, y, x + s.cx, y + s.cy};
        SetVideoTarget(s.cx, s.cy);
        g.video->SetTransform(D2D_MATRIX_3X2_F{float(s.cx) / chain.cx, 0, 0, float(s.cy) / chain.cy, float(x),
                                               float(y)});
        // The swap chain exists once it has a size.
        if (!g.attached) {
            if (IUnknown* swapChain = VideoSwapChain()) g.attached = SUCCEEDED(g.video->SetContent(swapChain));
        }
    } else {
        g.videoRect = {0, top, cw, top + ah};
    }
    if (g.panelOpen) {
        FillPanel();  // for a new DPI
        DrawPanel();
    }
    g.previewText.clear();  // redrawn on the next move, for a new DPI or size
    g.previewShadowSize = {};
    g.previewX = INT_MIN;
    PlaceSubtitles();
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

// Shows the subtitle track at index, or none (-1). Picking one of the video's own makes the engine send its cues.
void SelectSubtitle(int index) {
    if (index >= 0) {
        SubtitleTrack& track = g.tracks[size_t(index)];
        if (track.path.empty() && !track.read) SelectSubtitleTrack(track.id);
        g.lastSubtitle = index;
    }
    g.subtitle = index;
    DrawPanel();
    ShowSubtitles();
}

// Adds an SRT file as a subtitle track, unless it is already one, and returns its index, or -1 if it can't be read.
int AddSubtitleFile(const std::wstring& path) {
    for (size_t i = 0; i < g.tracks.size(); ++i) {
        const std::wstring& p = g.tracks[i].path;
        if (CompareStringOrdinal(p.c_str(), int(p.size()), path.c_str(), int(path.size()), TRUE) == CSTR_EQUAL) {
            return int(i);
        }
    }
    SubtitleTrack track{SubtitleFileLabel(path), path};
    if (!LoadSubtitles(path, track.subs)) return -1;
    g.tracks.push_back(std::move(track));
    UpdateControls();
    RefreshPanel();
    return int(g.tracks.size() - 1);
}

void LoadSubtitleFile(const std::wstring& path) {
    const int index = AddSubtitleFile(path);
    if (index < 0) {
        ShowError(L"Can't open the subtitles \"" + FileName(path) + L"\".\n\nIt isn't a readable SRT file.");
        return;
    }
    SelectSubtitle(index);
    if (IsWindowVisible(g.hwnd)) ShowOsd(L"Subtitles: " + g.tracks[size_t(index)].label);
}

// Adds the .srt files next to the video whose names start with its own, such as "Movie.en.srt", without showing
// them.
void AddSidecarSubtitles(const std::wstring& video) {
    const size_t slash = video.find_last_of(L"\\/"), dot = video.find_last_of(L'.');
    const size_t end = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? dot : video.size();
    WIN32_FIND_DATAW found;
    const HANDLE find = FindFirstFileExW((video.substr(0, end) + L"*.srt").c_str(), FindExInfoBasic, &found,
                                        FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    const std::wstring dir = video.substr(0, slash + 1);  // empty if there is no slash
    int count = 0;
    do {
        // The pattern can also match longer extensions through 8.3 names.
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsSubtitleFile(found.cFileName)) {
            AddSubtitleFile(dir + found.cFileName);
        }
    } while (++count < kMaxSidecarSubtitles && FindNextFileW(find, &found));
    FindClose(find);
}

bool OpenVideoFile(const std::wstring& path) {
    const HRESULT hr = OpenVideo(path);
    if (FAILED(hr)) {
        const bool missing = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                             hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        ShowError(L"Can't open \"" + FileName(path) + L"\"." + (missing ? L"\n\nThe file doesn't exist." : L""));
        return false;
    }
    // Seeks and tracks belong to the video they were for.
    g.duration = NAN;
    g.seekBusy = g.seekQueued = g.seekCheck = false;
    SetPanelOpen(false);
    g.tracks.clear();
    g.subtitle = g.lastSubtitle = g.audioTrack = -1;
    g.ownTrackSelected = false;
    g.audio.clear();
    // Stop reading the previous video's subtitles and drop any of its cues still queued.
    StopMkvSubtitles();
    HoldFrames(false);
    for (MSG msg; PeekMessageW(&msg, g.hwnd, WM_APP_SUBTITLE_CUE, WM_APP_SUBTITLE_CUE, PM_REMOVE);) {
        delete reinterpret_cast<Cue*>(msg.lParam);
    }
    for (MSG msg; PeekMessageW(&msg, g.hwnd, WM_APP_SUBTITLE_CUES, WM_APP_SUBTITLE_CUES, PM_REMOVE);) {
        delete reinterpret_cast<std::vector<Cue>*>(msg.lParam);
    }
    for (const Track& t : StartMkvSubtitles(path, g.hwnd)) g.tracks.push_back({TrackLabel(t), L"", t.id, {}, true});
    AddSidecarSubtitles(path);
    ShowSubtitles();
    HideOsd();
    if (HasSubtitles() && g.tracks[0].read) ShowOsd(L"Subtitles available (S)");
    SetWindowTextW(g.hwnd, FileName(path).c_str());
    g.title = FileName(path);
    g.title = g.title.substr(0, g.title.find_last_of(L'.'));
    UpdateControls();
    g.previewPictures.clear();
    g.previewWanted = g.previewOnScreen = -1;
    g.previewImage->SetContent(nullptr);
    g.previewX = INT_MIN;
    UpdatePreview({});  // hides it until the duration is known
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

void AddSubtitles() {
    const std::wstring path = RunFileDialog(g.hwnd, true);
    if (!path.empty()) LoadSubtitleFile(path);
}

// The video has subtitle tracks of its own, listed before the files. They start off, as they are often in another
// language or for the hard of hearing, but the first one is selected in the engine so S can show its cues.
void OnSubtitleTrack() {
    if (HasSubtitles() && g.tracks[0].read) return;  // the player reads them from the file itself
    size_t own = size_t(std::count_if(g.tracks.begin(), g.tracks.end(),
                                      [](const SubtitleTrack& t) { return t.path.empty(); }));
    const size_t before = own;
    for (const Track& t : SubtitleTracks()) {
        if (std::any_of(g.tracks.begin(), g.tracks.begin() + ptrdiff_t(own),
                        [&](const SubtitleTrack& s) { return s.id == t.id; })) {
            continue;
        }
        g.tracks.insert(g.tracks.begin() + ptrdiff_t(own), SubtitleTrack{TrackLabel(t), L"", t.id});
        if (g.subtitle >= int(own)) ++g.subtitle;
        if (g.lastSubtitle >= int(own)) ++g.lastSubtitle;
        ++own;
    }
    if (own == before) return;
    if (!g.ownTrackSelected) {
        g.ownTrackSelected = SelectSubtitleTrack(g.tracks[0].id);
        if (g.subtitle < 0) ShowOsd(L"Subtitles available (S)");
    }
    UpdateControls();
    RefreshPanel();
}

void OnSubtitleCue(WPARAM track, LPARAM cue) {
    const std::unique_ptr<Cue> owned(reinterpret_cast<Cue*>(cue));
    const auto it = std::find_if(g.tracks.begin(), g.tracks.end(),
                                 [&](const SubtitleTrack& t) { return t.path.empty() && !t.read && t.id == DWORD(track); });
    if (it != g.tracks.end() && AddCues(it->subs, {std::move(*owned)}) && it - g.tracks.begin() == g.subtitle) {
        ShowSubtitles();
    }
}

void OnSubtitleCues(WPARAM track, LPARAM cues) {
    const std::unique_ptr<std::vector<Cue>> owned(reinterpret_cast<std::vector<Cue>*>(cues));
    const auto it = std::find_if(g.tracks.begin(), g.tracks.end(),
                                 [&](const SubtitleTrack& t) { return t.read && t.id == DWORD(track); });
    if (it == g.tracks.end()) return;
    if (AddCues(it->subs, std::move(*owned)) && it - g.tracks.begin() == g.subtitle) ShowSubtitles();
}

// Plays the audio track or shows the subtitle track (0 for off) in the panel's column and row.
void PickTrack(int column, int row) {
    if (column == 0 && row != g.audioTrack) {
        SelectAudioTrack(size_t(row));
        g.audioTrack = row;
        DrawPanel();
        g.dcomp->Commit();
    } else if (column == 1 && row - 1 != g.subtitle) {
        SelectSubtitle(row - 1);
    }
}

// Shows the menu of a menu bar button, kFile or kSubs.
void ShowMenu(Control which) {
    if (g.fullscreen) return;
    const MenuItem file[] = {{kCmdOpen, L"Open...", 'O', L"Ctrl+O"}};
    // Opening a video clears its subtitles, so they are added to the one playing.
    const MenuItem subs[] = {{kCmdAddSubtitles, L"Add subtitle file...", 'A', L"", HasVideo()}};
    RECT anchor = ControlRect(which);
    MapWindowPoints(g.hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    g.menuOpen = which;
    DrawMenuBar();
    g.dcomp->Commit();
    const UINT cmd = which == kFile ? ShowPopupMenu(g.hwnd, anchor, file, int(std::size(file)))
                                    : ShowPopupMenu(g.hwnd, anchor, subs, int(std::size(subs)));
    // The menu had the mouse while it showed.
    g.menuOpen = kNone;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g.hwnd, &pt);
    g.hot = HitTest(pt);
    DrawMenuBar();
    if (g.controlsShown) DrawControls();
    g.dcomp->Commit();
    switch (cmd) {
        case kCmdOpen: Open(); break;
        case kCmdAddSubtitles: AddSubtitles(); break;
    }
}

// Starts the engine's seek, and has the subtitle reader go on from where it lands. While the track shown is one the
// reader reads, frames wait for the cues there, so the first one never shows without them.
void StartSeek(double seconds, bool fast) {
    const double landing = SeekTo(seconds, fast);
    const bool read = g.subtitle >= 0 && g.tracks[size_t(g.subtitle)].read;
    HoldFrames(SeekMkvSubtitles(landing, ++g.subtitleSeek) && read);
}

// Seeks to a time within the video and returns it; the caller shows it. While a seek is in flight the new one waits, replacing any
// that was already waiting, so dragging or holding an arrow key never piles up seeks in the engine.
double RequestSeek(double seconds, bool fast) {
    g.seekTarget = std::max(0.0, std::isfinite(g.duration) ? std::min(seconds, g.duration) : seconds);
    g.seekFast = fast;
    if (g.seekBusy) {
        g.seekQueued = true;
    } else {
        g.seekBusy = true;
        StartSeek(g.seekTarget, fast);
    }
    return g.seekTarget;
}

// A seek finished: start the waiting one, if any, or redo a step that went the wrong way, which a long gap
// between key frames can cause.
void OnSeeked() {
    if (g.seekQueued) {
        g.seekQueued = false;
        StartSeek(g.seekTarget, g.seekFast);
        return;
    }
    if (g.seekCheck) {
        g.seekCheck = false;
        const double landed = PlaybackTime();
        if (g.seekTarget > g.seekFrom ? landed <= g.seekFrom : landed >= g.seekFrom) {
            StartSeek(g.seekTarget, false);
            return;
        }
    }
    g.seekBusy = false;
}

// Steps by key frame, which shows at once; an exact seek would first decode up to the target.
void Seek(double seconds) {
    g.seekFrom = ShownTime();
    g.seekCheck = true;
    const double target = RequestSeek(g.seekFrom + seconds, true);
    if (g.controlsShown) DrawControls();  // committed with the OSD
    ShowOsd(FormatTime(target) + L" / " + FormatTime(g.duration));
}

// Plays or pauses, showing it at once rather than when the engine reports it.
void TogglePlay() {
    g.playing = !g.playing;
    SetPlaying(g.playing);
    UpdateControls();
    ShowFlash(g.playing ? L"\xF5B0" : L"\xE769");
}

// Mutes or unmutes, popping up the new volume icon if flash. At 0% it already counts as muted, so it unmutes to
// the volume before that, at least a little.
void ToggleMuted(bool flash) {
    if (g.volume == 0) {
        g.volume = SetVolume(std::max(g.restoreVolume, kVolumeStep));
        g.muted = false;
    } else {
        g.muted = ToggleMute();
    }
    UpdateControls();
    if (flash) ShowFlash(VolumeGlyph());
}

void ToggleSubtitles() {
    if (!HasSubtitles()) return;
    SelectSubtitle(g.subtitle >= 0 ? -1 : std::max(g.lastSubtitle, 0));
    ShowFlash(L"\xE7F0", g.subtitle < 0);
}

// Seeks to the time under x on the seek bar.
void SeekToX(int x, bool fast) {
    const RECT r = ControlRect(kSeek);
    const double duration = g.duration;
    if (!(duration > 0)) return;
    g.seekCheck = false;
    RequestSeek(std::clamp(double(x - r.left) / std::max(1L, r.right - r.left), 0.0, 1.0) * duration, fast);
    UpdateControlsAtFrame();
}

// Sets the volume under x on the volume slider.
void SetVolumeToX(int x) {
    const RECT r = VolumeTrack();
    g.volume = SetVolume(std::clamp(double(x - r.left) / std::max(1L, r.right - r.left), 0.0, 1.0));
    g.muted = g.muted && g.volume == 0;
    UpdateControlsAtFrame();
}

// Switches between the normal window and a borderless one covering its monitor.
void ToggleFullscreen() {
    g.fullscreen = !g.fullscreen;
    g.hot = kNone;
    g.drag = kNone;
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
    UpdatePreview({});
    Wake();
}

void Press(Control c) {
    if (IsMenuButton(c)) {
        ShowMenu(c);
    } else if (c == kVolume) {
        ToggleMuted(false);  // the button and slider already show it
    } else if (c == kFullscreen) {
        ToggleFullscreen();
    } else if (c != kNone && c != kSeek && HasVideo()) {
        if (c == kPlay) {
            TogglePlay();
        } else if (c == kSubtitles) {
            if (HasTrackChoice()) SetPanelOpen(!g.panelOpen);
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
            g.audio.clear();
            g.audioTrack = -1;
            for (const Track& t : AudioTracks()) {
                if (t.selected && g.audioTrack < 0) g.audioTrack = int(g.audio.size());
                g.audio.push_back(TrackLabel(t));
            }
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
            UpdateControls();
            SetIdleTimer(kIdleMs);
            break;
        case MF_MEDIA_ENGINE_EVENT_PAUSE:
        case MF_MEDIA_ENGINE_EVENT_ENDED:
            g.playing = IsPlaying();
            SetThreadExecutionState(ES_CONTINUOUS);
            Wake();
            UpdateControls();
            break;
        case MF_MEDIA_ENGINE_EVENT_SEEKED:
            OnSeeked();
            [[fallthrough]];
        case MF_MEDIA_ENGINE_EVENT_TIMEUPDATE:
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
            g.playing = g.seekBusy = g.seekQueued = g.seekCheck = false;
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

// Turns the volume up or down a step, showing the new level; held keeps the icon up rather than popping it again.
void StepVolume(bool up, bool held) {
    if (g.volume > 0) g.restoreVolume = g.volume;
    g.volume = ChangeVolume(up ? kVolumeStep : -kVolumeStep);
    g.muted = IsMuted();  // turning it up unmutes
    UpdateControls();
    ShowFlash(VolumeGlyph(), false, held, std::to_wstring(std::lround(g.volume * 100)) + L"%");
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
        case VK_DOWN:
            StepVolume(key == VK_UP, repeat);
            break;
        case 'M':
            if (!repeat) ToggleMuted(true);
            break;
        case 'S':
            if (!repeat) ToggleSubtitles();
            break;
        case VK_F11:
            if (!repeat) ToggleFullscreen();
            break;
        case VK_ESCAPE:
            if (repeat) break;
            if (g.panelOpen) {
                SetPanelOpen(false);
            } else if (g.fullscreen) {
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
    if (LRESULT result; HandlePopupMenuMessage(msg, wp, lp, result)) return result;
    switch (msg) {
        case WM_APP_MEDIA:
            OnMediaEvent(wp, lp);
            return 0;
        case WM_APP_SUBTITLE_TRACK:
            OnSubtitleTrack();
            return 0;
        case WM_APP_SUBTITLE_CUE:
            OnSubtitleCue(wp, lp);
            return 0;
        case WM_APP_SUBTITLE_CUES:
            OnSubtitleCues(wp, lp);
            return 0;
        case WM_APP_SUBTITLES_READY:
            if (UINT(wp) == g.subtitleSeek) HoldFrames(false);  // its cues came before it
            return 0;
        case WM_APP_FRAME:
            OnFrame();
            return 0;
        case WM_APP_PREVIEW:
            OnPreviewPicture();
            return 0;
        case WM_PAINT:  // everything is drawn by DirectComposition
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {  // the menu bar, but for its buttons, drags the window like the title bar
            const LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            return hit == HTCLIENT && pt.y < MenuBarHeight() && !IsMenuButton(HitTest(pt)) ? HTCAPTION : hit;
        }
        case WM_SYSCOMMAND:  // no system menu: Alt, Alt+Space, F10 and clicking the title bar icon
            if ((wp & 0xFFF0) == SC_MOUSEMENU || (wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_CONTEXTMENU:  // no system menu on right-clicking the title bar or the menu bar
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && g.dcomp) Layout();
            return 0;
        case WM_EXITSIZEMOVE:  // it may be on another monitor now, which the video's swap chain is sized for
        case WM_DISPLAYCHANGE:
            // A new size or DPI already laid it out through WM_SIZE.
            if (!IsIconic(hwnd) && g.dcomp) {
                const SIZE v = VideoSize(), chain = v.cx > 0 && v.cy > 0 ? ChainSize(v) : g.videoSize;
                if (chain.cx != g.videoSize.cx || chain.cy != g.videoSize.cy) Layout();
            }
            return 0;
        case WM_TIMER:
            switch (wp) {
                case kOsdTimer: HideOsd(); break;
                case kShowTimer: ShowMainWindow(); break;
                case kIdleTimer: OnIdle(); break;
            }
            return 0;
        case WM_KEYDOWN:
            OnKey(wp, (lp & (1 << 30)) != 0);
            return 0;
        case WM_SYSCHAR:  // Alt+F, Alt+S
            if (wp == 'f' || wp == 'F' || wp == 's' || wp == 'S') {
                ShowMenu((wp | 0x20) == 'f' ? kFile : kSubs);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const Control c = HitTest(pt);
            if (g.panelOpen) {  // a click off the panel closes it, and does nothing else on the video
                const PanelGeometry p = PanelLayout();
                int row;
                if (const int column = PanelColumnAt(p, pt, row); column >= 0 && row >= 0) PickTrack(column, row);
                if (PtInRect(&p.panel, pt)) return 0;
                if (c != kSubtitles) {
                    SetPanelOpen(false);
                    if (!IsOnBar(pt)) return 0;
                }
            }
            if (c == kSeek && HasVideo()) {
                g.drag = kSeek;
                SetCapture(hwnd);
                SeekToX(pt.x, true);
            } else if (c == kVolumeBar) {
                g.drag = kVolumeBar;
                SetCapture(hwnd);
                if (g.volume > 0) g.restoreVolume = g.volume;
                SetVolumeToX(pt.x);
            } else if (IsOnBar(pt)) {  // a double click there is just a second click
                Press(c);
            } else if (msg == WM_LBUTTONDBLCLK &&
                       static_cast<DWORD>(GetMessageTime() - g.lastClick) <= kDoubleClickMs) {
                if (HasVideo()) TogglePlay();  // the first click toggled playback; undo that
                g.flashFade->SetOpacity(0.0f);  // and don't show it
                ToggleFullscreen();
            } else {  // also a double click slower than ours but within the system's
                g.lastClick = GetMessageTime();
                if (HasVideo()) TogglePlay();
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g.drag) {
                const Control drag = g.drag;
                g.drag = kNone;
                if (drag == kSeek) SeekToX(GET_X_LPARAM(lp), false);  // lands on the exact frame
                ReleaseCapture();
                SetHot(HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));  // closes the volume slider if left
                UpdatePreview({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            }
            return 0;
        case WM_CAPTURECHANGED:
            g.drag = kNone;
            return 0;
        case WM_MOUSEMOVE:
            if (lp != g.lastMouse) Wake();  // not the move SetCursorPos fakes
            g.lastMouse = lp;
            if (!g.tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                g.tracking = TrackMouseEvent(&tme);
            }
            if (g.drag == kSeek) {
                SeekToX(GET_X_LPARAM(lp), true);
            } else if (g.drag == kVolumeBar) {
                SetVolumeToX(GET_X_LPARAM(lp));
            } else {
                SetHot(HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
                SetPanelHot({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            }
            UpdatePreview({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_MOUSELEAVE:
            g.tracking = false;
            SetHot(kNone);
            SetPanelHot({-1, -1});
            UpdatePreview({});
            if (g.playing && !g.drag && !g.panelOpen) SetControlsShown(false);
            return 0;
        case WM_MOUSEWHEEL: {  // scrolls the panel column under the mouse, or else changes the volume
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            int row;
            if (!g.panelOpen) {
                if (g.menuOpen || g.drag) return 0;
                // A step a notch, the finer steps of touchpads adding up to one.
                const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                if ((delta > 0) != (g.wheel > 0)) g.wheel = 0;  // turning back starts over
                g.wheel += delta;
                while (std::abs(g.wheel) >= WHEEL_DELTA) {
                    const bool up = g.wheel > 0;
                    g.wheel -= up ? WHEEL_DELTA : -WHEEL_DELTA;
                    // Steps in quick succession keep the icon up, like a held key.
                    StepVolume(up, static_cast<DWORD>(GetMessageTime() - g.lastWheelStep) < kOsdMs);
                    g.lastWheelStep = GetMessageTime();
                }
                return 0;
            }
            const PanelGeometry p = PanelLayout();
            const int column = PanelColumnAt(p, pt, row);
            if (column < 0) return 0;
            // Two rows a notch, and smoothly for the finer steps of touchpads and free-spinning wheels.
            g.columns[column].scroll -= MulDiv(GET_WHEEL_DELTA_WPARAM(wp), 2 * p.rowH, WHEEL_DELTA);
            g.hotColumn = -1;  // makes SetPanelHot redraw at the new scroll position
            SetPanelHot(pt);
            return 0;
        }
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
            StopMkvSubtitles();
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
    for (IDCompositionVisual** v : {g.background.GetAddressOf(), g.video.GetAddressOf(), g.osd.GetAddressOf(),
                                    g.flash.GetAddressOf(), g.controls.GetAddressOf(), g.panel.GetAddressOf(),
                                    g.menuBar.GetAddressOf()}) {
        if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(v);
        if (SUCCEEDED(hr)) hr = g.root->AddVisual(*v, FALSE, nullptr);  // in front of the previous ones
    }
    for (IDCompositionEffectGroup** e : {g.controlsFade.GetAddressOf(), g.flashFade.GetAddressOf()}) {
        if (SUCCEEDED(hr)) hr = g.dcomp->CreateEffectGroup(e);
        if (SUCCEEDED(hr)) hr = (*e)->SetOpacity(0.0f);
    }
    if (SUCCEEDED(hr)) hr = g.controls->SetEffect(g.controlsFade.Get());
    if (SUCCEEDED(hr)) hr = g.flash->SetEffect(g.flashFade.Get());
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateEffectGroup(&g.osdFade);
    if (SUCCEEDED(hr)) hr = g.osd->SetEffect(g.osdFade.Get());
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateScaleTransform(&g.flashScale);
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateTranslateTransform(&g.flashMove);
    if (SUCCEEDED(hr)) {
        ComPtr<IDCompositionTransform> transform;
        IDCompositionTransform* steps[] = {g.flashScale.Get(), g.flashMove.Get()};
        hr = g.dcomp->CreateTransformGroup(steps, UINT(std::size(steps)), &transform);
        if (SUCCEEDED(hr)) hr = g.flash->SetTransform(transform.Get());
    }
    // The preview's visuals are added to the controls while it shows.
    for (IDCompositionVisual** v :
         {g.previewShadow.GetAddressOf(), g.previewFrame.GetAddressOf(), g.previewImage.GetAddressOf(),
          g.previewTime.GetAddressOf()}) {
        if (SUCCEEDED(hr)) hr = g.dcomp->CreateVisual(v);
    }
    if (SUCCEEDED(hr)) hr = g.previewFrame->AddVisual(g.previewImage.Get(), FALSE, nullptr);
    if (SUCCEEDED(hr)) hr = g.dcomp->CreateRectangleClip(&g.previewClip);
    if (SUCCEEDED(hr)) hr = g.previewClip->SetLeft(0.0f);
    if (SUCCEEDED(hr)) hr = g.previewClip->SetTop(0.0f);
    if (SUCCEEDED(hr)) hr = g.previewFrame->SetClip(g.previewClip.Get());
    if (SUCCEEDED(hr)) hr = g.previewImage->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
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
    // On failure, destroying the window stops the player's threads, which must not outlive it.
    if (!InitPlayer(g.hwnd) || !InitComposition()) {
        MessageBoxW(nullptr, L"Can't start: Direct3D 11 or Media Foundation isn't available.", kAppName,
                    MB_OK | MB_ICONERROR);
        DestroyWindow(g.hwnd);
        return false;
    }
    if (std::none_of(files.begin(), files.end(), [](const std::wstring& f) { return !IsSubtitleFile(f); })) {
        ShowMainWindow();
        Layout();
        return true;
    }
    if (!OpenFiles(files)) {
        DestroyWindow(g.hwnd);
        return false;
    }
    // The window shows when the video's size is known (LOADEDMETADATA) so it opens at the right size.
    SetTimer(g.hwnd, kShowTimer, kShowFallbackMs, nullptr);
    return true;
}
