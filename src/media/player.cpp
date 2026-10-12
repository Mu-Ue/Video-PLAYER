#include "media/player.h"

#undef GetCurrentTime  // a 16-bit era winbase.h macro that would rename IMFMediaEngine::GetCurrentTime

#include <audiosessiontypes.h>
#include <avrt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>

#include "config.h"
#include "media/subtitles.h"

using Microsoft::WRL::ComPtr;

namespace {

// Forwards engine events, which arrive on Media Foundation threads, to the window thread as msg. Lives for the
// whole process, so it needs no reference counting.
class Notify final : public IMFMediaEngineNotify {
  public:
    HWND hwnd = nullptr;

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
    // The seek bar preview: a thread that decodes just the key frame the window last asked for. The window hands
    // it a file to open or a key frame to show under previewLock, then sets previewWake.
    std::thread previewer;
    HANDLE previewWake = nullptr;
    SRWLOCK previewLock = SRWLOCK_INIT;
    std::wstring previewPath;
    double previewTarget = NAN;  // seconds
    double previewBucket = -1;   // the last span of kPreviewSpan seconds the window asked for
    // A second parser of the open file, which knows where its key frames are; null if the format doesn't say.
    ComPtr<IMFMediaSource> keySource;
    ComPtr<IMFSeekInfo> keyFrames;
    bool started = false;  // MFStartup succeeded
    // The swap chains, guarded by lock: the render thread presents into the video's and the window thread sizes it;
    // the previewer sizes and presents into the preview's.
    SRWLOCK lock = SRWLOCK_INIT;
    ComPtr<IDXGISwapChain1> chain, previewChain;
    UINT width = 0, height = 0, previewWidth = 0, previewHeight = 0;
    std::atomic<bool> redraw = false;  // present the current frame even if it isn't new
    HANDLE stop = nullptr;             // tells the render thread and the previewer to exit
    std::thread renderer;
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
    HRESULT hr = MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g.manager.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Movie);
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

// Copies the engine's current frame into the w x h swap chain and presents it.
bool PresentFrame(IMFMediaEngineEx* engine, IDXGISwapChain1* chain, UINT w, UINT h) {
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
    const RECT dest{0, 0, LONG(w), LONG(h)};
    const MFARGB black{0, 0, 0, 255};
    if (FAILED(engine->TransferVideoFrame(buffer.Get(), nullptr, &dest, &black))) return false;
    return SUCCEEDED(chain->Present(0, 0));
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

// Once per compositor frame, presents the engine's frame for that moment if it is a new one. Nothing is queued
// ahead, so pausing and seeking show on screen as soon as the engine has the frame.
void RenderLoop() {
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
    for (;;) {
        if (waitClock) {
            const DWORD r = waitClock(1, &g.stop, INFINITE);
            if (r == WAIT_OBJECT_0) break;
            if (r != WAIT_OBJECT_0 + 1 && WaitForSingleObject(g.stop, 16) != WAIT_TIMEOUT) break;
        } else if (!output || FAILED(output->WaitForVBlank())) {
            if (WaitForSingleObject(g.stop, 16) != WAIT_TIMEOUT) break;
        } else if (WaitForSingleObject(g.stop, 0) != WAIT_TIMEOUT) {
            break;
        }
        LONGLONG pts;
        if (g.engine->OnVideoStreamTick(&pts) == S_OK || g.redraw) {
            AcquireSRWLockExclusive(&g.lock);
            g.redraw = !g.chain || !PresentFrame(g.engine.Get(), g.chain.Get(), g.width, g.height);
            ReleaseSRWLockExclusive(&g.lock);
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void CloseKeyFrames() {
    g.keyFrames.Reset();
    if (g.keySource) g.keySource->Shutdown();
    g.keySource.Reset();
}

void OpenKeyFrames(const std::wstring& path) {
    CloseKeyFrames();
    ComPtr<IMFByteStream> stream;
    ComPtr<IMFSourceResolver> resolver;
    MF_OBJECT_TYPE type;
    ComPtr<IUnknown> object;
    ComPtr<IMFGetService> service;
    if (FAILED(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path.c_str(),
                            &stream)) ||
        FAILED(MFCreateSourceResolver(&resolver)) ||
        FAILED(resolver->CreateObjectFromByteStream(stream.Get(), path.c_str(),
                                                    MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ, nullptr, &type,
                                                    &object)) ||
        FAILED(object.As(&g.keySource))) {
        return;
    }
    if (FAILED(g.keySource.As(&service)) ||
        FAILED(service->GetService(MF_SCRUBBING_SERVICE, IID_PPV_ARGS(&g.keyFrames)))) {
        CloseKeyFrames();
    }
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

// The key frame nearest to a time, in seconds, or the time itself if unknown.
double NearestKeyFrame(double seconds) {
    if (!g.keyFrames) return seconds;
    PROPVARIANT start{}, before{}, after{};  // 100 ns units, which need no clearing
    start.vt = VT_I8;
    start.hVal.QuadPart = LONGLONG(seconds * 1e7);
    if (SUCCEEDED(g.keyFrames->GetNearestKeyFrames(&GUID_NULL, &start, &before, &after)) && before.vt == VT_I8 &&
        after.vt == VT_I8) {
        const double a = before.hVal.QuadPart / 1e7, b = after.hVal.QuadPart / 1e7;
        seconds = seconds - a <= b - seconds ? a : b;
    }
    return seconds;
}

constexpr DWORD kVideoStream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr double kPreviewSpan = 10;  // seconds of video each preview picture stands for

// A reader of the file's video alone, which decodes on the GPU and hands out each frame shrunk there to w x h, its
// display size fitted to 320 px, about the largest the preview shows; null if it can't.
ComPtr<IMFSourceReader> OpenPreviewReader(const std::wstring& path, UINT& w, UINT& h) {
    ComPtr<IMFByteStream> stream;
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> native, type;
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path.c_str(),
                              &stream);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g.manager.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    // The decoder hands out a key frame as soon as it has it, rather than holding it to reorder later frames.
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromByteStream(stream.Get(), attributes.Get(), &reader);
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
    hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);  // the swap chain's format
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(kVideoStream, nullptr, type.Get());
    return SUCCEEDED(hr) ? reader : nullptr;
}

// Seeks the reader to the key frame at or before a time and presents it in the preview's swap chain, unless it is
// the one shown already. Only that frame is decoded.
void ShowKeyFrame(IMFSourceReader* reader, double seconds, LONGLONG& shown) {
    PROPVARIANT position{};  // needs no clearing
    position.vt = VT_I8;
    position.hVal.QuadPart = std::llround(seconds * 1e7);
    if (FAILED(reader->SetCurrentPosition(GUID_NULL, position))) return;
    ComPtr<IMFSample> sample;
    DWORD flags = 0;
    LONGLONG time = 0;
    for (int i = 0; i < 8 && !sample; ++i) {  // a gap in the stream comes as no sample
        if (FAILED(reader->ReadSample(kVideoStream, 0, nullptr, &flags, &time, &sample)) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            return;
        }
    }
    if (!sample || time == shown) return;
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgi;
    ComPtr<ID3D11Texture2D> texture;
    UINT index = 0;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgi)) ||
        FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture))) || FAILED(dxgi->GetSubresourceIndex(&index))) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return;
    ComPtr<ID3D11DeviceContext> context;
    g.device->GetImmediateContext(&context);
    AcquireSRWLockExclusive(&g.lock);
    ComPtr<ID3D11Texture2D> target;
    if (g.previewChain && SUCCEEDED(g.previewChain->GetBuffer(0, IID_PPV_ARGS(&target)))) {
        const D3D11_BOX box{0, 0, 0, std::min(desc.Width, g.previewWidth), std::min(desc.Height, g.previewHeight), 1};
        context->CopySubresourceRegion(target.Get(), 0, 0, 0, 0, texture.Get(), index, &box);
        target.Reset();
        if (SUCCEEDED(g.previewChain->Present(0, 0))) shown = time;
    }
    ReleaseSRWLockExclusive(&g.lock);
}

