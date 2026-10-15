#include "media/audio.h"

#include <audioclient.h>
#include <avrt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <thread>

#include "media/player.h"
#include "media/source.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kAhead = 1.0;          // seconds of sound decoded ahead of the player
constexpr double kNear = 5;             // seconds ahead the decoder decodes on to rather than seeking
constexpr double kGap = 0.01;           // seconds between samples' times taken as a gap, or overlap, in the sound
constexpr double kQueued = 0.04;        // seconds of sound queued at the output: how soon a new speed is heard
constexpr double kDeadband = 0.003;     // seconds the sound may drift from the picture before it is eased back
constexpr double kResync = 0.04;        // seconds of drift that restart it in step instead
constexpr double kEase = 0.2;           // speed change per second of drift while easing back
constexpr double kMaxEase = 0.002;      // at most
// Just after the sound starts, which it does up to an output period later than asked, it catches up faster, then
// eases once within a millisecond.
constexpr double kCatchUp = 4, kMaxCatchUp = 0.05;
constexpr double kSegment = 0.02;       // seconds of the time stretch's segments, which overlap by half
constexpr double kSearch = 0.008;       // seconds it looks either way for the best place to join the next segment

LONGLONG Now() {  // in 100 ns units
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return MFllMulDiv(now.QuadPart, 10000000, frequency.QuadPart, 0);
}

std::wstring Take(LPWSTR text) {
    std::wstring s = text ? text : L"";
    CoTaskMemFree(text);
    return s;
}

// The sound decoded ahead, which the decoder adds to and the player takes from, and what the player asks of the
// decoder; all under lock.
struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE decoderWake = nullptr;  // auto-reset
    HANDLE playerWake = nullptr;   // auto-reset
    bool quit = false;
    // Asked of the decoder.
    std::wstring path;
    bool open = false;     // path is yet to be opened
    int track = -1;        // the audio track to switch to, or -1
    double seekTo = -1;    // where to decode from, or -1
    // The decoder's.
    std::vector<Track> tracks;
    std::vector<BYTE> format;  // a WAVEFORMATEX of float samples; empty if there is no sound
    UINT formatVersion = 0;
    std::vector<float> samples;  // interleaved, from head on
    size_t head = 0;
    double time = 0;  // of the frame at head, in seconds
    bool end = false;  // nothing more to decode
    bool seeking = false;  // a seek is asked or under way, and samples holds nothing yet
    double asked = -1;     // where the player last asked to decode from, or -1 if it is yet to ask
    // The player's.
    double rate = 1;
    float gain = 1;
    AudioClock clock = nullptr;
    HWND hwnd = nullptr;
    UINT tracksMessage = 0;
} s;

std::thread decoder, player;

UINT32 Channels() {
    return s.format.size() >= sizeof(WAVEFORMATEX)
               ? reinterpret_cast<const WAVEFORMATEX*>(s.format.data())->nChannels
               : 0;
}

// --- The decoder ---

struct Decoder {
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    std::vector<DWORD> streams;  // the reader's stream index of each audio track
    int track = -1;
    UINT32 rate = 0, channels = 0;

    void Close() {
        reader.Reset();
        if (source) source->Shutdown();
        source.Reset();
        streams.clear();
        track = -1;
    }

    // Has the reader decode the track to float samples, and returns their format; empty if it can't.
    std::vector<BYTE> Select(int index) {
        track = index;
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        const DWORD stream = streams[size_t(index)];
        ComPtr<IMFMediaType> type, current;
        WAVEFORMATEX* wave = nullptr;
        UINT32 size = 0;
        HRESULT hr = reader->SetStreamSelection(stream, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&type);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(stream, nullptr, type.Get());
        if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(stream, &current);
        if (SUCCEEDED(hr)) hr = MFCreateWaveFormatExFromMFMediaType(current.Get(), &wave, &size);
        std::vector<BYTE> format;
        if (SUCCEEDED(hr) && wave->nChannels && wave->nSamplesPerSec && wave->wBitsPerSample == 32) {
            format.assign(reinterpret_cast<BYTE*>(wave), reinterpret_cast<BYTE*>(wave) + size);
            rate = wave->nSamplesPerSec;
            channels = wave->nChannels;
        }
        CoTaskMemFree(wave);
        return format;
    }

