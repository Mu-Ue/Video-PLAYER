#include "media/audio.h"

#include <audioclient.h>
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <thread>

#include "media/player.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kAhead = 1.0;          // seconds of sound decoded ahead of the player
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

// The sound decoded ahead, which the decoder adds to and the player takes from, and the stream it comes from; all
// under lock.
struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE decoderWake = nullptr;  // auto-reset
    HANDLE playerWake = nullptr;   // auto-reset
    bool quit = false;
    // The file's, from its source.
    std::wstring path;  // the file whose sound plays
    UINT sound = 0;     // the id of its source's sound, once that has told its tracks
    std::vector<Track> tracks;
    ComPtr<IMFMediaStream> stream;  // of the track read, once handed over
    UINT streamVersion = 0;         // counts the streams handed over
    std::vector<ComPtr<IMFMediaStream>> listened;  // the file's streams whose events are listened to
    std::vector<ComPtr<IMFMediaEvent>> events;     // stream's, for the decoder
    // The decoder's.
    std::vector<BYTE> format;  // a WAVEFORMATEX of float samples; empty if there is no sound
    UINT formatVersion = 0;
    std::vector<float> samples;  // interleaved, from head on
    size_t head = 0;
    double time = 0;  // of the frame at head, in seconds
    bool end = false;  // nothing more to decode
    bool seeking = false;  // the source started anew, and samples holds nothing yet
    double asked = -1;     // where the player asked the sound from, before which it is passed; -1 if anywhere
    // The player's.
    double rate = 1;
    float gain = 1;
    AudioClock clock = nullptr;
    HWND hwnd = nullptr;
    UINT tracksMessage = 0;
} s;

std::thread decoder, player;
UINT sounds = 0;  // under s.lock: counts the sounds of sources

UINT32 Channels() {
    return s.format.size() >= sizeof(WAVEFORMATEX)
               ? reinterpret_cast<const WAVEFORMATEX*>(s.format.data())->nChannels
               : 0;
}

bool Contains(const std::vector<ComPtr<IMFMediaStream>>& streams, IMFMediaStream* stream) {
    return std::any_of(streams.begin(), streams.end(), [&](const auto& s) { return s.Get() == stream; });
}

// Hands the decoder the events of the stream read, and keeps listening to each of the file's streams: a track
// switched back to comes as the same one. Lives for the whole process.
class Listener final : public IMFAsyncCallback {
  public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *object = static_cast<IMFAsyncCallback*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        ComPtr<IUnknown> state;
        ComPtr<IMFMediaStream> stream;
        ComPtr<IMFMediaEvent> event;
        if (FAILED(result->GetState(&state)) || FAILED(state.As(&stream)) ||
            FAILED(stream->EndGetEvent(result, &event))) {
            return S_OK;  // shut down
        }
        AcquireSRWLockExclusive(&s.lock);
        const bool read = s.stream.Get() == stream.Get();
        const bool listened = Contains(s.listened, stream.Get());
        if (read) s.events.push_back(event);
        ReleaseSRWLockExclusive(&s.lock);
        if (read) SetEvent(s.decoderWake);
        if (listened) stream->BeginGetEvent(this, stream.Get());
        return S_OK;
    }
} listener;

// --- The decoder ---

