// Plays a bench clip in the player and measures it from the outside, with no instrumentation in the player:
//
// - Every composition of the desktop, through DXGI Desktop Duplication: the frame number the clip carries as 16
//   bits and their inverse, whether a subtitle shows, and whether the controls cover the bottom of the video.
//   That gives the time to the first frame, cadence (each frame's time on screen against the clip's timeline),
//   dropped frames, subtitle exactness against the SRT's start <= pts < end, and seek latency.
// - The audio mix through WASAPI loopback: each click's onset against its flash frame's first composition.
// - The player process: CPU time, reads, memory, GPU memory, and context switches of all threads and the
//   busiest one (wakeups).
//
// Usage: bench <player.exe> <clip> [--seconds N] [--seeks N] [--subs] [--cores N] [--label text] [--csv file] [--dump file]
// The clip's .srt must sit next to it, as make_clips.ps1 writes it. Appends one row to the CSV (bench.csv by
// default) and prints it.

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mmdeviceapi.h>
#include <pdh.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double Freq() {
    static const double f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return double(q.QuadPart);
    }();
    return f;
}

double Ms(LONGLONG ticks) { return double(ticks) * 1000 / Freq(); }

[[noreturn]] void Fail(const char* what) {
    fprintf(stderr, "bench: %s\n", what);
    exit(1);
}

// --- The clip ---

struct Clip {
    double fps = 0;  // frames per second, from ffprobe-free parsing of the name: r23976_..., r24_...
    int width = 0, height = 0;
    int flashEvery = 0;  // frames between flashes, as make_clips.ps1 sets it
    std::vector<std::pair<double, double>> cues;  // start, end in seconds
    std::vector<double> pts;  // each frame's, as the container stores it
    std::vector<bool> kept;   // of a variable rate clip (v60_...), which 60 fps frames it has; empty otherwise
};

double ParseSrtTime(const std::string& s) {
    int h, m, sec, ms;
    return sscanf_s(s.c_str(), "%d:%d:%d,%d", &h, &m, &sec, &ms) == 4 ? h * 3600 + m * 60 + sec + ms / 1000.0 : NAN;
}

Clip LoadClip(const std::wstring& path) {
    Clip c;
    const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    if (name.rfind(L"r23976_", 0) == 0) c.fps = 24000.0 / 1001;
    else if (name.rfind(L"r24_", 0) == 0) c.fps = 24;
    else if (name.rfind(L"r30_", 0) == 0) c.fps = 30;
    else if (name.rfind(L"r60_", 0) == 0 || name.rfind(L"v60_", 0) == 0) c.fps = 60;
    else Fail("clip name must start with its rate, as make_clips.ps1 names it");
    c.width = name.find(L"_2160_") != std::wstring::npos ? 3840 : 1920;
    c.height = c.width * 9 / 16;
    c.flashEvery = int(std::lround(2 * c.fps));
    const std::wstring stem = path.substr(0, path.find_last_of(L'.'));
    std::ifstream ptsFile(stem + L".pts");
    for (double t; ptsFile >> t;) c.pts.push_back(t);
    if (name.rfind(L"v60_", 0) == 0) {  // its frames carry the numbers of the 60 fps frames it kept
        for (const double t : c.pts) {
            const size_t n = size_t(std::lround(t * c.fps));
            if (c.kept.size() <= n) c.kept.resize(n + 1);
            c.kept[n] = true;
        }
        c.pts.clear();
    }
    std::ifstream srt(stem + L".srt");
    for (std::string line; std::getline(srt, line);) {
        const size_t arrow = line.find(" --> ");
        if (arrow != std::string::npos) c.cues.push_back({ParseSrtTime(line.substr(0, arrow)), ParseSrtTime(line.substr(arrow + 5))});
    }
    return c;
}

// The burned-in truth: frame n shows a cue if start <= pts < end.
bool CueShows(const Clip& c, int n) {
    const double pts = size_t(n) < c.pts.size() ? c.pts[size_t(n)] : n / c.fps;
    return std::any_of(c.cues.begin(), c.cues.end(), [&](const auto& q) { return q.first <= pts && pts < q.second; });
}