    // Opens a file's sound; the tracks are empty if it has none.
    std::vector<Track> Open(const std::wstring& path) {
        Close();
        std::vector<Track> tracks;
        CreateAudioSource(path.c_str(), &source);
        ComPtr<IMFPresentationDescriptor> presentation;
        ComPtr<IMFAttributes> attributes;
        if (!source || FAILED(source->CreatePresentationDescriptor(&presentation)) ||
            FAILED(MFCreateAttributes(&attributes, 1)) ||
            FAILED(attributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE)) ||
            FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader))) {
            Close();
            return tracks;
        }
        DWORD count = 0;
        presentation->GetStreamDescriptorCount(&count);
        int chosen = -1;
        for (DWORD i = 0; i < count; ++i) {
            BOOL selected = FALSE;
            ComPtr<IMFStreamDescriptor> descriptor;
            ComPtr<IMFMediaTypeHandler> handler;
            GUID major = GUID_NULL;
            if (FAILED(presentation->GetStreamDescriptorByIndex(i, &selected, &descriptor)) ||
                FAILED(descriptor->GetMediaTypeHandler(&handler)) || FAILED(handler->GetMajorType(&major)) ||
                major != MFMediaType_Audio) {
                continue;
            }
            LPWSTR language = nullptr, name = nullptr;
            descriptor->GetAllocatedString(MF_SD_LANGUAGE, &language, nullptr);
            descriptor->GetAllocatedString(MF_SD_STREAM_NAME, &name, nullptr);
            if (selected && chosen < 0) chosen = int(streams.size());
            tracks.push_back({Take(language), Take(name), 0, false});
            streams.push_back(i);
        }
        if (tracks.empty()) Close();
        else tracks[size_t(std::max(chosen, 0))].selected = true;
        return tracks;
    }

    // Decodes the next sample and adds it; false at the end, or if it can't.
    bool Read(UINT seekVersion, const UINT& currentSeekVersion) {
        DWORD flags = 0;
        LONGLONG at = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(streams[size_t(track)], 0, nullptr, &flags, &at, &sample)) ||
            (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            return false;
        }
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (!sample || FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
            FAILED(buffer->Lock(&data, nullptr, &length))) {
            return true;  // a gap: nothing to add
        }
        const size_t frames = length / (sizeof(float) * channels);
        const float* f = reinterpret_cast<const float*>(data);
        AcquireSRWLockExclusive(&s.lock);
        const double time = at / 1e7;
        // Sound before where the player asked for it, which a source can start from after a seek (Matroska's of
        // a track just selected, from long before), is decoded past.
        if (seekVersion == currentSeekVersion && frames &&
            !(s.seeking && s.asked >= 0 && time + double(frames) / rate < s.asked)) {
            size_t skip = 0;
            if (s.samples.size() == s.head) {
                s.samples.clear();
                s.head = 0;
                s.time = time;
            } else {
                // Gaps are filled with silence and overlaps cut, so the samples stay one timeline; not the
                // rounding of times, such as Matroska's to the millisecond, as the samples themselves follow on.
                const double expected = s.time + double(s.samples.size() - s.head) / channels / rate;
                const double gap = time - expected;
                if (gap > kGap) {
                    s.samples.resize(s.samples.size() + size_t(std::min(gap, 1.0) * rate) * channels, 0.0f);
                } else if (gap < -kGap) {
                    skip = std::min(frames, size_t(-gap * rate));
                }
            }
            s.samples.insert(s.samples.end(), f + skip * channels, f + frames * channels);
            s.seeking = false;
        }
        ReleaseSRWLockExclusive(&s.lock);
        buffer->Unlock();
        return true;
    }
};

UINT seekVersion = 0;  // under s.lock: counts the seeks asked of the decoder