// Opens the files and shows the key frames the window asks for, one at a time. Asks made while it decodes replace
// each other, so only the latest is shown next and nothing queues up behind the mouse.
void PreviewLoop() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ComPtr<IMFSourceReader> reader;
    LONGLONG shown = -1;
    const HANDLE events[] = {g.stop, g.previewWake};
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        AcquireSRWLockExclusive(&g.previewLock);
        const std::wstring path = std::exchange(g.previewPath, {});
        const double target = std::exchange(g.previewTarget, NAN);
        ReleaseSRWLockExclusive(&g.previewLock);
        if (!path.empty()) {
            UINT w = 0, h = 0;
            reader = OpenPreviewReader(path, w, h);
            shown = -1;
            AcquireSRWLockExclusive(&g.lock);
            g.previewWidth = g.previewHeight = 0;
            if (reader && SUCCEEDED(SizeChain(g.previewChain, w, h))) {
                g.previewWidth = w;
                g.previewHeight = h;
            } else {  // audio only, or a format it can't shrink: no previews
                reader.Reset();
            }
            ReleaseSRWLockExclusive(&g.lock);
        }
        if (reader && !std::isnan(target)) ShowKeyFrame(reader.Get(), target, shown);
    }
    reader.Reset();
    if (com) CoUninitialize();
}

}  // namespace

