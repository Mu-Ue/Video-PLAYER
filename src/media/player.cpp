#include "media/player.h"

#undef GetCurrentTime  // a 16-bit era winbase.h macro that would rename IMFMediaEngine::GetCurrentTime

#include <audiosessiontypes.h>
#include <avrt.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>

#include "config.h"
#include "media/mkv.h"
#include "media/source.h"
#include "media/subtitles.h"
#include "ui/overlay.h"

using Microsoft::WRL::ComPtr;

namespace {

// Forwards engine events, which arrive on Media Foundation threads, to the window thread as msg, and wakes the
// render thread. Lives for the whole process, so it needs no reference counting.
class Notify final : public IMFMediaEngineNotify {
  public:
    HWND hwnd = nullptr;
    HANDLE wake = nullptr;

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *object = static_cast<IMFMediaEngineNotify*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD) override {
        PostMessageW(hwnd, WM_APP_MEDIA, event, LPARAM(param1));
        if (wake) SetEvent(wake);
        return S_OK;
    }
};

// Forwards the cues of the video's own subtitles, also from Media Foundation threads, as they start showing.
class TextNotify final : public IMFTimedTextNotify {
  public:
    HWND hwnd = nullptr;

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFTimedTextNotify)) {
            *object = static_cast<IMFTimedTextNotify*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    void STDMETHODCALLTYPE TrackAdded(DWORD) override { PostMessageW(hwnd, WM_APP_SUBTITLE_TRACK, 0, 0); }
    void STDMETHODCALLTYPE TrackRemoved(DWORD) override {}
    void STDMETHODCALLTYPE TrackSelected(DWORD, BOOL) override {}
    void STDMETHODCALLTYPE TrackReadyStateChanged(DWORD) override {}
    void STDMETHODCALLTYPE Error(MF_TIMED_TEXT_ERROR_CODE, HRESULT, DWORD) override {}
    void STDMETHODCALLTYPE Reset() override {}
    void STDMETHODCALLTYPE Cue(MF_TIMED_TEXT_CUE_EVENT event, double, IMFTimedTextCue* cue) override {
        if (event != MF_TIMED_TEXT_CUE_EVENT_ACTIVE || !cue) return;
        auto out = std::make_unique<::Cue>();
        out->start = cue->GetStartTime();
        out->end = out->start + cue->GetDuration();
        for (DWORD i = 0; i < cue->GetLineCount() && out->text.size() < kMaxCueChars; ++i) {
            ComPtr<IMFTimedTextFormattedText> line;
            LPWSTR text = nullptr;
            if (FAILED(cue->GetLine(i, &line)) || FAILED(line->GetText(&text)) || !text) continue;
            if (!out->text.empty()) out->text += L'\n';
            const UINT32 at = UINT32(out->text.size());
            out->text.append(text, std::min(wcslen(text), kMaxCueChars - at));
            CoTaskMemFree(text);
            const UINT32 length = UINT32(out->text.size()) - at;
            for (DWORD k = 0; k < line->GetSubformattingCount(); ++k) {
                DWORD first, count;
                ComPtr<IMFTimedTextStyle> style;
                auto font = MF_TIMED_TEXT_FONT_STYLE_NORMAL;
                BOOL bold = FALSE;
                if (FAILED(line->GetSubformatting(k, &first, &count, &style)) || !style || first >= length) continue;
                style->GetFontStyle(&font);
                style->GetBold(&bold);
                const bool italic = font != MF_TIMED_TEXT_FONT_STYLE_NORMAL;
                if (italic || bold) {
                    out->spans.push_back({at + first, std::min<UINT32>(count, length - first), italic, bold != FALSE});
                }
            }
        }
        if (out->text.empty() || !(out->end > out->start)) return;
        if (PostMessageW(hwnd, WM_APP_SUBTITLE_CUE, cue->GetTrackId(), LPARAM(out.get()))) out.release();
    }
};

struct {
    Notify notify;
    TextNotify textNotify;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D10Multithread> multithread;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<IMFMediaEngineEx> engine;
    ComPtr<IMFTimedText> timedText;  // null if unsupported
    std::vector<DWORD> audioStreams;  // the engine's stream index of each audio track
    // The seek bar preview: a thread that decodes just the key frame the window wants. The window hands it a file
    // to open or a key frame to decode under previewLock, then sets previewWake; it hands back the picture, and the
    // pictures' size, there.
    std::thread previewer;
    HANDLE previewWake = nullptr;
    // Signals previewDone when the previewer's GPU work is done; null if the device has no fences.
    ComPtr<ID3D11Fence> previewFence;
    HANDLE previewDone = nullptr;
    UINT64 previewFenceValue = 0;
    SRWLOCK previewLock = SRWLOCK_INIT;
    std::wstring previewPath;
    bool previewOpen = false;  // whether to open previewPath yet
    UINT previewWidth = 0, previewHeight = 0;
    LONGLONG previewWanted = -1;  // the key frame to decode, in 100 ns units, or -1
    LONGLONG readyTime = -1;      // a decoded picture waiting for the window, and its key frame
    ComPtr<ID3D11Texture2D> ready;
    ComPtr<ID3D11Texture2D> taken;  // the last one the window took, alive while it opens it
    double previewBucket = -1;      // the window's last span
    LONGLONG previewKey = -1;       // and its key frame
    bool previewKeyed = false;      // found with keyFrames
    // Where the open file's key frames are, from the previewer's parser of it: openedKeyFrames under previewLock as
    // it opens, keyFrames the window's once taken by KeyFrames. Null if the format doesn't say, or until then.
    ComPtr<IMFSeekInfo> openedKeyFrames;
    ComPtr<IMFSeekInfo> keyFrames;
    bool started = false;  // MFStartup succeeded
    // The swap chain, guarded by lock: the render thread presents into it and the window thread sizes it.
    SRWLOCK lock = SRWLOCK_INIT;
    ComPtr<IDXGISwapChain1> chain;
    ComPtr<IDXGISwapChain2> chain2;  // null before Windows 8.1: then the whole swap chain is drawn
    UINT width = 0, height = 0;
    UINT drawWidth = 0, drawHeight = 0;  // the part drawn: the video's size on screen, at most the swap chain's
    UINT sourceWidth = 0, sourceHeight = 0;  // the swap chain's source size, last set; 0 x 0 after a resize
    FramePainter* painter = nullptr;     // the render thread's, which keeps a buffer until told to let go
    // The subtitles drawn into the frames, which the window sets under subsLock, counting each change in subsVersion.
    SRWLOCK subsLock = SRWLOCK_INIT;
    std::shared_ptr<const Subtitles> subs;
    SubtitleLayout subsLayout{};
    std::atomic<UINT> subsVersion = 0;
    std::atomic<bool> redraw = false;  // present the current frame even if it isn't new; set with Redraw
    std::atomic<bool> frameWanted = false;  // post WM_APP_FRAME before the next frame; set with RequestFrame
    std::atomic<LONGLONG> landing = -1;     // of the last seek, in 100 ns units, until its frame shows
    std::atomic<bool> opened = false;       // a new video was opened, whose frames may come faster
    HANDLE stop = nullptr;             // tells the render thread and the previewer to exit
    HANDLE renderWake = nullptr;       // set on each engine event and redraw, while the render thread sleeps
    std::thread renderer;
    // The volume and mute, also kept here so they can be set before the engine is made.
    double volume = 1;
    bool muted = false;
} g;