// --- The player's window ---

struct FindWindow {
    DWORD pid;
    HWND hwnd = nullptr;
};

HWND PlayerWindow(DWORD pid) {
    FindWindow f{pid};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto& f = *reinterpret_cast<FindWindow*>(p);
            DWORD owner = 0;
            wchar_t cls[32];
            GetWindowThreadProcessId(h, &owner);
            if (owner == f.pid && IsWindowVisible(h) && GetClassNameW(h, cls, 32) && wcscmp(cls, L"VideoPLAYER") == 0) {
                f.hwnd = h;
                return FALSE;
            }
            return TRUE;
        },
        LPARAM(&f));
    return f.hwnd;
}

void BringToFront(HWND hwnd) {
    // An Alt press lets this process move the foreground (see the screenshot foreground trap).
    INPUT alt[2]{};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(hwnd);
}

// Keys go straight to the window, so they never depend on the foreground.
void PostKey(HWND hwnd, UINT key) {
    PostMessageW(hwnd, WM_KEYDOWN, key, 1);
    PostMessageW(hwnd, WM_KEYUP, key, LPARAM(0xC0000001));
}

// --- Desktop Duplication ---

struct Sample {
    LONGLONG time;  // QPC of the composition
    int frame;      // -1 if the bits didn't decode
    bool subtitle;  // white text in the subtitle area
    bool covered;   // the controls darken the bottom of the video
    BYTE bits[32];  // each bit's luma and its inverse's, for --dump
};