void DecoderLoop() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    Decoder d;
    UINT readVersion = 0;
    for (;;) {
        AcquireSRWLockExclusive(&s.lock);
        if (s.quit) {
            ReleaseSRWLockExclusive(&s.lock);
            break;
        }
        const bool open = std::exchange(s.open, false);
        const std::wstring path = open ? s.path : std::wstring();
        const int track = std::exchange(s.track, -1);
        const double seekTo = std::exchange(s.seekTo, -1.0);
        readVersion = seekVersion;
        const bool full = s.end || double(s.samples.size() - s.head) / std::max<UINT32>(Channels(), 1) /
                                           std::max<UINT32>(d.rate, 1) >= kAhead;
        ReleaseSRWLockExclusive(&s.lock);

        if (open || (track >= 0 && d.reader && track < int(d.streams.size()) && track != d.track)) {
            std::vector<Track> tracks;
            int index = track;
            if (open) {
                tracks = d.Open(path);
                index = int(std::find_if(tracks.begin(), tracks.end(), [](const Track& t) { return t.selected; }) -
                            tracks.begin());
            }
            std::vector<BYTE> format = d.reader && index < int(d.streams.size()) ? d.Select(index) : std::vector<BYTE>();
            AcquireSRWLockExclusive(&s.lock);
            if (open) {
                s.tracks = std::move(tracks);
                if (s.open) {  // another file was asked for meanwhile
                    ReleaseSRWLockExclusive(&s.lock);
                    continue;
                }
            }
            for (Track& t : s.tracks) t.selected = &t - s.tracks.data() == index;
            s.format = std::move(format);
            ++s.formatVersion;
            s.samples.clear();
            s.head = 0;
            s.end = s.format.empty();
            s.seeking = true;  // the player asks where to start
            s.asked = -1;
            ReleaseSRWLockExclusive(&s.lock);
            if (open) PostMessageW(s.hwnd, s.tracksMessage, 0, 0);
            SetEvent(s.playerWake);
            continue;
        }
        if (seekTo >= 0 && d.reader) {
            PROPVARIANT position{};
            position.vt = VT_I8;
            position.hVal.QuadPart = std::llround(seekTo * 1e7);
            d.reader->SetCurrentPosition(GUID_NULL, position);
            AcquireSRWLockExclusive(&s.lock);
            s.end = false;
            ReleaseSRWLockExclusive(&s.lock);
            continue;
        }
        if (!d.reader || d.track < 0 || full) {
            WaitForSingleObject(s.decoderWake, INFINITE);
            continue;
        }
        if (!d.Read(readVersion, seekVersion)) {
            AcquireSRWLockExclusive(&s.lock);
            if (readVersion == seekVersion) s.end = true;
            ReleaseSRWLockExclusive(&s.lock);
        }
    }
    d.Close();
    if (com) CoUninitialize();
}

// --- The player ---

// Follows the default output, as the system's own players do.
class DeviceWatch final : public IMMNotificationClient {
  public:
    std::atomic<bool> changed = false;
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            changed = true;
            SetEvent(s.playerWake);
        }
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
} deviceWatch;

// Stretches the sound in time, keeping its pitch: overlapping segments of it, each joined where it best matches
// how the last one goes on (WSOLA). At normal speed each segment is the last one's own continuation, so the sound
// passes through unchanged.
class Stretch {
  public:
    void Reset(UINT32 rate, UINT32 channels) {
        rate_ = rate;
        channels_ = channels;
        half_ = std::max<size_t>(16, size_t(kSegment * rate / 2));
        search_ = size_t(kSearch * rate);
        window_.resize(2 * half_);
        for (size_t i = 0; i < window_.size(); ++i) {
            window_[i] = float(0.5 - 0.5 * std::cos(2 * 3.14159265358979 * double(i) / double(window_.size())));
        }
        in_.clear();
        joined_ = false;
        position_ = 0;
    }
    size_t Half() const { return half_; }
    // The frames it needs past the next position to make the next half segment.
    size_t Needed() const { return size_t(std::ceil(position_)) + search_ + 2 * half_ + 1; }
    size_t Buffered() const { return in_.size() / channels_; }
    void Add(const float* frames, size_t count) { in_.insert(in_.end(), frames, frames + count * channels_); }
    // Seconds from the first frame added to the next position.
    double Position() const { return (position_ + double(dropped_)) / rate_; }
    void Start(double time) {
        time_ = time;
        dropped_ = 0;
    }
    double Time() const { return time_ + Position(); }