HRESULT CreateDevice() {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &g.device, nullptr, nullptr);
    if (FAILED(hr)) {  // no GPU video support: decode in software, still present through the GPU
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &g.device, nullptr, nullptr);
    }
    if (SUCCEEDED(hr)) hr = g.device.As(&g.multithread);
    if (SUCCEEDED(hr)) g.multithread->SetMultithreadProtected(TRUE);
    // A present never waits behind older ones.
    ComPtr<IDXGIDevice1> dxgi;
    if (SUCCEEDED(hr) && SUCCEEDED(g.device.As(&dxgi))) dxgi->SetMaximumFrameLatency(1);
    return hr;
}

HRESULT CreateEngine(IMFMediaEngineNotify* notify, ComPtr<IMFMediaEngineEx>& out) {
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 5);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g.manager.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Movie);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_EXTENSION, SourceExtension());
    ComPtr<IMFMediaEngineClassFactory> factory;
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    ComPtr<IMFMediaEngine> engine;
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attributes.Get(), &engine);
    if (SUCCEEDED(hr)) hr = engine.As(&out);
    return hr;
}

HRESULT CreateEngines() {
    UINT token = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&token, &g.manager);
    if (SUCCEEDED(hr)) hr = g.manager->ResetDevice(g.device.Get(), token);
    if (SUCCEEDED(hr)) hr = CreateEngine(&g.notify, g.engine);
    ComPtr<IMFGetService> service;
    if (SUCCEEDED(hr) && SUCCEEDED(g.engine.As(&service)) &&
        SUCCEEDED(service->GetService(MF_MEDIA_ENGINE_TIMEDTEXT, IID_PPV_ARGS(&g.timedText))) &&
        FAILED(g.timedText->RegisterNotifications(&g.textNotify))) {
        g.timedText.Reset();
    }
    return hr;
}

// What subtitles drawn into frames w wide show.
struct FrameText {
    std::wstring text[2];  // bottom, top
    std::vector<StyleSpan> spans[2];
    SubtitleLayout layout{};
    UINT width = 0;
    bool operator==(const FrameText&) const = default;
};

// The render thread's subtitles: those drawn into the frames from time `from` until `next` (100 ns units), in the
// painter's slots 0 and 1, and in slots 2 and 3 those drawn ahead for a time to come.
struct FrameSubtitles {
    FramePainter painter;  // opened for the first text, as most videos have none
    bool opened = false;
    UINT version = UINT_MAX;
    UINT width = 0;  // of the frames they are laid out for
    LONGLONG from = LLONG_MAX, next = LLONG_MIN;
    FrameText drawn, ahead;
    POINT at[2]{}, aheadAt[2]{};  // where the bottom and top ones go
    UINT aheadVersion = UINT_MAX, aheadWidth = 0;
    LONGLONG aheadTime = LLONG_MIN;  // looked up ahead for
};

// The cues showing at time t, a frame's (negative before the first), for frames w wide; sets next to when that
// changes.
FrameText TextAt(LONGLONG t, UINT w, LONGLONG& next) {
    AcquireSRWLockShared(&g.subsLock);
    const std::shared_ptr<const Subtitles> subs = g.subs;
    FrameText f{.layout = g.subsLayout, .width = w};
    ReleaseSRWLockShared(&g.subsLock);
    std::vector<size_t> active;
    double change = INFINITY;
    // Half a 100 ns unit past the frame, so a cue time that rounding puts just past it still counts as reached.
    if (subs && t >= 0) change = ActiveCues(*subs, (double(t) + 0.5) / 1e7, active);
    next = t < 0 ? 0 : change < 1e11 ? LONGLONG(std::ceil(change * 1e7 - 0.5)) : LLONG_MAX;
    for (const size_t i : active) {
        const Cue& cue = subs->cues[i];
        std::wstring& s = f.text[cue.top];
        if (!s.empty()) s += L'\n';
        const UINT32 at = UINT32(s.size());
        s += cue.text;
        for (StyleSpan span : cue.spans) {
            span.start += at;
            f.spans[cue.top].push_back(span);
        }
    }
    return f;
}

// Draws the text into the painter's slots first and first + 1 and sets where they go.
void PrepareText(FrameSubtitles& subtitles, int first, const FrameText& f, POINT at[2]) {
    FramePainter& painter = subtitles.painter;
    if (!subtitles.opened && (!f.text[0].empty() || !f.text[1].empty())) {
        subtitles.opened = true;
        // Under lock, as the window lets go of the painter's target.
        AcquireSRWLockExclusive(&g.lock);
        painter.Open(g.device.Get());
        ReleaseSRWLockExclusive(&g.lock);
    }
    const SubtitleLayout& layout = f.layout;
    // Frames are drawn at another size than on screen only before Windows 8.1.
    const float w = float(f.width), scale = layout.width > 0 ? w / layout.width : 1;
    for (int top = 0; top < 2; ++top) {
        const SIZE s =
            painter.Prepare(first + top, f.text[top], f.spans[top], layout.fontSize * scale, layout.maxWidth * scale);
        at[top].x = LONG(std::round((w - float(s.cx)) / 2));
        at[top].y = LONG(std::round(top ? layout.top * scale : std::max(0.0f, layout.bottom * scale - float(s.cy))));
    }
}