// Adds frames of sound at a time, in seconds; frames from before where the player asked are passed.
void Add(double time, const float* f, size_t frames, UINT32 rate, UINT32 channels) {
    AcquireSRWLockExclusive(&s.lock);
    if (frames && !(s.samples.size() == s.head && s.asked >= 0 && time + double(frames) / rate < s.asked)) {
        size_t skip = 0;
        if (s.samples.size() == s.head) {
            s.samples.clear();
            s.head = 0;
            s.time = time;
        } else {
            // Gaps are filled with silence and overlaps cut, so the samples stay one timeline; not the rounding of
            // times, such as Matroska's to the millisecond, as the samples themselves follow on.
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
}

// Decodes a stream's samples to float ones, with the decoder Media Foundation has for its format, or none if it is
// uncompressed. A track it can't decode plays as silence, so it still keeps time.
struct Decoder {
    ComPtr<IMFTransform> mft;
    GUID subtype = GUID_NULL;  // of what is converted, MFAudioFormat_Float or MFAudioFormat_PCM; GUID_NULL if silent
    UINT32 rate = 0, channels = 0, bits = 0;
    bool provides = false;     // the decoder makes its own output samples
    DWORD outputSize = 0;
    ComPtr<IMFSample> output;  // made for the decoder, if it doesn't
    double next = -1;          // the time the decoder's next output follows on at, in seconds, or -1
    bool fed = false;          // the decoder was given a sample since it started or was flushed
    std::vector<float> floats;

    void Close() {
        if (mft) mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft.Reset();
        output.Reset();
        subtype = GUID_NULL;
        next = -1;
        fed = false;
    }

    // Takes on the format of type, if it is uncompressed audio it can convert.
    bool Take(IMFMediaType* type) {
        GUID sub = GUID_NULL;
        type->GetGUID(MF_MT_SUBTYPE, &sub);
        const UINT32 r = MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        const UINT32 c = MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0);
        const UINT32 b = MFGetAttributeUINT32(type, MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
        if (!r || !c || c > 32 ||
            !((sub == MFAudioFormat_Float && b == 32) ||
              (sub == MFAudioFormat_PCM && (b == 8 || b == 16 || b == 24 || b == 32)))) {
            return false;
        }
        subtype = sub;
        rate = r;
        channels = c;
        bits = b;
        return true;
    }

    // Has the decoder output float samples if it can, else integer ones.
    bool SetOutput() {
        ComPtr<IMFMediaType> type, pcm;
        for (DWORD i = 0; SUCCEEDED(mft->GetOutputAvailableType(0, i, &type)); ++i, type.Reset()) {
            GUID sub = GUID_NULL;
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFAudioFormat_Float) break;
            if (sub == MFAudioFormat_PCM && !pcm) pcm = type;
        }
        if (!type) type = pcm;
        MFT_OUTPUT_STREAM_INFO info{};
        if (!type || FAILED(mft->SetOutputType(0, type.Get(), 0)) || !Take(type.Get()) ||
            FAILED(mft->GetOutputStreamInfo(0, &info))) {
            subtype = GUID_NULL;
            return false;
        }
        provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        outputSize = std::max<DWORD>(info.cbSize, rate * channels * (bits / 8) / 4);  // a quarter second
        output.Reset();
        return true;
    }

    // The format the samples are converted to, with the channel mask of type if it has one.
    std::vector<BYTE> Format(IMFMediaType* type) const {
        WAVEFORMATEXTENSIBLE wave{};
        wave.Format.wFormatTag = channels > 2 ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_IEEE_FLOAT;
        wave.Format.nChannels = WORD(channels);
        wave.Format.nSamplesPerSec = rate;
        wave.Format.wBitsPerSample = 32;
        wave.Format.nBlockAlign = WORD(channels * 4);
        wave.Format.nAvgBytesPerSec = rate * channels * 4;
        size_t size = sizeof(WAVEFORMATEX);
        if (channels > 2) {
            wave.Format.cbSize = sizeof(wave) - sizeof(WAVEFORMATEX);
            wave.Samples.wValidBitsPerSample = 32;
            wave.dwChannelMask = type ? MFGetAttributeUINT32(type, MF_MT_AUDIO_CHANNEL_MASK, 0) : 0;
            wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            size = sizeof(wave);
        }
        const BYTE* p = reinterpret_cast<const BYTE*>(&wave);
        return std::vector<BYTE>(p, p + size);
    }

    // Opens a stream's track; returns the format its sound is decoded to.
    std::vector<BYTE> Open(IMFMediaStream* stream) {
        Close();
        ComPtr<IMFStreamDescriptor> descriptor;
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type, decoded;
        if (FAILED(stream->GetStreamDescriptor(&descriptor)) || FAILED(descriptor->GetMediaTypeHandler(&handler)) ||
            FAILED(handler->GetCurrentMediaType(&type))) {
            type.Reset();
        } else if (!Take(type.Get())) {
            GUID sub = GUID_NULL;
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            const MFT_REGISTER_TYPE_INFO input{MFMediaType_Audio, sub};
            IMFActivate** found = nullptr;
            UINT32 count = 0;
            if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
                                    MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                    &input, nullptr, &found, &count))) {
                for (UINT32 i = 0; i < count; ++i) {
                    if (!mft && SUCCEEDED(found[i]->ActivateObject(IID_PPV_ARGS(&mft))) &&
                        (FAILED(mft->SetInputType(0, type.Get(), 0)) || !SetOutput())) {
                        mft.Reset();
                        found[i]->ShutdownObject();
                    }
                    found[i]->Release();
                }
                CoTaskMemFree(found);
            }
            if (mft) {
                mft->GetOutputCurrentType(0, &decoded);
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            }
        }
        if (subtype == GUID_NULL) {  // silent
            rate = 48000;
            channels = 2;
        }
        return Format(decoded ? decoded.Get() : type.Get());
    }

    // Converts a sample of uncompressed sound and adds it at time.
    void Convert(IMFSample* sample, double time) {
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length))) {
            return;
        }
        const size_t frames = length / (bits / 8) / channels, count = frames * channels;
        floats.resize(count);
        float* f = floats.data();
        if (subtype == MFAudioFormat_Float) {
            memcpy(f, data, count * sizeof(float));
        } else if (bits == 16) {
            const auto* p = reinterpret_cast<const INT16*>(data);
            for (size_t i = 0; i < count; ++i) f[i] = p[i] * (1.0f / 32768);
        } else if (bits == 24) {
            for (size_t i = 0; i < count; ++i) {
                const BYTE* p = data + i * 3;
                f[i] = float(INT32(UINT32(p[0]) << 8 | UINT32(p[1]) << 16 | UINT32(p[2]) << 24)) * (1.0f / 2147483648.0f);
            }
        } else if (bits == 32) {
            const auto* p = reinterpret_cast<const INT32*>(data);
            for (size_t i = 0; i < count; ++i) f[i] = float(p[i]) * (1.0f / 2147483648.0f);
        } else {
            for (size_t i = 0; i < count; ++i) f[i] = (int(data[i]) - 128) * (1.0f / 128);
        }
        buffer->Unlock();
        Add(time, f, frames, rate, channels);
        next = time + double(frames) / rate;
    }

    // Takes what the decoder has made; false if the format of its sound changed.
    bool Pull() {
        for (;;) {
            if (!provides && !output) {
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(MFCreateSample(&output)) || FAILED(MFCreateMemoryBuffer(outputSize, &buffer)) ||
                    FAILED(output->AddBuffer(buffer.Get()))) {
                    output.Reset();
                    return true;
                }
            }
            if (!provides) {
                ComPtr<IMFMediaBuffer> buffer;
                if (SUCCEEDED(output->GetBufferByIndex(0, &buffer))) buffer->SetCurrentLength(0);
            }
            MFT_OUTPUT_DATA_BUFFER out{0, provides ? nullptr : output.Get(), 0, nullptr};
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            ComPtr<IMFSample> made;
            if (provides) {
                made.Attach(out.pSample);
            } else {
                made = output;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                SetOutput();
                return false;
            }
            if (FAILED(hr) || !made) return true;
            LONGLONG at = 0;
            const double time = SUCCEEDED(made->GetSampleTime(&at)) ? at / 1e7 : next;
            if (time >= 0 && subtype != GUID_NULL) Convert(made.Get(), time);
        }
    }

    // Decodes a sample of the stream; false if the format of the sound changed.
    bool Decode(IMFSample* sample) {
        LONGLONG at = 0;
        const bool timed = SUCCEEDED(sample->GetSampleTime(&at));
        if (subtype == GUID_NULL) {  // silence as long as the sample
            LONGLONG duration = 0;
            if (!timed) return true;
            if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) duration = 200000;
            floats.assign(size_t(MFllMulDiv(duration, rate, 10000000, 0)) * channels, 0.0f);
            Add(at / 1e7, floats.data(), floats.size() / channels, rate, channels);
            return true;
        }
        if (!mft) {
            if (timed) Convert(sample, at / 1e7);
            return true;
        }
        if (next < 0 && timed) next = at / 1e7;
        fed = true;
        if (mft->ProcessInput(0, sample, 0) == MF_E_NOTACCEPTING) {
            if (!Pull()) return false;
            mft->ProcessInput(0, sample, 0);
        }
        return Pull();
    }

    // Drops what the decoder holds; not before it is fed anything, which some decoders (FLAC's) don't take.
    void Flush() {
        if (mft && fed) mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        fed = false;
        next = -1;
    }
    bool Drain() {
        if (!mft) return true;
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        return Pull();
    }
};