    // Makes the next half segment into out, moving on by tempo half segments; Needed() frames must be in.
    void Next(float* out, double tempo) {
        const size_t c = channels_;
        const size_t at = size_t(std::lround(position_));
        size_t from = at;
        if (joined_) {
            // Within the search, nothing matches the last segment's run better than the run itself.
            const size_t natural = last_ + half_;
            from = natural + search_ >= at && natural <= at + search_ ? natural : BestJoin(natural, at);
        }
        // Fades from the last segment's second half into this one's first; its own continuation fades back into
        // itself, as the window's halves add up to 1, so it is passed through as it is.
        const float* seg = in_.data() + from * c;
        const float* last = in_.data() + (last_ + half_) * c;
        if (joined_ && last == seg) {
            std::copy(seg, seg + half_ * c, out);
        } else {
            for (size_t i = 0; i < half_; ++i) {
                for (size_t k = 0; k < c; ++k) {
                    out[i * c + k] =
                        (joined_ ? last[i * c + k] * window_[half_ + i] : 0.0f) + seg[i * c + k] * window_[i];
                }
            }
        }
        joined_ = true;
        last_ = from;
        position_ += double(half_) * tempo;
        // Lets go of what no join can use any more, in large steps.
        const size_t next = size_t(std::max(0.0, std::floor(position_)));
        const size_t keep = std::min(last_ + half_, next > search_ ? next - search_ : 0);
        if (keep >= 8192) {
            in_.erase(in_.begin(), in_.begin() + ptrdiff_t(keep * c));
            position_ -= double(keep);
            last_ -= keep;
            dropped_ += keep;
        }
    }

  private:
    // Where near at, within the search, the input best matches its run from natural, by correlation of their
    // channels' sum, coarsely and then exactly around the best.
    size_t BestJoin(size_t natural, size_t at) const {
        const size_t c = channels_;
        const size_t lo = at > search_ ? at - search_ : 0, hi = at + search_;
        const auto score = [&](size_t from, size_t stride) {
            double sum = 0, energy = 1e-9;
            for (size_t i = 0; i < half_; i += stride) {
                float a = 0, b = 0;
                for (size_t k = 0; k < c; ++k) {
                    a += in_[(natural + i) * c + k];
                    b += in_[(from + i) * c + k];
                }
                sum += double(a) * b;
                energy += double(b) * b;
            }
            return sum / std::sqrt(energy);
        };
        size_t best = at;
        double top = -1e300;
        for (size_t from = lo; from <= hi; from += 4) {
            if (const double v = score(from, 4); v > top) top = v, best = from;
        }
        const size_t coarse = best;
        top = -1e300;
        for (size_t from = coarse > lo + 3 ? coarse - 3 : lo; from <= std::min(coarse + 3, hi); ++from) {
            if (const double v = score(from, 1); v > top) top = v, best = from;
        }
        return best;
    }

    UINT32 rate_ = 1, channels_ = 1;
    size_t half_ = 0, search_ = 0;
    std::vector<float> window_, in_;
    bool joined_ = false;  // a segment was made, which the next fades from
    size_t last_ = 0;      // where the last segment started in in_
    double position_ = 0;  // where the next segment is due in in_, in frames
    size_t dropped_ = 0;   // frames let go of since Start
    double time_ = 0;      // of the first frame since Start
};

struct Output {
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    HANDLE ready = nullptr;  // set by the client for each period
    UINT64 frequency = 0;
    UINT32 bufferFrames = 0, rate = 0, channels = 0;
    // How long after the clock's position a frame is heard, in seconds: the system mixer's own buffer, a shared
    // stream's default one on the device, which the position doesn't count.
    double delay = 0;
    double period = 0;  // of the system mixer, in seconds: the output starts playing at its next pass
    bool started = false;

    void Close() {
        if (client && started) client->Stop();
        started = false;
        render.Reset();
        clock.Reset();
        client.Reset();
    }