// Has the painter draw the cues showing at time t into frames w wide, unless it already does. Those drawn ahead
// are swapped in rather than drawn again.
void UpdateFrameSubtitles(FrameSubtitles& f, LONGLONG t, UINT w) {
    const UINT version = g.subsVersion;
    if (version == f.version && w == f.width && t >= f.from && t < f.next) return;
    f.version = version;
    f.width = w;
    f.from = t;
    FrameText text = TextAt(t, w, f.next);
    if (text == f.drawn) return;
    if (text == f.ahead) {
        f.painter.Swap();
        std::swap(f.drawn, f.ahead);
        std::swap(f.at, f.aheadAt);
        return;
    }
    PrepareText(f, 0, text, f.at);
    f.drawn = std::move(text);
}

// Draws the cues that show from f.next ahead into slots 2 and 3, so the frame they start on only swaps them in.
void PrepareAhead(FrameSubtitles& f) {
    if (f.next == LLONG_MAX || (f.version == f.aheadVersion && f.width == f.aheadWidth && f.next == f.aheadTime)) {
        return;
    }
    f.aheadVersion = f.version;
    f.aheadWidth = f.width;
    f.aheadTime = f.next;
    LONGLONG next;
    FrameText text = TextAt(f.next, f.width, next);
    if (text == f.drawn || text == f.ahead) return;
    PrepareText(f, 2, text, f.aheadAt);
    f.ahead = std::move(text);
}

// Copies the engine's current frame into the top left w x h of the swap chain, draws the subtitles over it and
// presents just that, which the swap chain stretches to its whole size. Call with lock held.
bool PresentFrame(UINT w, UINT h, FrameSubtitles& subtitles) {
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(g.chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
    const RECT dest{0, 0, LONG(w), LONG(h)};
    const MFARGB black{0, 0, 0, 255};
    if (FAILED(g.engine->TransferVideoFrame(buffer.Get(), nullptr, &dest, &black))) return false;
    subtitles.painter.Draw(buffer.Get(), subtitles.at);  // the frame still shows if they can't be
    // Takes effect with this present, so the picture and its size change in the same compositor frame.
    if (g.chain2 && (w != g.sourceWidth || h != g.sourceHeight)) {
        if (FAILED(g.chain2->SetSourceSize(w, h))) return false;
        g.sourceWidth = w;
        g.sourceHeight = h;
    }
    return SUCCEEDED(g.chain->Present(0, 0));
}

// Sizes a swap chain for DirectComposition, creating it on the first call. Call with lock held.
HRESULT SizeChain(ComPtr<IDXGISwapChain1>& chain, UINT width, UINT height) {
    if (chain) return chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = g.device.As(&dxgi);
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForComposition(g.device.Get(), &desc, nullptr, &chain);
    return hr;
}

// The engine's video display size in pixels, or 0 x 0.
SIZE DisplaySize(IMFMediaEngineEx* engine) {
    DWORD w = 0, h = 0;
    if (!engine || !engine->HasVideo() || FAILED(engine->GetNativeVideoSize(&w, &h)) || !w || !h) return {};
    // The picture aspect ratio differs from w:h for non-square pixels (anamorphic video): stretch the width.
    DWORD ax = 0, ay = 0;
    if (SUCCEEDED(engine->GetVideoAspectRatio(&ax, &ay)) && ax && ay && UINT64(ax) * h != UINT64(ay) * w) {
        w = DWORD(std::lround(double(h) * ax / ay));
    }
    return {LONG(w), LONG(h)};
}

// Has the render thread present the current frame even if it isn't new.
void Redraw() {
    g.redraw = true;
    if (g.renderWake) SetEvent(g.renderWake);
}

}  // namespace

void RequestFrame() {
    g.frameWanted = true;
    if (g.renderWake) SetEvent(g.renderWake);
}

namespace {

// Tells how often the compositor composes: through the frame statistics of Windows 11, or before that through a
// device of its own.
class CompositorClock {
  public:
    explicit CompositorClock(ID3D11Device* device) {
        const HMODULE dcomp = GetModuleHandleW(L"dcomp.dll");
        getFrameId_ = reinterpret_cast<GetFrameIdFn>(GetProcAddress(dcomp, "DCompositionGetFrameId"));
        getStatistics_ = reinterpret_cast<GetStatisticsFn>(GetProcAddress(dcomp, "DCompositionGetStatistics"));
        if (!getFrameId_ || !getStatistics_) {
            if (ComPtr<IDXGIDevice> dxgi; SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
                DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&device_));
            }
        }
        QueryPerformanceFrequency(&frequency_);
    }

    // The time between compositor frames, in performance counter ticks; 0 if unknown.
    LONGLONG Period() const {
        if (getFrameId_ && getStatistics_) {
            COMPOSITION_FRAME_ID id;
            COMPOSITION_FRAME_STATS stats;
            return SUCCEEDED(getFrameId_(COMPOSITION_FRAME_ID_CREATED, &id)) &&
                           SUCCEEDED(getStatistics_(id, &stats, 0, nullptr, nullptr))
                       ? LONGLONG(stats.framePeriod)
                       : 0;
        }
        DCOMPOSITION_FRAME_STATISTICS stats;
        if (!device_ || FAILED(device_->GetFrameStatistics(&stats)) || !stats.currentCompositionRate.Numerator) return 0;
        return frequency_.QuadPart * stats.currentCompositionRate.Denominator / stats.currentCompositionRate.Numerator;
    }
    LONGLONG Frequency() const { return frequency_.QuadPart; }

  private:
    using GetFrameIdFn = HRESULT(WINAPI*)(COMPOSITION_FRAME_ID_TYPE, COMPOSITION_FRAME_ID*);
    using GetStatisticsFn = HRESULT(WINAPI*)(COMPOSITION_FRAME_ID, COMPOSITION_FRAME_STATS*, UINT,
                                             COMPOSITION_TARGET_ID*, UINT*);
    GetFrameIdFn getFrameId_ = nullptr;
    GetStatisticsFn getStatistics_ = nullptr;
    ComPtr<IDCompositionDevice> device_;
    LARGE_INTEGER frequency_;
};

