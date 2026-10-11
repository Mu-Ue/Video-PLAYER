#include "media/player.h"

#undef GetCurrentTime  // a 16-bit era winbase.h macro that would rename IMFMediaEngine::GetCurrentTime

#include <audiosessiontypes.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

// Forwards engine events, which arrive on Media Foundation threads, to the window thread. Lives for the
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

struct {
    Notify notify;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D10Multithread> multithread;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<IMFMediaEngineEx> engine;
    bool started = false;  // MFStartup succeeded
    // The video's swap chain, guarded by lock: the render thread presents into it, the window thread sizes it.
    SRWLOCK lock = SRWLOCK_INIT;
    ComPtr<IDXGISwapChain1> chain;
    UINT width = 0, height = 0;
    std::atomic<bool> redraw = false;  // present the current frame even if it isn't new
    HANDLE stop = nullptr;              // tells the render thread to exit
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
    return hr;
}

HRESULT CreateEngine() {
    UINT token = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&token, &g.manager);
    if (SUCCEEDED(hr)) hr = g.manager->ResetDevice(g.device.Get(), token);
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, &g.notify);
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g.manager.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Movie);
    ComPtr<IMFMediaEngineClassFactory> factory;
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    ComPtr<IMFMediaEngine> engine;
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attributes.Get(), &engine);
    if (SUCCEEDED(hr)) hr = engine.As(&g.engine);
    return hr;
}

// Copies the engine's current frame into the swap chain and presents it.
bool PresentFrame() {
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(g.chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
    const RECT dest{0, 0, LONG(g.width), LONG(g.height)};
    const MFARGB black{0, 0, 0, 255};
    if (FAILED(g.engine->TransferVideoFrame(buffer.Get(), nullptr, &dest, &black))) return false;
    return SUCCEEDED(g.chain->Present(0, 0));
}

// Once per compositor frame, presents the engine's frame for that moment if it is a new one. Nothing is queued
// ahead, so pausing and seeking show on screen as soon as the engine has the frame.
void RenderLoop() {
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
        AcquireSRWLockExclusive(&g.lock);
        LONGLONG pts;
        if (g.chain && (g.engine->OnVideoStreamTick(&pts) == S_OK || g.redraw)) g.redraw = !PresentFrame();
        ReleaseSRWLockExclusive(&g.lock);
    }
}

}  // namespace

bool InitPlayer(HWND hwnd) {
    g.notify.hwnd = hwnd;
    if (FAILED(CreateDevice())) return false;
    g.started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!g.started || FAILED(CreateEngine())) return false;
    g.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g.stop) return false;
    g.renderer = std::thread(RenderLoop);
    return true;
}

void ShutdownPlayer() {
    if (g.renderer.joinable()) {
        SetEvent(g.stop);
        g.renderer.join();
    }
    g.chain.Reset();
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
    return hr;
}

bool HasVideo() { return g.engine && g.engine->HasVideo(); }

SIZE VideoSize() {
    DWORD w = 0, h = 0;
    if (!HasVideo() || FAILED(g.engine->GetNativeVideoSize(&w, &h)) || !w || !h) return {};
    // The picture aspect ratio differs from w:h for non-square pixels (anamorphic video): stretch the width.
    DWORD ax = 0, ay = 0;
    if (SUCCEEDED(g.engine->GetVideoAspectRatio(&ax, &ay)) && ax && ay && UINT64(ax) * h != UINT64(ay) * w) {
        w = DWORD(std::lround(double(h) * ax / ay));
    }
    return {LONG(w), LONG(h)};
}

void SetVideoSize(int width, int height) {
    AcquireSRWLockExclusive(&g.lock);
    HRESULT hr;
    if (g.chain) {
        hr = g.chain->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN, 0);
    } else {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        hr = g.device.As(&dxgi);
        if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = UINT(width);
        desc.Height = UINT(height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForComposition(g.device.Get(), &desc, nullptr, &g.chain);
    }
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

bool ToggleMute() {
    const bool muted = !g.engine->GetMuted();
    g.engine->SetMuted(muted);
    return muted;
}