    // Opens a client of the default output for the format, measuring its delay if measure. A fresh one each time
    // the sound starts: one started again after a reset is heard later than its position tells.
    bool Open(const std::vector<BYTE>& format, bool measure) {
        Close();
        const auto* wave = reinterpret_cast<const WAVEFORMATEX*>(format.data());
        ComPtr<IMMDevice> device;
        HRESULT hr = devices ? devices->GetDefaultAudioEndpoint(eRender, eConsole, &device) : E_FAIL;
        if (SUCCEEDED(hr) && measure) {
            ComPtr<IAudioClient> probe;
            WAVEFORMATEX* mix = nullptr;
            UINT32 frames = 0;
            delay = SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &probe)) &&
                            SUCCEEDED(probe->GetMixFormat(&mix)) &&
                            SUCCEEDED(probe->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mix, nullptr)) &&
                            SUCCEEDED(probe->GetBufferSize(&frames)) && mix->nSamplesPerSec
                        ? std::min(double(frames) / mix->nSamplesPerSec, 0.1)
                        : 0;
            CoTaskMemFree(mix);
        }
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &client);
        if (SUCCEEDED(hr)) {
            if (ComPtr<IAudioClient2> client2; SUCCEEDED(client.As(&client2))) {
                AudioClientProperties properties{sizeof(properties), FALSE, AudioCategory_Movie, AUDCLNT_STREAMOPTIONS_NONE};
                client2->SetClientProperties(&properties);
            }
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    REFERENCE_TIME(kQueued * 2 * 1e7), 0, wave, nullptr);
        }
        if (SUCCEEDED(hr)) {
            REFERENCE_TIME periodDefault = 0;
            period = SUCCEEDED(client->GetDevicePeriod(&periodDefault, nullptr)) ? std::min(periodDefault / 1e7, 0.05) : 0;
            hr = client->SetEventHandle(ready);
        }
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&clock));
        if (SUCCEEDED(hr)) hr = clock->GetFrequency(&frequency);
        if (FAILED(hr) || !frequency) {
            Close();
            return false;
        }
        rate = wave->nSamplesPerSec;
        channels = wave->nChannels;
        return true;
    }
};

// Where each run of frames handed to the output came from, to tell what is heard.
struct Mapping {
    LONGLONG frame;  // the first frame's number since the output started
    double time;     // its time in the video, in seconds
    double step;     // seconds of video a frame
};