// Waits until shortly before the compositor frame after the one at tick, a performance counter time, period
// ticks long, so the window can update as late as it can and still make that frame; at once if it can't tell when
// that is. Returns false if told to stop.
bool WaitBeforeNextFrame(const CompositorClock& clock, HANDLE timer, LONGLONG tick, LONGLONG period) {
    const LONGLONG freq = clock.Frequency();
    if (!timer || period <= 0) return true;
    const LONGLONG next = tick + period;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Until 2 ms before it, for the window and the compositor, in 100 ns units.
    const LONGLONG wait = (next - now.QuadPart - freq / 500) * 10'000'000 / freq;
    if (wait <= 0) return true;
    LARGE_INTEGER due;
    due.QuadPart = -wait;  // relative
    if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) return true;
    const HANDLE events[] = {g.stop, timer};
    return WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0;
}

// Once per compositor frame, presents the engine's frame for that moment if it is a new one. Nothing is queued
// ahead, so pausing and seeking show on screen as soon as the engine has the frame. While paused, or with no video,
// it sleeps once kIdleTicks frames have passed with nothing new, until an engine event or a redraw.
void RenderLoop() {
    constexpr int kIdleTicks = 60;  // grace for a frame still on its way after an event
    // Scheduled as playback, so frames stay on time while the CPU is busy.
    DWORD task = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Playback", &task);
    // The compositor clock is Windows 11 only; Windows 10 waits for the vertical blank of the first monitor.
    using WaitFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
    const auto waitClock = reinterpret_cast<WaitFn>(
        GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionWaitForCompositorClock"));
    ComPtr<IDXGIOutput> output;
    if (!waitClock) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(g.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter->EnumOutputs(0, &output);
    }
    // Without it or a fine timer, WM_APP_FRAME is posted at once.
    const CompositorClock clock(g.device.Get());
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    constexpr int kLatchTicks = 30;  // frames to keep waiting for WM_APP_FRAME's moment after the last request
    const HANDLE events[] = {g.stop, g.renderWake};
    int idle = 0, latchGrace = 0;
    FrameSubtitles subtitles;
    AcquireSRWLockExclusive(&g.lock);
    g.painter = &subtitles.painter;
    ReleaseSRWLockExclusive(&g.lock);
    LONGLONG frameTime = -1;  // of the frame on screen
    LONGLONG fastest = 0;     // the shortest time between frames of the video yet, or 0
    LONGLONG previous = -1;   // the video's last frame, for that
    for (;;) {
        if (g.opened.exchange(false)) fastest = 0, previous = -1;
        if (idle >= kIdleTicks && (!g.engine->HasVideo() || (g.engine->IsPaused() && !g.redraw))) {
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            idle = 0;
        }
        if (waitClock) {
            const DWORD r = waitClock(1, &g.stop, INFINITE);
            if (r == WAIT_OBJECT_0) break;
            if (r != WAIT_OBJECT_0 + 1 && WaitForSingleObject(g.stop, 16) != WAIT_TIMEOUT) break;
        } else if (!output || FAILED(output->WaitForVBlank())) {
            if (WaitForSingleObject(g.stop, 16) != WAIT_TIMEOUT) break;
        } else if (WaitForSingleObject(g.stop, 0) != WAIT_TIMEOUT) {
            break;
        }
        LARGE_INTEGER tick;
        QueryPerformanceCounter(&tick);
        // The compositor's period, looked up once a tick and only if needed.
        LONGLONG period = -1;
        const auto framePeriod = [&] { return period < 0 ? period = clock.Period() : period; };
        // An event that came while awake restarts the grace.
        idle = WaitForSingleObject(g.renderWake, 0) == WAIT_OBJECT_0 ? 0 : idle + 1;
        LONGLONG pts;
        bool fresh = g.engine->OnVideoStreamTick(&pts) == S_OK;
        // While playing, a frame not due yet is looked for again just before the next compositor frame, so it makes
        // that frame rather than racing the compositor at the following tick; unless none can be due by then, as
        // frames come no faster than the fastest yet.
        if (!fresh && waitClock && !g.engine->IsPaused() &&
            (!fastest || frameTime + fastest - std::llround(g.engine->GetCurrentTime() * 1e7) <=
                             framePeriod() * 10'000'000 / clock.Frequency())) {
            if (!WaitBeforeNextFrame(clock, timer, tick.QuadPart, framePeriod())) break;
            fresh = g.engine->OnVideoStreamTick(&pts) == S_OK;
        }
        if (fresh) {
            // The engine gives the frame a seek lands on as at the seek's time, which can be past the frame's own
            // time, and so past a cue's end.
            LONGLONG landing = g.landing;
            if (landing >= 0 && std::abs(pts - landing) <= 1 && g.landing.compare_exchange_strong(landing, -1)) {
                pts = LandedFrameTime(pts);
            }
            if (previous >= 0 && pts > previous && (!fastest || pts - previous < fastest)) fastest = pts - previous;
            frameTime = previous = pts;
        }
        if (fresh || g.redraw) {
            // The subtitles are laid out outside the lock, so the window never waits for that.
            AcquireSRWLockShared(&g.lock);
            const bool part = g.chain2 && g.drawWidth && g.drawHeight;
            const UINT w = part ? std::min(g.drawWidth, g.width) : g.width;
            const UINT h = part ? std::min(g.drawHeight, g.height) : g.height;
            ReleaseSRWLockShared(&g.lock);
            UpdateFrameSubtitles(subtitles, frameTime, w);
            AcquireSRWLockExclusive(&g.lock);
            g.redraw = !g.chain || !w || !h || !PresentFrame(std::min(w, g.width), std::min(h, g.height), subtitles);
            ReleaseSRWLockExclusive(&g.lock);
            if (!g.redraw) {
                idle = 0;
                // The next frame is furthest off now, so the next subtitles are drawn now rather than just before it.
                PrepareAhead(subtitles);
            }
        }
        // Then waits for the next frame's latest moment while frames are wanted, and a little after, as a request
        // that comes just after a frame begins would otherwise wait out the next one.
        if (g.frameWanted || latchGrace > 0) {
            if (!WaitBeforeNextFrame(clock, timer, tick.QuadPart, framePeriod())) break;
            if (g.frameWanted.exchange(false)) {
                latchGrace = kLatchTicks;
                PostMessageW(g.notify.hwnd, WM_APP_FRAME, 0, 0);
            } else {
                --latchGrace;
            }
        }
    }
    AcquireSRWLockExclusive(&g.lock);
    g.painter = nullptr;
    ReleaseSRWLockExclusive(&g.lock);
    if (timer) CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

// The open file's key frames, for the window thread; null until the previewer has opened the file, or if the format
// doesn't say.
IMFSeekInfo* KeyFrames() {
    if (!g.keyFrames) {
        AcquireSRWLockShared(&g.previewLock);
        g.keyFrames = g.openedKeyFrames;
        ReleaseSRWLockShared(&g.previewLock);
    }
    return g.keyFrames.Get();
}

// Whether a path names a device, such as a pipe, console or raw volume, rather than a file: "\\.\…", "\??\…",
// "\\?\…" other than the "\\?\C:\…" and "\\?\UNC\…" forms of long file paths, or a "\\host\pipe\…" or mailslot
// share. Either slash works.
bool IsDevicePath(const std::wstring& path) {
    const auto slash = [&](size_t i) { return i < path.size() && (path[i] == L'\\' || path[i] == L'/'); };
    if (path.size() < 4 || !slash(0)) return false;
    if (path[1] == L'?' && path[2] == L'?' && slash(3)) return true;
    if (!slash(1)) return false;  // a rooted path, "\dir\…"
    size_t host = 2;  // where the server name of a UNC path starts
    if ((path[2] == L'.' || path[2] == L'?') && slash(3)) {
        if (path[2] == L'.') return true;
        if (path.size() > 6 && path[5] == L':' && slash(6)) return false;  // \\?\C:\…
        if (_wcsnicmp(path.c_str() + 4, L"UNC", 3) != 0 || !slash(7)) return true;
        host = 8;
    }
    const size_t share = path.find_first_of(L"\\/", host);
    if (share == std::wstring::npos) return false;
    const size_t end = std::min(path.find_first_of(L"\\/", share + 1), path.size());
    const auto isShare = [&](const wchar_t* name) {
        return CompareStringOrdinal(path.c_str() + share + 1, int(end - share - 1), name, -1, TRUE) == CSTR_EQUAL;
    };
    return isShare(L"pipe") || isShare(L"mailslot");
}

// Takes a string the callee allocated.
std::wstring Take(LPWSTR text) {
    std::wstring s = text ? text : L"";
    CoTaskMemFree(text);
    return s;
}

std::wstring StreamString(DWORD stream, REFGUID key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring s;
    if (SUCCEEDED(g.engine->GetStreamAttribute(stream, key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        s = value.pwszVal;
    }
    PropVariantClear(&value);
    return s;
}

// Where the key frames are, from a list of their times, for a file whose parser doesn't say.
class KeyFrameList final : public IMFSeekInfo {
  public:
    explicit KeyFrameList(std::vector<LONGLONG> times) : times_(std::move(times)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = riid == __uuidof(IUnknown) || riid == __uuidof(IMFSeekInfo) ? this : nullptr;
        if (!*object) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    // The key frames at or before the time and after it, or the first or last one past either end.
    STDMETHODIMP GetNearestKeyFrames(const GUID* format, const PROPVARIANT* start, PROPVARIANT* before,
                                     PROPVARIANT* after) override {
        if ((format && *format != GUID_NULL) || !start || start->vt != VT_I8 || !before || !after) return E_INVALIDARG;
        const auto next = std::upper_bound(times_.begin(), times_.end(), start->hVal.QuadPart);
        before->vt = after->vt = VT_I8;
        before->hVal.QuadPart = next == times_.begin() ? times_.front() : *(next - 1);
        after->hVal.QuadPart = next == times_.end() ? times_.back() : *next;
        return S_OK;
    }

  private:
    std::atomic<ULONG> refs_ = 1;
    const std::vector<LONGLONG> times_;  // not empty
};

// The key frame nearest to a time, in seconds, or the time itself if unknown.
double NearestKeyFrame(IMFSeekInfo* keyFrames, double seconds) {
    if (!keyFrames) return seconds;
    PROPVARIANT start{}, before{}, after{};  // 100 ns units, which need no clearing
    start.vt = VT_I8;
    start.hVal.QuadPart = LONGLONG(seconds * 1e7);
    if (SUCCEEDED(keyFrames->GetNearestKeyFrames(&GUID_NULL, &start, &before, &after)) && before.vt == VT_I8 &&
        after.vt == VT_I8) {
        const double a = before.hVal.QuadPart / 1e7, b = after.hVal.QuadPart / 1e7;
        seconds = seconds - a <= b - seconds ? a : b;
    }
    return seconds;
}

constexpr DWORD kVideoStream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr double kPreviewCount = 200;   // preview pictures across the whole seek bar
constexpr double kMinPreviewSpan = 1;  // seconds of video each preview picture stands for, at least

// A parser of the file of its own, for the previews' readers; null if it can't be made.
ComPtr<IMFMediaSource> OpenPreviewSource(const std::wstring& path) {
    ComPtr<IMFByteStream> stream;
    ComPtr<IMFSourceResolver> resolver;
    ComPtr<IUnknown> object;
    ComPtr<IMFMediaSource> source;
    MF_OBJECT_TYPE type;
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path.c_str(),
                              &stream);
    if (SUCCEEDED(hr)) hr = MFCreateSourceResolver(&resolver);
    if (SUCCEEDED(hr)) {
        hr = resolver->CreateObjectFromByteStream(stream.Get(), path.c_str(),
                                                  MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ, nullptr, &type,
                                                  &object);
    }
    if (SUCCEEDED(hr)) hr = object.As(&source);
    return SUCCEEDED(hr) ? source : nullptr;
}

// A reader of the source's video alone, and w x h, its display size fitted to 320 px, about the largest the preview
// shows; null if it has no video. It hands out frames as stored until SetPreviewType, and leaves the source running
// once let go, for the next reader.
ComPtr<IMFSourceReader> OpenPreviewReader(IMFMediaSource* source, UINT& w, UINT& h) {
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> native;
    HRESULT hr = MFCreateAttributes(&attributes, 5);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g.manager.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    // The decoder hands out a key frame as soon as it has it, rather than holding it to reorder later frames.
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source, attributes.Get(), &reader);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(kVideoStream, TRUE);
    if (SUCCEEDED(hr)) hr = reader->GetNativeMediaType(kVideoStream, 0, &native);
    UINT32 fw = 0, fh = 0;
    if (SUCCEEDED(hr)) hr = MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &fw, &fh);
    if (FAILED(hr) || !fw || !fh) return nullptr;
    // Non-square pixels (anamorphic video) stretch the width.
    UINT32 ax = 1, ay = 1;
    if (FAILED(MFGetAttributeRatio(native.Get(), MF_MT_PIXEL_ASPECT_RATIO, &ax, &ay)) || !ax || !ay) ax = ay = 1;
    const double dw = double(fw) * ax / ay, scale = 320.0 / std::max(dw, double(fh));
    w = UINT(std::max(2L, std::lround(dw * scale / 2) * 2));  // even, as video processing likes
    h = UINT(std::max(2L, std::lround(fh * scale / 2) * 2));
    return reader;
}

// Has the reader decode on the GPU and hand out each frame shrunk there to w x h; false if it can't.
bool SetPreviewType(IMFSourceReader* reader, UINT w, UINT h) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);  // the swap chain's format
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(kVideoStream, nullptr, type.Get());
    return SUCCEEDED(hr);
}