// Starts the sound over, from nothing decoded yet, in a format; the player waits for where it starts.
void StartOver(std::vector<BYTE>* format, bool end) {
    AcquireSRWLockExclusive(&s.lock);
    if (format) {
        s.format = std::move(*format);
        ++s.formatVersion;
    }
    s.samples.clear();
    s.head = 0;
    s.end = end;
    s.seeking = true;
    s.asked = -1;
    ReleaseSRWLockExclusive(&s.lock);
    SetEvent(s.playerWake);
}

void DecoderLoop() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    Decoder d;
    ComPtr<IMFMediaStream> stream;
    UINT streamVersion = 0;
    std::vector<ComPtr<IMFMediaEvent>> events;
    // A sample is asked for; the stream refused the last ask (until its next event); it is stopped; it ended.
    bool requested = false, refused = false, stopped = true, ended = false;
    for (;;) {
        AcquireSRWLockExclusive(&s.lock);
        if (s.quit) {
            ReleaseSRWLockExclusive(&s.lock);
            break;
        }
        const bool changed = s.streamVersion != streamVersion;
        if (changed) {
            streamVersion = s.streamVersion;
            stream = s.stream;
        }
        events.swap(s.events);
        const bool full = double(s.samples.size() - s.head) / std::max<UINT32>(Channels(), 1) /
                              std::max<UINT32>(d.rate, 1) >= kAhead;
        ReleaseSRWLockExclusive(&s.lock);

        if (changed) {
            d.Close();
            std::vector<BYTE> format = stream ? d.Open(stream.Get()) : std::vector<BYTE>();
            StartOver(&format, !stream);
            requested = refused = ended = false;
            stopped = true;
        }
        for (const auto& event : events) {
            MediaEventType type = MEUnknown;
            HRESULT status = S_OK;
            event->GetType(&type);
            event->GetStatus(&status);
            refused = false;
            bool same = true;  // the format of the sound stays
            if (FAILED(status) || type == MEError || type == MEEndOfStream) {
                ended = true;
                requested = false;
                if (SUCCEEDED(status) && type == MEEndOfStream) same = d.Drain();
                AcquireSRWLockExclusive(&s.lock);
                s.end = true;
                ReleaseSRWLockExclusive(&s.lock);
                SetEvent(s.playerWake);
            } else if (type == MEMediaSample) {
                requested = false;
                PROPVARIANT value;
                PropVariantInit(&value);
                ComPtr<IMFSample> sample;
                if (SUCCEEDED(event->GetValue(&value)) && value.vt == VT_UNKNOWN && value.punkVal &&
                    SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&sample)))) {
                    same = d.Decode(sample.Get());
                }
                PropVariantClear(&value);
            } else if (type == MEStreamSeeked || (type == MEStreamStarted && stopped)) {
                stopped = ended = requested = false;  // a sample asked for before is dropped
                d.Flush();
                StartOver(nullptr, false);
            } else if (type == MEStreamStarted) {
                stopped = false;
            } else if (type == MEStreamStopped) {
                stopped = true;
                requested = false;
            }
            if (!same) {
                std::vector<BYTE> format = d.Format(nullptr);
                StartOver(&format, ended);
            }
        }
        // Asks for one sample at a time while less than kAhead is decoded.
        if (stream && !requested && !refused && !stopped && !ended && !full) {
            requested = SUCCEEDED(stream->RequestSample(nullptr));
            refused = !requested;
        }
        const bool idle = events.empty();
        events.clear();
        if (idle) WaitForSingleObject(s.decoderWake, INFINITE);
    }
    d.Close();
    stream.Reset();
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
            // Starts the sound where the picture will be when its first frame is heard, from what is decoded ahead.
            // If that doesn't reach there, the decoder passes what comes before, as it decodes far faster than it
            // plays; if it starts later, as just after a seek, the picture catches up.
            const double start = clock + (double(Now() - clockAt) / 1e7 + out.delay + out.period) * rate;
            AcquireSRWLockExclusive(&s.lock);
            bool ready = false;
            const size_t c = out.channels;
            const double end = s.time + double(s.samples.size() - s.head) / c / out.rate;
            if (!s.seeking && start >= s.time) {
                if (start < end - 0.05) {
                    s.head += size_t((start - s.time) * out.rate) * c;
                    s.time = start;
                    ready = true;
                } else if (!s.end) {
                    s.head = s.samples.size();
                    s.time = end;
                    s.asked = std::max(0.0, start - 0.05);  // so the samples begin a little before
                    SetEvent(s.decoderWake);
                }
            }
            const bool over = s.end && !ready;
            ReleaseSRWLockExclusive(&s.lock);
            if (!ready) {
                WaitForSingleObject(s.playerWake, over ? 20 : 5);
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
            if (gain <= 1) {
                for (size_t i = 0; i < size_t(room) * c; ++i) to[i] = made[i] * gain;
            } else {  // boosted: peaks bend smoothly toward full scale rather than clipping
                constexpr float knee = 0.9f;
                for (size_t i = 0; i < size_t(room) * c; ++i) {
                    const float y = made[i] * gain;
                    const float a = std::abs(y);
                    to[i] = a <= knee ? y : std::copysign(knee + (1 - knee) * std::tanh((a - knee) / (1 - knee)), y);
                }
            }
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
    AcquireSRWLockExclusive(&s.lock);
    s.stream.Reset();
    std::vector<ComPtr<IMFMediaStream>> listened = std::move(s.listened);
    s.events.clear();
    ReleaseSRWLockExclusive(&s.lock);
}