void PlayerLoop() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    DWORD task = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    Output out;
    out.ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&out.devices)))) {
        out.devices->RegisterEndpointNotificationCallback(&deviceWatch);
    }
    Stretch stretch;
    std::vector<float> made;  // made by the stretch, not yet handed to the output
    std::deque<Mapping> mappings;
    LONGLONG madeFrames = 0;  // frames made since the output started
    UINT formatVersion = UINT_MAX;
    std::vector<BYTE> format;
    bool synced = false, failed = false, catchingUp = false;
    double ease = 0, lastClock = -1;
    LONGLONG lastClockAt = 0;
    const auto stop = [&] {
        if (out.started) out.Close();
        synced = false;
    };
    for (;;) {
        AcquireSRWLockShared(&s.lock);
        const bool quit = s.quit;
        const UINT version = s.formatVersion;
        const double rate = s.rate;
        const float gain = s.gain;
        ReleaseSRWLockShared(&s.lock);
        if (quit) break;
        if (version != formatVersion || deviceWatch.changed.exchange(false) || failed) {
            stop();
            AcquireSRWLockShared(&s.lock);
            format = s.format;
            ReleaseSRWLockShared(&s.lock);
            formatVersion = version;
            failed = !format.empty() && !out.Open(format, true);
            if (failed) {  // the output is gone or busy: tried again shortly
                WaitForSingleObject(s.playerWake, 200);
                continue;
            }
            if (!format.empty()) stretch.Reset(out.rate, out.channels);
        }

        // Plays only while the picture moves on.
        double clock = 0;
        LONGLONG clockAt = 0;
        bool running = s.clock(clock, clockAt) && !format.empty();
        if (running && clock == lastClock && clockAt - lastClockAt > 300000) running = false;  // stalled
        if (clock != lastClock) lastClock = clock, lastClockAt = clockAt;
        if (!running) {
            stop();
            WaitForSingleObject(s.playerWake, !format.empty() && s.clock(clock, clockAt) ? 10 : INFINITE);
            continue;
        }

        if (!synced) {
            stop();
            if (!out.client && !out.Open(format, false)) {
                failed = true;
                continue;
            }
            // Starts the sound where the picture will be when its first frame is heard: from what is decoded
            // ahead if that reaches there, else once the decoder has gone there.
            const double start = clock + (double(Now() - clockAt) / 1e7 + out.delay + out.period) * rate;
            AcquireSRWLockExclusive(&s.lock);
            bool ready = false;
            const size_t c = out.channels;
            const double end = s.time + double(s.samples.size() - s.head) / c / out.rate;
            if (!s.seeking && start >= s.time && start < end - 0.05) {
                s.head += size_t((start - s.time) * out.rate) * c;
                s.time = start;
                ready = true;
            } else {
                // Else the decoder goes on to there, as it decodes far faster than it plays: from where it is if
                // that is a little before, or after a seek of its own.
                const bool nearby = s.seeking ? s.asked >= 0 && start >= s.asked && start < s.asked + kNear
                                            : start >= s.time && start < end + kNear;
                s.samples.clear();
                s.head = 0;
                s.seeking = true;
                s.asked = std::max(0.0, start - 0.05);  // so the samples begin a little before
                if (!nearby) {
                    s.seekTo = s.asked;
                    ++seekVersion;
                }
                SetEvent(s.decoderWake);
            }
            ReleaseSRWLockExclusive(&s.lock);
            if (!ready) {
                WaitForSingleObject(s.playerWake, 5);
                continue;
            }
            stretch.Reset(out.rate, out.channels);
            stretch.Start(start);
            made.clear();
            mappings.clear();
            madeFrames = 0;
            ease = 0;
            synced = true;
            catchingUp = true;
        }

        // Tops the output up to kQueued, stretching the sound decoded ahead.
        const size_t c = out.channels;
        UINT32 padding = 0;
        if (FAILED(out.client->GetCurrentPadding(&padding))) {
            failed = true;
            continue;
        }
        const UINT32 want = std::min(out.bufferFrames, UINT32(kQueued * out.rate));
        UINT32 room = want > padding ? want - padding : 0;
        bool starved = false, finished = false;
        while (made.size() / c < room) {
            if (stretch.Buffered() < stretch.Needed()) {
                AcquireSRWLockExclusive(&s.lock);
                const size_t has = (s.samples.size() - s.head) / c;
                const size_t take = std::min(has, stretch.Needed() - stretch.Buffered() + 4096);
                stretch.Add(s.samples.data() + s.head, take);
                s.head += take * c;
                s.time += double(take) / out.rate;
                if (s.head > s.samples.size() / 2) {  // lets go of what was taken, in large steps
                    s.samples.erase(s.samples.begin(), s.samples.begin() + ptrdiff_t(s.head));
                    s.head = 0;
                }
                finished = s.end && take == has;
                ReleaseSRWLockExclusive(&s.lock);
                SetEvent(s.decoderWake);
                if (stretch.Buffered() < stretch.Needed()) {
                    starved = !finished;
                    break;
                }
            }
            const double tempo = rate * (1 + ease);
            mappings.push_back({madeFrames + LONGLONG(made.size() / c), stretch.Time(), tempo / out.rate});
            const size_t at = made.size();
            made.resize(at + stretch.Half() * c);
            stretch.Next(made.data() + at, tempo);
        }
        if (finished && made.size() / c < room) made.resize(room * c, 0.0f);  // silence past the end
        room = std::min<UINT32>(room, UINT32(made.size() / c));
        BYTE* buffer = nullptr;
        if (room && SUCCEEDED(out.render->GetBuffer(room, &buffer))) {
            float* to = reinterpret_cast<float*>(buffer);
            for (size_t i = 0; i < size_t(room) * c; ++i) to[i] = made[i] * gain;
            out.render->ReleaseBuffer(room, 0);
            made.erase(made.begin(), made.begin() + ptrdiff_t(room) * ptrdiff_t(c));
            madeFrames += room;
        } else if (room) {
            failed = true;
            continue;
        }
        if (!out.started) {
            out.started = SUCCEEDED(out.client->Start());
            if (!out.started) failed = true;
        }
        if (starved) synced = false;  // the decoder fell behind: started over where the picture is

        // Eases the sound back in step with the picture when they drift apart, or starts it over if far.
        UINT64 position = 0, positionAt = 0;
        // Not before the output has played anything: until then its position tells nothing.
        if (out.started && SUCCEEDED(out.clock->GetPosition(&position, &positionAt)) && position &&
            !mappings.empty()) {
            const LONGLONG heardFrame = LONGLONG(double(position) * out.rate / double(out.frequency));
            while (mappings.size() > 1 && mappings[1].frame <= heardFrame) mappings.pop_front();
            const Mapping& m = mappings.front();
            if (heardFrame >= m.frame) {
                const double heard = m.time + double(heardFrame - m.frame) * m.step;
                const double picture =
                    clock + (double(LONGLONG(positionAt) - clockAt) / 1e7 + out.delay) * rate;
                const double drift = heard - picture;
                if (std::abs(drift) > kResync) {
                    synced = false;
                    stop();
                    continue;
                }
                if (std::abs(drift) < 0.001) catchingUp = false;
                ease = catchingUp ? std::clamp(-kCatchUp * drift, -kMaxCatchUp, kMaxCatchUp)
                       : std::abs(drift) < kDeadband ? 0
                                                     : std::clamp(-kEase * drift, -kMaxEase, kMaxEase);
            }
        }
        const HANDLE events[] = {out.ready, s.playerWake};
        WaitForMultipleObjects(2, events, FALSE, 20);
    }
    stop();
    out.Close();
    if (out.devices) out.devices->UnregisterEndpointNotificationCallback(&deviceWatch);
    out.devices.Reset();
    CloseHandle(out.ready);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (com) CoUninitialize();
}

}  // namespace