// Seeks the reader to the key frame at or before a time, in 100 ns units, and decodes just that frame; null if it
// can't.
ComPtr<IMFSample> ReadKeyFrame(IMFSourceReader* reader, LONGLONG time) {
    PROPVARIANT position{};  // needs no clearing
    position.vt = VT_I8;
    position.hVal.QuadPart = time;
    if (FAILED(reader->SetCurrentPosition(GUID_NULL, position))) return nullptr;
    ComPtr<IMFSample> sample;
    DWORD flags = 0;
    LONGLONG at = 0;
    while (!sample) {  // a gap in the stream comes as no sample
        if (FAILED(reader->ReadSample(kVideoStream, 0, nullptr, &flags, &at, &sample)) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            return nullptr;
        }
    }
    return sample;
}

// Copies a decoded frame into a texture of its own, the preview's size, shared with the window's device; null if
// it can't. The keyed mutex hands it over with key 1 once the copy is done. Only the previewer calls it.
ComPtr<ID3D11Texture2D> KeepKeyFrame(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgi;
    ComPtr<ID3D11Texture2D> texture;
    UINT index = 0;
    if (!sample || FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgi)) ||
        FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture))) || FAILED(dxgi->GetSubresourceIndex(&index))) {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return nullptr;
    const UINT w = g.previewWidth, h = g.previewHeight;  // only the previewer sets them
    const D3D11_TEXTURE2D_DESC keep{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
                                    D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX};
    ComPtr<ID3D11Texture2D> kept;
    ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(g.device->CreateTexture2D(&keep, nullptr, &kept)) || FAILED(kept.As(&mutex)) ||
        mutex->AcquireSync(0, INFINITE) != S_OK) {
        return nullptr;
    }
    ComPtr<ID3D11DeviceContext> context;
    g.device->GetImmediateContext(&context);
    const D3D11_BOX box{0, 0, 0, std::min(desc.Width, w), std::min(desc.Height, h), 1};
    context->CopySubresourceRegion(kept.Get(), 0, 0, 0, 0, texture.Get(), index, &box);
    mutex->ReleaseSync(1);
    // Waits until the GPU has decoded and copied it: the window would wait for that on its own thread, and miss
    // frames. Without a fence, it at least sends the work to the GPU now.
    ComPtr<ID3D11DeviceContext4> context4;
    const bool fenced = g.previewFence && SUCCEEDED(context.As(&context4)) &&
                        SUCCEEDED(context4->Signal(g.previewFence.Get(), ++g.previewFenceValue)) &&
                        SUCCEEDED(g.previewFence->SetEventOnCompletion(g.previewFenceValue, g.previewDone));
    context->Flush();
    if (fenced) WaitForSingleObject(g.previewDone, 1000);
    return kept;
}