void WakeAudio() {
    if (s.playerWake) SetEvent(s.playerWake);
}

void OpenAudio(const std::wstring& path) {
    AcquireSRWLockExclusive(&s.lock);
    s.path = path;
    s.sound = 0;
    s.tracks.clear();
    s.stream.Reset();
    ++s.streamVersion;
    std::vector<ComPtr<IMFMediaStream>> listened = std::move(s.listened);  // let go of after the lock
    s.events.clear();
    ReleaseSRWLockExclusive(&s.lock);
    SetEvent(s.decoderWake);
}

std::vector<Track> AudioTrackList() {
    AcquireSRWLockShared(&s.lock);
    std::vector<Track> tracks = s.tracks;
    ReleaseSRWLockShared(&s.lock);
    return tracks;
}

void SelectAudio(size_t index) {
    AcquireSRWLockExclusive(&s.lock);
    if (index < s.tracks.size()) {
        for (Track& t : s.tracks) t.selected = &t - s.tracks.data() == ptrdiff_t(index);
    }
    ReleaseSRWLockExclusive(&s.lock);
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

UINT SetAudioTracks(const wchar_t* url, std::vector<Track> tracks) {
    AcquireSRWLockExclusive(&s.lock);
    const bool mine = url && !s.path.empty() && CompareStringOrdinal(url, -1, s.path.c_str(), -1, TRUE) == CSTR_EQUAL;
    if (mine) {
        s.sound = ++sounds;
        s.tracks = std::move(tracks);
    }
    const UINT sound = mine ? s.sound : 0;
    ReleaseSRWLockExclusive(&s.lock);
    if (mine) PostMessageW(s.hwnd, s.tracksMessage, 0, 0);
    return sound;
}

int AudioTrackToRead(UINT sound) {
    AcquireSRWLockShared(&s.lock);
    int index = -1;
    if (sound && sound == s.sound) {
        for (const Track& t : s.tracks) {
            if (t.selected) index = int(&t - s.tracks.data());
        }
    }
    ReleaseSRWLockShared(&s.lock);
    return index;
}

void SetAudioStream(UINT sound, IMFMediaStream* stream) {
    AcquireSRWLockExclusive(&s.lock);
    const bool mine = sound && sound == s.sound;
    const bool listen = mine && !Contains(s.listened, stream);
    if (listen) s.listened.push_back(stream);
    if (mine && s.stream.Get() != stream) {
        s.stream = stream;
        ++s.streamVersion;
        s.events.clear();
    }
    ReleaseSRWLockExclusive(&s.lock);
    if (listen) stream->BeginGetEvent(&listener, stream);
    if (mine) SetEvent(s.decoderWake);
}