bool InitPlayer(HWND hwnd) {
    g.notify.hwnd = g.textNotify.hwnd = hwnd;
    if (FAILED(CreateDevice())) return false;
    g.started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!g.started || FAILED(CreateEngines())) return false;
    g.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.previewWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.stop || !g.previewWake) return false;
    g.renderer = std::thread(RenderLoop);
    g.previewer = std::thread(PreviewLoop);
    return true;
}

void ShutdownPlayer() {
    if (g.stop) SetEvent(g.stop);
    if (g.renderer.joinable()) g.renderer.join();
    if (g.previewer.joinable()) g.previewer.join();
    g.chain.Reset();
    g.previewChain.Reset();
    CloseKeyFrames();
    g.timedText.Reset();
    if (g.engine) g.engine->Shutdown();
    g.engine.Reset();
    g.manager.Reset();
    if (g.started) MFShutdown();
    g.started = false;
}

HRESULT OpenVideo(const std::wstring& path) {
    // A byte stream keeps the source a local file: the path is never parsed as a URL.
    ComPtr<IMFByteStream> stream;
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path.c_str(),
                              &stream);
    if (FAILED(hr)) return hr;
    BSTR name = SysAllocString(path.c_str());  // the extension helps pick the container parser
    if (!name) return E_OUTOFMEMORY;
    hr = g.engine->SetSourceFromByteStream(stream.Get(), name);
    SysFreeString(name);
    g.redraw = true;  // the first frame, even if it never plays
    if (SUCCEEDED(hr)) hr = g.engine->Play();
    if (FAILED(hr)) return hr;
    OpenKeyFrames(path);
    // The previewer reads the file on its own; if it can't, there are just no previews.
    AcquireSRWLockExclusive(&g.previewLock);
    g.previewPath = path;
    g.previewTarget = NAN;
    ReleaseSRWLockExclusive(&g.previewLock);
    g.previewBucket = -1;
    SetEvent(g.previewWake);
    return hr;
}

void SeekPreview(double seconds) {
    // One picture for each 10 seconds, like Netflix: moves within them decode nothing new.
    const double bucket = std::floor(seconds / kPreviewSpan);
    if (bucket == g.previewBucket) return;
    g.previewBucket = bucket;
    // The key frame nearest the middle, which needs no decoding up to it.
    AcquireSRWLockExclusive(&g.previewLock);
    g.previewTarget = NearestKeyFrame((bucket + 0.5) * kPreviewSpan);
    ReleaseSRWLockExclusive(&g.previewLock);
    SetEvent(g.previewWake);
}

IUnknown* PreviewSwapChain() {
    AcquireSRWLockShared(&g.lock);
    IUnknown* chain = g.previewChain.Get();
    ReleaseSRWLockShared(&g.lock);
    return chain;
}

SIZE PreviewSize() {
    AcquireSRWLockShared(&g.lock);
    const SIZE size{LONG(g.previewWidth), LONG(g.previewHeight)};
    ReleaseSRWLockShared(&g.lock);
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
    const HRESULT hr = SizeChain(g.chain, UINT(width), UINT(height));
    if (SUCCEEDED(hr)) {
        g.width = UINT(width);
        g.height = UINT(height);
        g.redraw = true;  // at the new size, even while paused
    }
    ReleaseSRWLockExclusive(&g.lock);
}

IUnknown* VideoSwapChain() { return g.chain.Get(); }

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

void SeekTo(double seconds, bool fast) {
    // The engine's approximate seek still decodes from a key frame up to near the target, a frame at a time,
    // so a fast seek goes exactly to a key frame instead (just past it, so rounding can't land a frame early).
    if (fast && g.keyFrames) {
        g.engine->SetCurrentTimeEx(NearestKeyFrame(seconds) + 0.0005, MF_MEDIA_ENGINE_SEEK_MODE_NORMAL);
        return;
    }
    g.engine->SetCurrentTimeEx(seconds,
                               fast ? MF_MEDIA_ENGINE_SEEK_MODE_APPROXIMATE : MF_MEDIA_ENGINE_SEEK_MODE_NORMAL);
}

double Volume() { return g.engine ? g.engine->GetVolume() : 1; }

bool IsMuted() { return g.engine && g.engine->GetMuted(); }

double ChangeVolume(double delta) {
    const double volume = std::clamp(std::round((g.engine->GetVolume() + delta) * 100) / 100, 0.0, 1.0);
    g.engine->SetVolume(volume);
    if (delta > 0) g.engine->SetMuted(FALSE);
    return volume;
}

double SetVolume(double volume) {
    volume = std::clamp(std::round(volume * 100) / 100, 0.0, 1.0);
    g.engine->SetVolume(volume);
    if (volume > 0) g.engine->SetMuted(FALSE);
    return volume;
}

bool ToggleMute() {
    const bool muted = !g.engine->GetMuted();
    g.engine->SetMuted(muted);
    return muted;
}