// Opens the files, which also tells where their key frames are, and decodes the key frames the window wants, one at
// a time, always the latest. Nothing queues: each wake reads what is wanted now, and it decodes back to back while
// that changes. Each picture is handed to the window, which keeps it, even if the mouse moved on while it decoded.
// The decoder, which holds frames of the video's full size, is let go once none was wanted for kPreviewIdleMs.
void PreviewLoop() {
    constexpr DWORD kPreviewIdleMs = 15000;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;  // null while idle
    UINT w = 0, h = 0;
    bool decodes = false;  // a reader shrinks frames for the previews
    LONGLONG done = -1;  // the key frame last decoded, or tried, in 100 ns units
    const HANDLE events[] = {g.stop, g.previewWake};
    for (;;) {
        AcquireSRWLockExclusive(&g.previewLock);
        const std::wstring path = g.previewOpen ? std::exchange(g.previewPath, {}) : std::wstring();
        const LONGLONG time = g.previewWanted;
        ReleaseSRWLockExclusive(&g.previewLock);
        if (!path.empty()) {
            reader.Reset();
            if (source) source->Shutdown();
            source = OpenPreviewSource(path);
            reader = source ? OpenPreviewReader(source.Get(), w, h) : nullptr;
            decodes = reader && SetPreviewType(reader.Get(), w, h);
            done = -1;
            // The parser knows where the key frames are, if the format says.
            ComPtr<IMFGetService> service;
            ComPtr<IMFSeekInfo> keyFrames;
            if (source && SUCCEEDED(source.As(&service))) {
                service->GetService(MF_SCRUBBING_SERVICE, IID_PPV_ARGS(&keyFrames));
            }
            // Media Foundation's Matroska parser doesn't say, but the file's index lists them.
            if (std::vector<LONGLONG> times; !keyFrames && !(times = MkvKeyFrames(path.c_str())).empty()) {
                keyFrames.Attach(new (std::nothrow) KeyFrameList(std::move(times)));
            }
            AcquireSRWLockExclusive(&g.previewLock);
            if (g.previewPath.empty()) g.openedKeyFrames = std::move(keyFrames);  // else another file is waiting
            g.previewWidth = decodes ? w : 0;  // audio only, or a format it can't shrink: no previews
            g.previewHeight = decodes ? h : 0;
            ReleaseSRWLockExclusive(&g.previewLock);
        }
        if (!decodes || time < 0 || time == done) {
            const DWORD r = WaitForMultipleObjects(2, events, FALSE, reader ? kPreviewIdleMs : INFINITE);
            if (r == WAIT_TIMEOUT) {
                reader.Reset();
                // Destroyed once the device's queue goes to the GPU.
                ComPtr<ID3D11DeviceContext> context;
                g.device->GetImmediateContext(&context);
                context->Flush();
            }
            if (r != WAIT_OBJECT_0 + 1 && r != WAIT_TIMEOUT) break;
            continue;
        }
        if (!reader) {
            reader = OpenPreviewReader(source.Get(), w, h);
            decodes = reader && SetPreviewType(reader.Get(), w, h);
            if (!decodes) continue;
        }
        done = time;  // a key frame it can't decode isn't tried again and again
        const ComPtr<ID3D11Texture2D> picture = KeepKeyFrame(ReadKeyFrame(reader.Get(), time).Get());
        if (!picture) continue;
        AcquireSRWLockExclusive(&g.previewLock);
        const bool reopened = !g.previewPath.empty();  // a picture of the previous file
        if (!reopened) {
            g.readyTime = time;
            g.ready = picture;
        }
        ReleaseSRWLockExclusive(&g.previewLock);
        if (!reopened) PostMessageW(g.notify.hwnd, WM_APP_PREVIEW, 0, 0);
    }
    reader.Reset();
    if (source) source->Shutdown();
    if (com) CoUninitialize();
}

}  // namespace