class Capture {
  public:
    // Duplicates the output showing the window's client area and finds the video in it, whose aspect is aspect.
    bool Open(HWND hwnd, double aspect) {
        hwnd_ = hwnd;
        aspect_ = aspect;
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; !output_ && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
                DXGI_OUTPUT_DESC desc;
                output->GetDesc(&desc);
                if (desc.Monitor != monitor) continue;
                origin_ = {desc.DesktopCoordinates.left, desc.DesktopCoordinates.top};
                output.As(&output_);
                if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &device_, nullptr, &context_))) {
                    return false;
                }
                break;
            }
        }
        return output_ && SUCCEEDED(output_->DuplicateOutput(device_.Get(), &dup_));
    }

    // Records compositions until stop, calling back with each one.
    template <class F>
    void Run(const std::atomic<bool>& stop, F&& onSample) {
        while (!stop) {
            DXGI_OUTDUPL_FRAME_INFO info;
            ComPtr<IDXGIResource> resource;
            const HRESULT hr = dup_->AcquireNextFrame(100, &info, &resource);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (hr == DXGI_ERROR_ACCESS_LOST) {  // a mode change or a secure desktop: duplicate again
                dup_.Reset();
                if (FAILED(output_->DuplicateOutput(device_.Get(), &dup_))) return;
                continue;
            }
            if (FAILED(hr)) return;
            if (info.LastPresentTime.QuadPart) {
                ComPtr<ID3D11Texture2D> desktop;
                if (SUCCEEDED(resource.As(&desktop))) {
                    Sample s{info.LastPresentTime.QuadPart};
                    if (Read(desktop.Get(), s)) onSample(s);
                    if (snapAt && s.time >= snapAt) {
                        Snap(desktop.Get());
                        snapAt = 0;
                    }
                }
            }
            dup_->ReleaseFrame();
        }
    }

    LONGLONG snapAt = 0;  // QPC time of the composition --snap saves, or 0
    std::wstring snapPath;

  private:
    // Saves the window's client area as a top-down 32-bit BMP.
    void Snap(ID3D11Texture2D* desktop) {
        const int w = client_.right - client_.left, h = client_.bottom - client_.top;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(w);
        d.Height = UINT(h);
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &t))) return;
        const D3D11_BOX box{UINT(client_.left - origin_.x), UINT(client_.top - origin_.y), 0,
                            UINT(client_.right - origin_.x), UINT(client_.bottom - origin_.y), 1};
        context_->CopySubresourceRegion(t.Get(), 0, 0, 0, 0, desktop, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(context_->Map(t.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        BITMAPINFOHEADER bi{sizeof(bi), w, -h, 1, 32, BI_RGB};
        BITMAPFILEHEADER bf{0x4D42, DWORD(sizeof(bf) + sizeof(bi) + size_t(w) * h * 4), 0, 0, DWORD(sizeof(bf) + sizeof(bi))};
        std::ofstream f(snapPath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(&bf), sizeof(bf));
        f.write(reinterpret_cast<const char*>(&bi), sizeof(bi));
        for (int y = 0; y < h; ++y) f.write(static_cast<const char*>(m.pData) + size_t(y) * m.RowPitch, std::streamsize(w) * 4);
        context_->Unmap(t.Get(), 0);
    }

    // The video's rect within the desktop texture, found from the window's client area and the video's aspect:
    // the player fits it below the menu bar, centered. Recomputed when the client area moves or resizes.
    bool Locate() {
        RECT client;
        if (!GetClientRect(hwnd_, &client)) return false;
        POINT tl{0, 0};
        ClientToScreen(hwnd_, &tl);
        const RECT now{tl.x, tl.y, tl.x + client.right, tl.y + client.bottom};
        if (EqualRect(&now, &client_) && staging_) return true;
        client_ = now;
        // The menu bar is 26 px at 96 DPI; none in fullscreen, where the client covers the monitor.
        const UINT dpi = GetDpiForWindow(hwnd_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        const bool full = EqualRect(&mi.rcMonitor, &now);
        const int bar = full ? 0 : MulDiv(26, int(dpi), 96);
        const int cw = client.right, ah = client.bottom - bar;
        if (cw <= 0 || ah <= 0) return false;
        const double scale = std::min(cw / aspect_, double(ah)) ;
        const int vh = int(std::lround(scale)), vw = int(std::lround(scale * aspect_));
        video_.left = now.left - origin_.x + (cw - vw) / 2;
        video_.top = now.top - origin_.y + bar + (ah - vh) / 2;
        video_.right = video_.left + vw;
        video_.bottom = video_.top + vh;
        // The staging texture holds the two bit rows, then the bottom fifth of the video.
        subRows_ = vh / 5;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(vw);
        d.Height = UINT(2 + subRows_);
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_.Reset();
        return SUCCEEDED(device_->CreateTexture2D(&d, nullptr, &staging_));
    }

    bool Read(ID3D11Texture2D* desktop, Sample& s) {
        if (!Locate()) return false;
        const int vw = video_.right - video_.left, vh = video_.bottom - video_.top;
        // The bit rows at the middle of their bands: 17.5% and 22.5% of the height.
        const int rows[2] = {video_.top + int(vh * 0.175), video_.top + int(vh * 0.225)};
        for (int i = 0; i < 2; ++i) {
            const D3D11_BOX box{UINT(video_.left), UINT(rows[i]), 0, UINT(video_.right), UINT(rows[i] + 1), 1};
            context_->CopySubresourceRegion(staging_.Get(), 0, 0, UINT(i), 0, desktop, 0, &box);
        }
        const D3D11_BOX sub{UINT(video_.left), UINT(video_.bottom - subRows_), 0, UINT(video_.right),
                            UINT(video_.bottom), 1};
        context_->CopySubresourceRegion(staging_.Get(), 0, 0, 2, 0, desktop, 0, &sub);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        const auto pixel = [&](int x, int y) { return static_cast<const BYTE*>(m.pData) + size_t(y) * m.RowPitch + size_t(x) * 4; };
        const auto luma = [&](int x, int y) {
            const BYTE* p = pixel(x, y);
            return (p[0] + 2 * p[1] + p[2]) / 4;
        };
        // Each bit's cell center; a bit and its inverse must disagree, or the capture caught a blend.
        int frame = 0;
        bool valid = true;
        for (int b = 0; b < 16 && valid; ++b) {
            const int x = int((b + 0.5) * vw / 16);
            s.bits[2 * b] = BYTE(luma(x, 0));
            s.bits[2 * b + 1] = BYTE(luma(x, 1));
            const bool one = luma(x, 0) > 128, inverse = luma(x, 1) > 128;
            valid = one != inverse;
            if (one) frame |= 1 << b;
        }
        s.frame = valid ? frame : -1;
        // The gray is 96; the subtitle's fill is white. The controls' gradient darkens the bottom row.
        int bright = 0;
        for (int y = 2; y < 2 + subRows_; y += 2) {
            for (int x = 0; x < vw; x += 2) bright += luma(x, y) > 220;
        }
        s.subtitle = bright > 20;
        int dark = 0;
        for (int x = vw / 20; x < vw; x += vw / 10) dark += luma(x, 2 + subRows_ - 4) < 70;
        s.covered = dark >= 5;
        context_->Unmap(staging_.Get(), 0);
        return true;
    }

    HWND hwnd_ = nullptr;
    double aspect_ = 16.0 / 9;
    POINT origin_{};
    RECT client_{}, video_{};
    int subRows_ = 0;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> staging_;
};

// --- Loopback audio ---

// Hands over the result of ActivateAudioInterfaceAsync by setting an event. Lives on the stack of its caller.
class Activated final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
  public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Activated() { CloseHandle(done); }
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject*>(this);
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done);
        return S_OK;
    }
};