void StartAudio(AudioClock clock, HWND hwnd, UINT tracks) {
    s.clock = clock;
    s.hwnd = hwnd;
    s.tracksMessage = tracks;
    s.decoderWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.playerWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.decoderWake || !s.playerWake) return;
    decoder = std::thread(DecoderLoop);
    player = std::thread(PlayerLoop);
}

void StopAudio() {
    AcquireSRWLockExclusive(&s.lock);
    s.quit = true;
    ReleaseSRWLockExclusive(&s.lock);
    if (s.decoderWake) SetEvent(s.decoderWake);
    if (s.playerWake) SetEvent(s.playerWake);
    if (decoder.joinable()) decoder.join();
    if (player.joinable()) player.join();
}

void WakeAudio() {
    if (s.playerWake) SetEvent(s.playerWake);
}

void OpenAudio(const std::wstring& path) {
    AcquireSRWLockExclusive(&s.lock);
    s.path = path;
    s.open = true;
    s.track = -1;
    s.seekTo = -1;
    s.tracks.clear();
    s.format.clear();
    ++s.formatVersion;
    s.samples.clear();
    s.head = 0;
    s.end = true;
    s.seeking = false;
    s.asked = -1;
    ++seekVersion;
    ReleaseSRWLockExclusive(&s.lock);
    SetEvent(s.decoderWake);
    SetEvent(s.playerWake);
}

std::vector<Track> AudioTrackList() {
    AcquireSRWLockShared(&s.lock);
    std::vector<Track> tracks = s.tracks;
    ReleaseSRWLockShared(&s.lock);
    return tracks;
}

void SelectAudio(size_t index) {
    AcquireSRWLockExclusive(&s.lock);
    s.track = int(index);
    ReleaseSRWLockExclusive(&s.lock);
    SetEvent(s.decoderWake);
}

void SetAudioRate(double rate) {
    AcquireSRWLockExclusive(&s.lock);
    s.rate = rate;
    ReleaseSRWLockExclusive(&s.lock);
    SetEvent(s.playerWake);
}

void SetAudioVolume(double volume, bool muted) {
    AcquireSRWLockExclusive(&s.lock);
    s.gain = muted ? 0.0f : float(volume);
    ReleaseSRWLockExclusive(&s.lock);
}