void PrepareVideo(const std::wstring& path) {
    PrepareSourceExtension(path.empty() || IsDevicePath(path) ? nullptr : path.c_str());
}

bool InitPlayer(HWND hwnd) {
    g.notify.hwnd = g.textNotify.hwnd = hwnd;
    if (FAILED(CreateDevice())) return false;
    g.started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!g.started || FAILED(CreateEngines())) return false;
    g.engine->SetVolume(g.volume);
    g.engine->SetMuted(g.muted);
    g.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.previewWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.renderWake = g.notify.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.stop || !g.previewWake || !g.renderWake) return false;
    ComPtr<ID3D11Device5> device5;
    g.previewDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g.previewDone && SUCCEEDED(g.device.As(&device5))) {
        device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.previewFence));
    }
    g.renderer = std::thread(RenderLoop);
    g.previewer = std::thread(PreviewLoop);
    return true;
}

void ShutdownPlayer() {
    if (g.stop) SetEvent(g.stop);
    if (g.renderer.joinable()) g.renderer.join();
    if (g.previewer.joinable()) g.previewer.join();
    g.previewFence.Reset();
    g.chain2.Reset();
    g.chain.Reset();
    g.subs.reset();
    g.ready.Reset();
    g.taken.Reset();
    g.keyFrames.Reset();
    g.openedKeyFrames.Reset();
    g.timedText.Reset();
    if (g.engine) g.engine->Shutdown();
    g.engine.Reset();
    g.manager.Reset();
    if (g.started) MFShutdown();
    g.started = false;
}

HRESULT OpenVideo(const std::wstring& path) {
    if (IsDevicePath(path)) return E_ACCESSDENIED;
    // A byte stream keeps the source a local file: the path is never parsed as a URL.
    ComPtr<IMFByteStream> stream;
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path.c_str(),
                              &stream);
    if (FAILED(hr)) return hr;
    BSTR name = SysAllocString(path.c_str());  // the extension helps pick the container parser
    if (!name) return E_OUTOFMEMORY;
    g.opened = true;
    hr = g.engine->SetSourceFromByteStream(stream.Get(), name);
    SysFreeString(name);
    Redraw();  // the first frame, even if it never plays
    if (SUCCEEDED(hr)) hr = g.engine->Play();
    if (FAILED(hr)) return hr;
    // The last file's key frames and previews are done with; OpenSeekHelpers has the previewer open this one.
    g.keyFrames.Reset();
    AcquireSRWLockExclusive(&g.previewLock);
    g.openedKeyFrames.Reset();
    g.previewPath = path;
    g.previewOpen = false;
    g.previewWanted = -1;
    g.ready.Reset();
    ReleaseSRWLockExclusive(&g.previewLock);
    g.previewBucket = -1;
    g.previewKey = -1;
    return hr;
}

void OpenSeekHelpers() {
    // The previewer reads the file on its own; if it can't, there are just no previews or key frames.
    AcquireSRWLockExclusive(&g.previewLock);
    g.previewOpen = true;
    ReleaseSRWLockExclusive(&g.previewLock);
    SetEvent(g.previewWake);
}

LONGLONG PreviewKeyFrame(double seconds, double duration) {
    // A span grows with the video, so a sweep of the bar shows as many pictures however long it is; moves within
    // one look up nothing.
    const double span = std::max(kMinPreviewSpan, duration / kPreviewCount);
    const double bucket = std::floor(seconds / span);
    IMFSeekInfo* keyFrames = KeyFrames();
    if (bucket == g.previewBucket && g.previewKeyed == !!keyFrames) return g.previewKey;
    g.previewBucket = bucket;
    g.previewKeyed = keyFrames;
    // Spans can share a key frame.
    g.previewKey = std::llround(NearestKeyFrame(keyFrames, (bucket + 0.5) * span) * 1e7);
    return g.previewKey;
}

LONGLONG PreviewKeyFrameNear(double seconds, double duration, int spans) {
    const double span = std::max(kMinPreviewSpan, duration / kPreviewCount);
    const double bucket = std::floor(seconds / span) + spans;
    if (bucket < 0 || bucket * span >= duration) return -1;
    return std::llround(NearestKeyFrame(KeyFrames(), (bucket + 0.5) * span) * 1e7);
}

void DecodePreview(LONGLONG time) {
    AcquireSRWLockExclusive(&g.previewLock);
    g.previewWanted = time;
    ReleaseSRWLockExclusive(&g.previewLock);
    SetEvent(g.previewWake);
}

HANDLE TakePreviewPicture(LONGLONG& time) {
    AcquireSRWLockExclusive(&g.previewLock);
    g.taken = std::move(g.ready);
    time = g.readyTime;
    ReleaseSRWLockExclusive(&g.previewLock);
    ComPtr<IDXGIResource> resource;
    HANDLE handle = nullptr;
    if (!g.taken || FAILED(g.taken.As(&resource)) || FAILED(resource->GetSharedHandle(&handle))) return nullptr;
    return handle;
}

SIZE PreviewSize() {
    AcquireSRWLockShared(&g.previewLock);
    const SIZE size{LONG(g.previewWidth), LONG(g.previewHeight)};
    ReleaseSRWLockShared(&g.previewLock);
    return size;
}

bool HasVideo() { return g.engine && g.engine->HasVideo(); }