// Records the QPC time of each click's onset in the player's own audio, and only its, until stop, so other sound
// on the PC can't add or hide clicks.
void RecordClicks(DWORD pid, const std::atomic<bool>& stop, std::vector<LONGLONG>& onsets, std::atomic<bool>& ready) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    AUDIOCLIENT_ACTIVATION_PARAMS params{AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK};
    params.ProcessLoopbackParams = {pid, PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE};
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob = {sizeof(params), reinterpret_cast<BYTE*>(&params)};
    Activated handler;
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    ComPtr<IUnknown> unknown;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HRESULT result = E_FAIL;
    constexpr UINT channels = 2, rate = 48000;
    WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, channels, rate, rate * channels * 4, channels * 4, 32, 0};
    const bool ok = SUCCEEDED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                                          &activation, &handler, &op)) &&
                    WaitForSingleObject(handler.done, INFINITE) == WAIT_OBJECT_0 &&
                    SUCCEEDED(op->GetActivateResult(&result, &unknown)) && SUCCEEDED(result) &&
                    SUCCEEDED(unknown.As(&client)) &&
                    SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                                 2'000'000, 0, &format, nullptr)) &&
                    SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
    ready = true;
    if (!ok) {
        fprintf(stderr, "bench: can't capture the player's audio; no A/V offset\n");
        return;
    }
    LONGLONG lastOnset = LLONG_MIN / 2;
    const double qpcPer100ns = Freq() / 1e7;
    while (!stop) {
        Sleep(2);
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
            BYTE* data;
            UINT32 frames;
            DWORD flags;
            UINT64 position, qpc;  // qpc in 100 ns units
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, &position, &qpc))) break;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const float* f = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    if (std::abs(f[size_t(i) * channels]) < 0.05f) continue;
                    const LONGLONG t = LONGLONG((double(qpc) + i * 1e7 / rate) * qpcPer100ns);
                    if (Ms(t - lastOnset) > 500) onsets.push_back(t);
                    lastOnset = t;  // within a click
                }
            }
            capture->ReleaseBuffer(frames);
        }
    }
    client->Stop();
}

// --- The player process ---