std::vector<Track> AudioTracks() {
    std::vector<Track> tracks;
    g.audioStreams.clear();
    DWORD count = 0;
    if (!g.engine || FAILED(g.engine->GetNumberOfStreams(&count))) return tracks;
    for (DWORD i = 0; i < count; ++i) {
        PROPVARIANT type;
        PropVariantInit(&type);
        const bool audio = SUCCEEDED(g.engine->GetStreamAttribute(i, MF_MT_MAJOR_TYPE, &type)) &&
                           type.vt == VT_CLSID && type.puuid && *type.puuid == MFMediaType_Audio;
        PropVariantClear(&type);
        if (!audio) continue;
        BOOL selected = FALSE;
        g.engine->GetStreamSelection(i, &selected);
        tracks.push_back({StreamString(i, MF_SD_LANGUAGE), StreamString(i, MF_SD_STREAM_NAME), 0, selected != FALSE});
        g.audioStreams.push_back(i);
    }
    return tracks;
}

void SelectAudioTrack(size_t index) {
    if (index >= g.audioStreams.size()) return;
    for (size_t k = 0; k < g.audioStreams.size(); ++k) g.engine->SetStreamSelection(g.audioStreams[k], k == index);
    g.engine->ApplyStreamSelections();
}

std::vector<Track> SubtitleTracks() {
    std::vector<Track> tracks;
    ComPtr<IMFTimedTextTrackList> list;
    if (!g.timedText || FAILED(g.timedText->GetTextTracks(&list))) return tracks;
    for (DWORD i = 0; i < list->GetLength(); ++i) {
        ComPtr<IMFTimedTextTrack> track;
        LPWSTR language = nullptr, label = nullptr;
        if (FAILED(list->GetTrack(i, &track))) continue;
        track->GetLanguage(&language);
        track->GetLabel(&label);
        tracks.push_back({Take(language), Take(label), track->GetId(), false});
    }
    return tracks;
}

bool SelectSubtitleTrack(DWORD id) {
    ComPtr<IMFTimedTextTrackList> list;
    if (!g.timedText || FAILED(g.timedText->GetTextTracks(&list))) return false;
    for (DWORD i = 0; i < list->GetLength(); ++i) {
        ComPtr<IMFTimedTextTrack> track;
        if (SUCCEEDED(list->GetTrack(i, &track)) && track->GetId() != id) {
            g.timedText->SelectTrack(track->GetId(), FALSE);
        }
    }
    return SUCCEEDED(g.timedText->SelectTrack(id, TRUE));
}

SIZE VideoSize() { return DisplaySize(g.engine.Get()); }

void SetVideoSize(int width, int height) {
    AcquireSRWLockExclusive(&g.lock);
    if (g.painter) g.painter->DropTarget();  // resizing needs the buffers let go
    const HRESULT hr = SizeChain(g.chain, UINT(width), UINT(height));
    if (SUCCEEDED(hr)) {
        if (!g.chain2) g.chain.As(&g.chain2);
        g.sourceWidth = g.sourceHeight = 0;  // resizing set it to the whole size
        g.width = UINT(width);
        g.height = UINT(height);
        Redraw();  // at the new size, even while paused
    }
    ReleaseSRWLockExclusive(&g.lock);
}

void SetVideoTarget(int width, int height) {
    AcquireSRWLockExclusive(&g.lock);
    if (UINT(width) != g.drawWidth || UINT(height) != g.drawHeight) {
        g.drawWidth = UINT(std::max(width, 0));
        g.drawHeight = UINT(std::max(height, 0));
        Redraw();
    }
    ReleaseSRWLockExclusive(&g.lock);
}

IUnknown* VideoSwapChain() { return g.chain.Get(); }

void SetSubtitles(std::shared_ptr<const Subtitles> subs) {
    AcquireSRWLockExclusive(&g.subsLock);
    g.subs.swap(subs);
    ++g.subsVersion;
    ReleaseSRWLockExclusive(&g.subsLock);
    Redraw();  // even while paused
}

void SetSubtitleLayout(const SubtitleLayout& layout) {
    AcquireSRWLockExclusive(&g.subsLock);
    const bool changed = !(layout == g.subsLayout);
    if (changed) {
        g.subsLayout = layout;
        ++g.subsVersion;
    }
    ReleaseSRWLockExclusive(&g.subsLock);
    if (changed) Redraw();
}

bool IsPlaying() { return g.engine && !g.engine->IsPaused() && !g.engine->IsEnded(); }

void SetPlaying(bool playing) {
    if (!g.engine) return;
    if (playing) {
        g.engine->Play();  // from the start if it has ended
    } else {
        g.engine->Pause();
    }
}

double PlaybackTime() { return g.engine ? g.engine->GetCurrentTime() : 0; }

double Duration() { return g.engine ? g.engine->GetDuration() : NAN; }

// The engine's approximate seek still decodes from a key frame up to near the target, a frame at a time, so a fast
// seek goes exactly to a key frame instead (just past it, so rounding can't land a frame early).
double SeekLanding(double seconds, bool fast) {
    IMFSeekInfo* keyFrames = fast ? KeyFrames() : nullptr;
    return keyFrames ? NearestKeyFrame(keyFrames, seconds) + 0.0005 : seconds;
}

void SeekTo(double landing, bool fast) {
    g.landing = std::llround(landing * 1e7);
    g.engine->SetCurrentTimeEx(landing, fast && !KeyFrames() ? MF_MEDIA_ENGINE_SEEK_MODE_APPROXIMATE
                                                             : MF_MEDIA_ENGINE_SEEK_MODE_NORMAL);
}

double Volume() { return g.volume; }

bool IsMuted() { return g.muted; }

namespace {

void SetMuted(bool muted) {
    g.muted = muted;
    if (g.engine) g.engine->SetMuted(muted);
}

}  // namespace

double ChangeVolume(double delta) {
    g.volume = std::clamp(std::round((g.volume + delta) * 100) / 100, 0.0, 1.0);
    if (g.engine) g.engine->SetVolume(g.volume);
    if (delta > 0) SetMuted(false);
    return g.volume;
}

double SetVolume(double volume) {
    g.volume = std::clamp(std::round(volume * 100) / 100, 0.0, 1.0);
    if (g.engine) g.engine->SetVolume(g.volume);
    if (g.volume > 0) SetMuted(false);
    return g.volume;
}

bool ToggleMute() {
    SetMuted(!g.muted);
    return g.muted;
}