// The parts of SYSTEM_PROCESS_INFORMATION and SYSTEM_THREAD_INFORMATION read here, laid out as on x64.
struct ThreadInfo {
    LARGE_INTEGER kernel, user, create;
    ULONG waitTime;
    PVOID start;
    HANDLE clientPid, clientTid;
    LONG priority, basePriority;
    ULONG contextSwitches, state, waitReason;
};
static_assert(sizeof(ThreadInfo) == 0x50);
struct ProcessInfo {
    ULONG next, threads;
    BYTE skip1[0x48];
    HANDLE pid;
    BYTE skip2[0x100 - 0x58];
};
static_assert(sizeof(ProcessInfo) == 0x100);

// Context switches of all the process's threads, and of its busiest.
void ContextSwitches(DWORD pid, ULONGLONG& total, ULONG& busiest) {
    using Query = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    std::vector<BYTE> buffer(1 << 20);
    ULONG needed = 0;
    while (query(5 /* SystemProcessInformation */, buffer.data(), ULONG(buffer.size()), &needed) == LONG(0xC0000004)) {
        buffer.resize(needed + (1 << 16));
    }
    total = busiest = 0;
    for (size_t at = 0;;) {
        const auto* p = reinterpret_cast<const ProcessInfo*>(buffer.data() + at);
        if (DWORD(ULONG_PTR(p->pid)) == pid) {
            const auto* t = reinterpret_cast<const ThreadInfo*>(p + 1);
            for (ULONG i = 0; i < p->threads; ++i) {
                total += t[i].contextSwitches;
                busiest = std::max(busiest, t[i].contextSwitches);
            }
            return;
        }
        if (!p->next) return;
        at += p->next;
    }
}

// The process's GPU memory in MB, dedicated and shared, from the GPU Process Memory counters.
void GpuMemory(DWORD pid, double& dedicated, double& shared) {
    dedicated = shared = NAN;
    PDH_HQUERY q;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    const auto sum = [&](const wchar_t* counter) {
        wchar_t path[256];
        swprintf_s(path, L"\\GPU Process Memory(pid_%lu_*)\\%s", pid, counter);
        DWORD size = 0;
        PdhExpandWildCardPathW(nullptr, path, nullptr, &size, 0);
        std::vector<wchar_t> list(size + 2);
        if (PdhExpandWildCardPathW(nullptr, path, list.data(), &size, 0) != ERROR_SUCCESS) return double(NAN);
        std::vector<PDH_HCOUNTER> counters;
        for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1) {
            PDH_HCOUNTER c;
            if (PdhAddEnglishCounterW(q, p, 0, &c) == ERROR_SUCCESS) counters.push_back(c);
        }
        if (PdhCollectQueryData(q) != ERROR_SUCCESS) return double(NAN);
        double total = 0;
        for (PDH_HCOUNTER c : counters) {
            PDH_FMT_COUNTERVALUE v;
            if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) total += v.doubleValue;
        }
        return total / (1 << 20);
    };
    dedicated = sum(L"Dedicated Usage");
    shared = sum(L"Shared Usage");
    PdhCloseQuery(q);
}

std::string Narrow(const std::wstring& w) {
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), int(s.size()), nullptr, nullptr);
    s.pop_back();
    return s;
}

double Mean(const std::vector<double>& v) { return v.empty() ? NAN : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size()); }

double Sd(const std::vector<double>& v) {
    if (v.size() < 2) return NAN;
    const double m = Mean(v);
    double s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / double(v.size() - 1));
}

double Max(const std::vector<double>& v) { return v.empty() ? NAN : *std::max_element(v.begin(), v.end()); }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 3) Fail("usage: bench <player.exe> <clip> [--seconds N] [--seeks N] [--subs] [--cores N] [--label text] [--csv file] [--dump file]");
    const std::wstring player = argv[1], clipPath = argv[2];
    double seconds = 20;
    int seeks = 0, cores = 0;
    bool subs = false;
    std::wstring label = L"run", csv = L"bench.csv", dump, snapPath;
    double snapMs = 0;
    for (int i = 3; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--seconds" && more) seconds = _wtof(argv[++i]);
        else if (a == L"--seeks" && more) seeks = _wtoi(argv[++i]);
        else if (a == L"--cores" && more) cores = _wtoi(argv[++i]);
        else if (a == L"--label" && more) label = argv[++i];
        else if (a == L"--csv" && more) csv = argv[++i];
        else if (a == L"--dump" && more) dump = argv[++i];
        else if (a == L"--snap" && i + 2 < argc) {  // --snap <ms> <file.bmp>
            snapMs = _wtof(argv[++i]);
            snapPath = argv[++i];
        }
        else if (a == L"--subs") subs = true;
        else Fail("unknown argument");
    }
    const Clip clip = LoadClip(clipPath);
    if (subs && (clip.cues.empty() || clip.pts.empty())) Fail("--subs needs the clip's .srt and .pts next to it");

    // The cursor rests right of the window's middle, off it and off the controls, so they hide while playing.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetCursorPos(mi.rcWork.right - 2, (mi.rcWork.top + mi.rcWork.bottom) / 2);

    std::atomic<bool> stop = false, audioReady = false;
    std::vector<LONGLONG> clicks;

    std::wstring command = L"\"" + player + L"\" \"" + clipPath + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi;

    if (!CreateProcessW(player.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr,
                        &si, &pi)) {
        Fail("can't start the player");
    }
    if (cores > 0) SetProcessAffinityMask(pi.hProcess, (DWORD_PTR(1) << cores) - 1);
    std::thread audio(RecordClicks, pi.dwProcessId, std::cref(stop), std::ref(clicks), std::ref(audioReady));
    while (!audioReady) Sleep(1);
    const LONGLONG launched = Now();  // after the audio capture attaches, which mustn't count
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    HWND hwnd = nullptr;
    for (int i = 0; i < 1000 && !(hwnd = PlayerWindow(pi.dwProcessId)); ++i) {
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) Fail("the player exited");
    }
    if (!hwnd) Fail("no player window");
    // Topmost, so no other window that takes the foreground mid-run covers it.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    for (int i = 0; i < 20 && GetForegroundWindow() != hwnd; ++i) {
        BringToFront(hwnd);
        Sleep(50);
    }
    if (GetForegroundWindow() != hwnd) fprintf(stderr, "bench: the player isn't in front; captures may be of other windows\n");

    Capture capture;
    if (!capture.Open(hwnd, double(clip.width) / clip.height)) Fail("can't duplicate the window's output");
    if (snapMs > 0) {
        capture.snapPath = snapPath;
        capture.snapAt = launched + LONGLONG(snapMs / 1000 * Freq());
    }
    std::vector<Sample> samples;
    std::mutex samplesLock;
    std::thread recorder([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        capture.Run(stop, [&](const Sample& s) {
            std::lock_guard l(samplesLock);
            samples.push_back(s);
        });
    });

    // Subtitles on once the window shows; seeks spread over the run's second half, after the start settles.
    Sleep(500);
    if (subs) PostKey(hwnd, 'S');
    std::vector<LONGLONG> seekTimes;
    const double start = 4, end = seconds - 1;
    Sleep(DWORD(start * 1000) - 500);
    for (int i = 0; i < seeks; ++i) {
        const LONGLONG due = launched + LONGLONG((start + (end - start) * (i + 0.5) / seeks) * Freq());
        while (Now() < due) Sleep(1);
        seekTimes.push_back(Now());
        PostKey(hwnd, VK_RIGHT);
    }
    while (Ms(Now() - launched) < seconds * 1000) Sleep(10);

    // Process counters before closing it.
    FILETIME created, exited, kernel, user;
    GetProcessTimes(pi.hProcess, &created, &exited, &kernel, &user);
    const double cpuMs = (ULARGE_INTEGER{kernel.dwLowDateTime, kernel.dwHighDateTime}.QuadPart +
                          ULARGE_INTEGER{user.dwLowDateTime, user.dwHighDateTime}.QuadPart) / 1e4;
    IO_COUNTERS io{};
    GetProcessIoCounters(pi.hProcess, &io);
    PROCESS_MEMORY_COUNTERS_EX mem{sizeof(mem)};
    GetProcessMemoryInfo(pi.hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
    ULONGLONG switches = 0;
    ULONG busiest = 0;
    ContextSwitches(pi.dwProcessId, switches, busiest);
    double gpuDedicated, gpuShared;
    GpuMemory(pi.dwProcessId, gpuDedicated, gpuShared);

    stop = true;
    recorder.join();
    audio.join();
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);

    if (!dump.empty()) {  // each composition, for a closer look
        std::ofstream d(dump);
        d << "ms,frame,subtitle,covered,truth\n";
        for (const Sample& s : samples) {
            d << Ms(s.time - launched) << ',' << s.frame << ',' << s.subtitle << ',' << s.covered << ','
              << (s.frame >= 0 && CueShows(clip, s.frame));
            if (s.frame < 0) {
                for (BYTE b : s.bits) d << ',' << int(b);
            }
            d << '\n';
        }
        for (LONGLONG k : seekTimes) d << "seek," << Ms(k - launched) << '\n';
        for (LONGLONG k : clicks) d << "click," << Ms(k - launched) << '\n';
    }

    // --- Analysis ---
    // Changes: the compositions where a new frame number first shows.
    struct Change {
        LONGLONG time;
        int frame;
        size_t sample;
    };
    std::vector<Change> changes;
    int invalid = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const Sample& s = samples[i];
        if (s.frame < 0) {
            ++invalid;
            continue;
        }
        if (changes.empty() || changes.back().frame != s.frame) changes.push_back({s.time, s.frame, i});
    }
    const double ttff = changes.empty() ? NAN : Ms(changes.front().time - launched);

    // Seek landings: the first change after each key press whose frame jumps forward by more than a second.
    std::vector<double> seekMs;
    std::vector<bool> afterSeek(changes.size());  // changes within a second after a landing, kept out of cadence
    for (LONGLONG k : seekTimes) {
        int before = -1;
        for (size_t i = 0; i < changes.size(); ++i) {
            if (changes[i].time <= k) {
                before = changes[i].frame;
                continue;
            }
            if (before >= 0 && changes[i].frame > before + clip.fps) {
                seekMs.push_back(Ms(changes[i].time - k));
                for (size_t j = i; j < changes.size() && Ms(changes[j].time - changes[i].time) < 1000; ++j) afterSeek[j] = true;
                break;
            }
        }
    }

    // Cadence, over steady playback from 3 s in: each change's time against the clip's timeline, fitted by its
    // median offset, and frames skipped.
    std::vector<double> residual;
    int dropped = 0, steady = 0;
    {
        std::vector<std::pair<double, double>> points;  // seconds since launch, frame time
        for (size_t i = 0; i < changes.size(); ++i) {
            if (Ms(changes[i].time - launched) < 3000 || afterSeek[i]) continue;
            points.push_back({Ms(changes[i].time - launched) / 1000, changes[i].frame / clip.fps});
            if (i > 0 && !afterSeek[i - 1] && changes[i].frame > changes[i - 1].frame + 1 &&
                changes[i].frame <= changes[i - 1].frame + clip.fps) {
                for (int n = changes[i - 1].frame + 1; n < changes[i].frame; ++n) {
                    dropped += clip.kept.empty() || (size_t(n) < clip.kept.size() && clip.kept[size_t(n)]);
                }
            }
            ++steady;
        }
        // Each seek shifts the timeline, so the offset is fitted per run of frames between seeks.
        for (size_t from = 0; from < points.size();) {
            size_t to = from + 1;
            while (to < points.size() && std::abs((points[to].first - points[to].second) - (points[to - 1].first - points[to - 1].second)) < 0.5) ++to;
            std::vector<double> offsets;
            for (size_t i = from; i < to; ++i) offsets.push_back(points[i].first - points[i].second);
            std::nth_element(offsets.begin(), offsets.begin() + ptrdiff_t(offsets.size() / 2), offsets.end());
            const double median = offsets[offsets.size() / 2];
            for (size_t i = from; i < to; ++i) residual.push_back((points[i].first - points[i].second - median) * 1000);
            from = to;
        }
    }
    std::vector<double> absResidual;
    for (double r : residual) absResidual.push_back(std::abs(r));

    // Seeks after which the controls show within a second, though no mouse moved: the player's own doing.
    int seekPopups = 0;
    for (LONGLONG k : seekTimes) {
        const bool before = std::any_of(samples.begin(), samples.end(), [&](const Sample& s) {
            return s.covered && s.time <= k && Ms(k - s.time) < 300;
        });
        seekPopups += !before && std::any_of(samples.begin(), samples.end(), [&](const Sample& s) {
            return s.covered && s.time > k && Ms(s.time - k) < 1000;
        });
    }

    // Subtitles: each composition with a valid frame, once they are on, against the truth for its frame. The
    // controls covering the bottom are counted apart, as their own failure.
    int subChecked = 0, subWrong = 0, covered = 0;
    if (subs) {
        for (const Sample& s : samples) {
            if (s.frame < 0 || Ms(s.time - launched) < 3000) continue;
            if (s.covered) {
                ++covered;
                continue;
            }
            ++subChecked;
            subWrong += s.subtitle != CueShows(clip, s.frame);
        }
    }

    // A/V: each flash frame's first composition against the nearest click; positive means the video is late. Not
    // just after a seek, which can land on a flash frame before its sound plays.
    std::vector<double> av;
    for (size_t i = 0; i < changes.size(); ++i) {
        const Change& c = changes[i];
        if (c.frame == 0 || c.frame % clip.flashEvery || Ms(c.time - launched) < 3000 || afterSeek[i]) continue;
        double best = NAN;
        for (LONGLONG k : clicks) {
            const double d = Ms(c.time - k);
            if (std::abs(d) < 500 && !(std::abs(d) >= std::abs(best))) best = d;
        }
        if (!std::isnan(best)) av.push_back(best);
    }

    const std::string header =
        "label,clip,seconds,cores,ttff_ms,frames,dropped,cadence_mean_abs_ms,cadence_max_abs_ms,invalid_samples,"
        "sub_checked,sub_wrong,controls_covered,av_mean_ms,av_sd_ms,av_n,seeks,seek_mean_ms,seek_max_ms,seek_popups,cpu_ms,"
        "ctx_switches,busiest_thread_ctx,reads,read_mb,peak_ws_mb,private_mb,gpu_dedicated_mb,gpu_shared_mb";
    char row[1024];
    sprintf_s(row, "%s,%s,%.0f,%d,%.1f,%d,%d,%.2f,%.2f,%d,%d,%d,%d,%.1f,%.1f,%zu,%zu,%.1f,%.1f,%d,%.0f,%llu,%lu,%llu,%.2f,%.1f,%.1f,%.1f,%.1f",
              Narrow(label).c_str(), Narrow(clipPath.substr(clipPath.find_last_of(L"\\/") + 1)).c_str(), seconds,
              cores, ttff, steady, dropped, Mean(absResidual), Max(absResidual), invalid, subChecked, subWrong, covered,
              Mean(av), Sd(av), av.size(), seekMs.size(), Mean(seekMs), Max(seekMs), seekPopups, cpuMs, switches, busiest,
              io.ReadOperationCount, io.ReadTransferCount / 1048576.0, mem.PeakWorkingSetSize / 1048576.0,
              mem.PrivateUsage / 1048576.0, gpuDedicated, gpuShared);
    const bool fresh = GetFileAttributesW(csv.c_str()) == INVALID_FILE_ATTRIBUTES;
    std::ofstream out(csv, std::ios::app);
    if (fresh) out << header << '\n';
    out << row << '\n';
    printf("%s\n%s\n", header.c_str(), row);
    if (seeks && seekMs.size() < size_t(seeks)) fprintf(stderr, "bench: only %zu of %d seeks landed\n", seekMs.size(), seeks);
    return 0;
}
