#include "media/source.h"

#include <audioclient.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

#include "media/mkv.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 Type(const char (&s)[5]) {
    return UINT32(BYTE(s[0])) << 24 | UINT32(BYTE(s[1])) << 16 | UINT32(BYTE(s[2])) << 8 | BYTE(s[3]);
}

UINT32 Be32(const BYTE* p) { return UINT32(p[0]) << 24 | UINT32(p[1]) << 16 | UINT32(p[2]) << 8 | p[3]; }
UINT64 Be64(const BYTE* p) { return UINT64(Be32(p)) << 32 | Be32(p + 4); }

// Reads the few small pieces of the file the box headers are, a page or two at a time: the headers of a track sit
// close together.
class Reader {
  public:
    explicit Reader(IMFByteStream* s) : s_(s) {}
    bool Read(QWORD at, BYTE* data, ULONG size) {
        if (size > kPage) return false;
        if (at < start_ || at + size > start_ + filled_) {
            start_ = at / kPage * kPage;
            filled_ = 0;
            if (FAILED(s_->SetCurrentPosition(start_)) || FAILED(s_->Read(buf_, sizeof(buf_), &filled_)) ||
                at + size > start_ + filled_) {
                return false;
            }
        }
        memcpy(data, buf_ + (at - start_), size);
        return true;
    }

  private:
    static constexpr ULONG kPage = 4096;  // the least the disk cache reads
    IMFByteStream* s_;
    QWORD start_ = 0;
    ULONG filled_ = 0;
    BYTE buf_[2 * kPage];  // so a piece that crosses into the next page is in it too
};

struct Box {
    UINT32 type;
    QWORD body, end;
    // Reads up to size bytes from the start of the body, but no fewer than least; returns how many.
    ULONG Read(Reader& r, BYTE* data, ULONG least, ULONG size) const {
        const ULONG n = ULONG(std::min<QWORD>(end - body, size));
        return n >= least && r.Read(body, data, n) ? n : 0;
    }
};

// Calls f(box) for each box in [at, end), up to a malformed or truncated one, or until f returns false. Reads only
// the headers.
template <class F>
void Boxes(Reader& r, QWORD at, QWORD end, F&& f) {
    while (at < end && end - at >= 8) {
        BYTE h[16];
        if (!r.Read(at, h, 8)) return;
        UINT64 size = Be32(h);
        Box box{Be32(h + 4), at + 8, 0};
        if (size == 1) {  // a 64-bit size follows
            if (end - at < 16 || !r.Read(at + 8, h + 8, 8)) return;
            size = Be64(h + 8);
            box.body = at + 16;
        } else if (size == 0) {  // up to the end
            size = end - at;
        }
        if (size < box.body - at || size > end - at) return;
        box.end = at + size;
        if (!f(box)) return;
        at = box.end;
    }
}

// How much earlier than their samples say a file's tracks start, in 100 ns units.
struct Shifts {
    LONGLONG video = 0;
    std::vector<LONGLONG> audio;  // of each audio track, in file order
    LONGLONG lead = 0;            // of every audio track, on top
    bool Any() const {
        return video || lead || std::any_of(audio.begin(), audio.end(), [](LONGLONG a) { return a != 0; });
    }
};

// How far the engine's clock runs ahead of the sound: its audio renderer's buffer, which it doesn't count. That
// is a shared stream's default buffer on the default output, which the renderer plays to. 0 if it can't tell.
LONGLONG AudioLead() {
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    WAVEFORMATEX* format = nullptr;
    UINT32 frames = 0;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices));
    if (SUCCEEDED(hr)) hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &client);
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, format, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&frames);
    const LONGLONG lead =
        SUCCEEDED(hr) && format->nSamplesPerSec ? MFllMulDiv(frames, 10000000, format->nSamplesPerSec, 0) : 0;
    CoTaskMemFree(format);
    return std::clamp<LONGLONG>(lead, 0, 1'000'000);  // 100 ms at most
}

// The lead found ahead of the first file by PrepareSourceExtension, which loads the audio stack.
std::atomic<PTP_WORK> leadWork = nullptr;
LONGLONG preparedLead = 0;

// The lead found ahead if there is one, else now, so a change of output shows on the next file.
LONGLONG TakeAudioLead() {
    if (const PTP_WORK work = leadWork.exchange(nullptr)) {
        WaitForThreadpoolWorkCallbacks(work, FALSE);
        CloseThreadpoolWork(work);
        return preparedLead;
    }
    return AudioLead();
}

// Each track's shift from its edit list: 0 if the edit doesn't start it later, delays it instead or plays it at
// another rate. The video's is 0 if the file has more than one video track.
Shifts ReadMp4Shifts(IMFByteStream* s) {
    QWORD length;
    if (FAILED(s->GetLength(&length))) return {};
    Reader r(s);
    Shifts shifts;
    LONGLONG video = 0;
    int videos = 0;
    Boxes(r, 0, length, [&](const Box& moov) {
        if (moov.type != Type("moov")) return true;
        Boxes(r, moov.body, moov.end, [&](const Box& trak) {
            if (trak.type != Type("trak")) return true;
            UINT32 handler = 0, scale = 0;
            LONGLONG start = 0;  // the first edit's media time
            BYTE h[28];
            Boxes(r, trak.body, trak.end, [&](const Box& b) {
                if (b.type == Type("edts")) {
                    Boxes(r, b.body, b.end, [&](const Box& elst) {
                        if (elst.type != Type("elst")) return true;
                        const ULONG n = elst.Read(r, h, 20, 28);
                        const bool v1 = n && h[0] == 1;
                        if (!n || (v1 && n < 28) || !Be32(h + 4)) return false;
                        const BYTE* rate = h + (v1 ? 24 : 16);
                        if (rate[0] == 0 && rate[1] == 1) start = v1 ? LONGLONG(Be64(h + 16)) : INT32(Be32(h + 12));
                        return false;
                    });
                } else if (b.type == Type("mdia")) {
                    Boxes(r, b.body, b.end, [&](const Box& m) {
                        if (m.type == Type("mdhd") && m.Read(r, h, 24, 24)) {
                            scale = Be32(h + (h[0] ? 20 : 12));
                        } else if (m.type == Type("hdlr") && m.Read(r, h, 12, 12)) {
                            handler = Be32(h + 8);
                        }
                        return true;
                    });
                }
                return true;
            });
            // Rounded down: Media Foundation rounds the sample times either way, and the first frame mustn't land
            // before 0.
            const LONGLONG shift = start > 0 && scale ? MFllMulDiv(start, 10000000, scale, 0) : 0;
            if (handler == Type("vide") && ++videos == 1) video = shift;
            if (handler == Type("soun")) shifts.audio.push_back(std::min<LONGLONG>(shift, 10'000'000));  // 1 s at most
            return true;
        });
        return false;
    });
    if (videos == 1) shifts.video = video;
    return shifts;
}

// Passes on the events of an inner object through a queue of its own, after Adjust has had a look at each. The
// inner object holds it while it waits for an event, until it shuts down.
template <class I>
class Relay : public I, public IMFAsyncCallback {
  public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ? static_cast<I*>(this)
                                                                                          : Find(riid);
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

    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override { return queue_->GetEvent(flags, event); }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
        return queue_->BeginGetEvent(callback, state);
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
        return queue_->EndGetEvent(result, event);
    }
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                            const PROPVARIANT* value) override {
        return queue_->QueueEventParamVar(type, extended, status, value);
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        ComPtr<IMFMediaEvent> event;
        if (FAILED(from_->EndGetEvent(result, &event))) return S_OK;  // shut down
        Adjust(event);
        queue_->QueueEvent(event.Get());
        from_->BeginGetEvent(this, nullptr);
        return S_OK;
    }

  protected:
    virtual ~Relay() = default;
    // Starts passing on from's events.
    HRESULT Listen(IMFMediaEventGenerator* from) {
        from_ = from;
        HRESULT hr = MFCreateEventQueue(&queue_);
        if (SUCCEEDED(hr)) hr = from->BeginGetEvent(this, nullptr);
        return hr;
    }
    virtual void* Find(REFIID riid) = 0;  // another interface, not yet referenced; null if none
    virtual void Adjust(ComPtr<IMFMediaEvent>& event) = 0;

    ComPtr<IMFMediaEventQueue> queue_;

  private:
    std::atomic<ULONG> refs_ = 1;
    ComPtr<IMFMediaEventGenerator> from_;
};

// The bytes and rate of a stream's uncompressed audio frames; 0 if it isn't uncompressed audio.
struct Pcm {
    UINT32 blockAlign = 0, rate = 0;
};

// One of the source's streams, whose samples it moves shift earlier. A video stream's shift is dropped if Media
// Foundation turns out to apply the edit itself. Uncompressed audio is cut where playback starts, as the renderer
// would drop a sample that starts before it whole, which leaves the sound early by up to a sample's length.
class ShiftedStream final : public Relay<IMFMediaStream> {
  public:
    ShiftedStream(IMFMediaSource* source, IMFMediaStream* inner, LONGLONG shift, bool video, Pcm pcm,
                  const std::atomic<LONGLONG>& start)
        : source_(source), inner_(inner), shift_(shift), first_(video), pcm_(pcm), start_(start) {}
    HRESULT Start() { return Listen(inner_.Get()); }
    IMFMediaStream* Inner() const { return inner_.Get(); }
    void Shutdown() { queue_->Shutdown(); }

    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override {
        if (!source) return E_POINTER;
        return source_.CopyTo(source);
    }
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override {
        return inner_->GetStreamDescriptor(descriptor);
    }
    STDMETHODIMP RequestSample(IUnknown* token) override { return inner_->RequestSample(token); }

  private:
    void* Find(REFIID riid) override {
        return riid == __uuidof(IMFMediaStream) ? static_cast<IMFMediaStream*>(this) : nullptr;
    }
    void Adjust(ComPtr<IMFMediaEvent>& event) override {
        MediaEventType type;
        if ((!shift_ && !pcm_.blockAlign) || FAILED(event->GetType(&type)) || type != MEMediaSample) return;
        PROPVARIANT value;
        PropVariantInit(&value);
        ComPtr<IMFSample> sample;
        LONGLONG time;
        if (SUCCEEDED(event->GetValue(&value)) && value.vt == VT_UNKNOWN && value.punkVal &&
            SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&sample))) &&
            SUCCEEDED(sample->GetSampleTime(&time))) {
            // The source starts at 0, so the first sample is the first frame: nearer 0 than shift if Media
            // Foundation already applied the edit.
            if (first_) {
                first_ = false;
                if (time < shift_ - time) shift_ = 0;
            }
            if (shift_) sample->SetSampleTime(time - shift_);
            if (pcm_.blockAlign) Cut(sample.Get(), time - shift_);
        }
        PropVariantClear(&value);
    }

    // Cuts off the frames of an uncompressed audio sample at `time` that come before playback starts.
    void Cut(IMFSample* sample, LONGLONG time) const {
        const LONGLONG start = start_;
        ComPtr<IMFMediaBuffer> whole, cut;
        DWORD length = 0;
        if (time >= start || FAILED(sample->ConvertToContiguousBuffer(&whole)) ||
            FAILED(whole->GetCurrentLength(&length))) {
            return;
        }
        const DWORD frames = length / pcm_.blockAlign;
        const DWORD skip = DWORD(std::min<LONGLONG>(MFllMulDiv(start - time, pcm_.rate, 10000000, 0), frames));
        if (!skip || skip == frames || FAILED(MFCreateMemoryBuffer((frames - skip) * pcm_.blockAlign, &cut))) return;
        BYTE *from = nullptr, *to = nullptr;
        if (FAILED(whole->Lock(&from, nullptr, nullptr))) return;
        if (SUCCEEDED(cut->Lock(&to, nullptr, nullptr))) {
            memcpy(to, from + skip * pcm_.blockAlign, (frames - skip) * pcm_.blockAlign);
            cut->Unlock();
            cut->SetCurrentLength((frames - skip) * pcm_.blockAlign);
        }
        whole->Unlock();
        if (to && SUCCEEDED(sample->RemoveAllBuffers()) && SUCCEEDED(sample->AddBuffer(cut.Get()))) {
            sample->SetSampleTime(time + MFllMulDiv(skip, 10000000, pcm_.rate, 0));
            sample->SetSampleDuration(MFllMulDiv(frames - skip, 10000000, pcm_.rate, 0));
        }
    }

    const ComPtr<IMFMediaSource> source_;
    const ComPtr<IMFMediaStream> inner_;
    LONGLONG shift_;  // only touched while passing on events, one at a time
    bool first_;      // the first sample is yet to be checked
    const Pcm pcm_;
    const std::atomic<LONGLONG>& start_;  // the source's, which it outlives
};

// The major type of a stream.
GUID MajorType(IMFStreamDescriptor* descriptor) {
    ComPtr<IMFMediaTypeHandler> handler;
    GUID major = GUID_NULL;
    if (SUCCEEDED(descriptor->GetMediaTypeHandler(&handler))) handler->GetMajorType(&major);
    return major;
}

Pcm PcmFormat(IMFStreamDescriptor* descriptor) {
    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType> type;
    GUID subtype = GUID_NULL;
    if (FAILED(descriptor->GetMediaTypeHandler(&handler)) || FAILED(handler->GetCurrentMediaType(&type)) ||
        FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
        (subtype != MFAudioFormat_PCM && subtype != MFAudioFormat_Float)) {
        return {};
    }
    const Pcm pcm{MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BLOCK_ALIGNMENT, 0),
                  MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0)};
    return pcm.blockAlign && pcm.rate ? pcm : Pcm{};
}

// Media Foundation's own source for the file, with the tracks' samples moved earlier.
class ShiftedSource final : public Relay<IMFMediaSourceEx>, public IMFGetService {
  public:
    ShiftedSource(IMFMediaSource* inner, const Shifts& shifts)
        : inner_(inner), video_(shifts.video), lead_(shifts.lead) {
        inner->QueryInterface(IID_PPV_ARGS(&ex_));
        // The stream ids aren't the file's track ids, but the audio streams come in the file's order. If their
        // number doesn't match, which one is which isn't known, and none is moved.
        ComPtr<IMFPresentationDescriptor> presentation;
        DWORD count = 0;
        if (FAILED(inner->CreatePresentationDescriptor(&presentation)) ||
            FAILED(presentation->GetStreamDescriptorCount(&count))) {
            return;
        }
        for (DWORD i = 0; i < count; ++i) {
            BOOL selected;
            ComPtr<IMFStreamDescriptor> descriptor;
            DWORD id;
            if (FAILED(presentation->GetStreamDescriptorByIndex(i, &selected, &descriptor)) ||
                FAILED(descriptor->GetStreamIdentifier(&id)) ||
                (MajorType(descriptor.Get()) == MFMediaType_Audio && audio_.size() == shifts.audio.size())) {
                audio_.clear();
                return;
            }
            if (MajorType(descriptor.Get()) == MFMediaType_Audio) audio_.push_back({id, shifts.audio[audio_.size()]});
        }
        if (audio_.size() != shifts.audio.size()) audio_.clear();
    }
    HRESULT Start() { return Listen(inner_.Get()); }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override { return Relay::QueryInterface(riid, object); }
    STDMETHODIMP_(ULONG) AddRef() override { return Relay::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return Relay::Release(); }

    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override {
        return inner_->GetCharacteristics(characteristics);
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override {
        return inner_->CreatePresentationDescriptor(descriptor);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* format, const PROPVARIANT* at) override {
        if (at && at->vt == VT_I8) start_ = at->hVal.QuadPart;  // else it resumes where it was
        return inner_->Start(descriptor, format, at);
    }
    STDMETHODIMP Stop() override { return inner_->Stop(); }
    STDMETHODIMP Pause() override { return inner_->Pause(); }
    STDMETHODIMP Shutdown() override {
        AcquireSRWLockExclusive(&lock_);
        shut_ = true;
        std::vector<ComPtr<ShiftedStream>> streams = std::move(streams_);
        ReleaseSRWLockExclusive(&lock_);
        const HRESULT hr = inner_->Shutdown();
        queue_->Shutdown();
        for (const auto& stream : streams) stream->Shutdown();
        return hr;
    }

    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override {
        return ex_->GetSourceAttributes(attributes);
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** attributes) override {
        return ex_->GetStreamAttributes(id, attributes);
    }
    STDMETHODIMP SetD3DManager(IUnknown* manager) override { return ex_->SetD3DManager(manager); }

    STDMETHODIMP GetService(REFGUID service, REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        ComPtr<IMFGetService> inner;
        return SUCCEEDED(inner_.As(&inner)) ? inner->GetService(service, riid, object) : MF_E_UNSUPPORTED_SERVICE;
    }

  private:
    void* Find(REFIID riid) override {
        if (riid == __uuidof(IMFMediaSource)) return static_cast<IMFMediaSourceEx*>(this);
        if (riid == __uuidof(IMFMediaSourceEx)) return ex_ ? static_cast<IMFMediaSourceEx*>(this) : nullptr;
        if (riid == __uuidof(IMFGetService)) return static_cast<IMFGetService*>(this);
        return nullptr;
    }
    // Hands the engine its own stream for each of the inner source's, the same one each time.
    void Adjust(ComPtr<IMFMediaEvent>& event) override {
        MediaEventType type;
        if (FAILED(event->GetType(&type)) || (type != MENewStream && type != MEUpdatedStream)) return;
        PROPVARIANT value;
        PropVariantInit(&value);
        ComPtr<IMFMediaStream> inner;
        if (SUCCEEDED(event->GetValue(&value)) && value.vt == VT_UNKNOWN && value.punkVal &&
            SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&inner)))) {
            ComPtr<ShiftedStream> stream = Wrap(inner.Get());
            GUID extended = GUID_NULL;
            HRESULT status = S_OK;
            event->GetExtendedType(&extended);
            event->GetStatus(&status);
            PROPVARIANT wrapped;
            wrapped.vt = VT_UNKNOWN;
            wrapped.punkVal = static_cast<IMFMediaStream*>(stream.Get());
            ComPtr<IMFMediaEvent> out;
            if (stream && SUCCEEDED(MFCreateMediaEvent(type, extended, status, &wrapped, &out)) &&
                SUCCEEDED(event->CopyAllItems(out.Get()))) {
                event = std::move(out);
            }
        }
        PropVariantClear(&value);
    }
    ComPtr<ShiftedStream> Wrap(IMFMediaStream* inner) {
        AcquireSRWLockExclusive(&lock_);
        ComPtr<ShiftedStream> stream;
        for (const auto& s : streams_) {
            if (s->Inner() == inner) stream = s;
        }
        if (!stream && !shut_) {
            // The file has only the one video track.
            ComPtr<IMFStreamDescriptor> descriptor;
            GUID major = GUID_NULL;
            DWORD id = 0;
            Pcm pcm;
            if (SUCCEEDED(inner->GetStreamDescriptor(&descriptor)) && SUCCEEDED(descriptor->GetStreamIdentifier(&id))) {
                major = MajorType(descriptor.Get());
                if (major == MFMediaType_Audio) pcm = PcmFormat(descriptor.Get());
            }
            const bool video = major == MFMediaType_Video;
            LONGLONG shift = video ? video_ : major == MFMediaType_Audio ? lead_ : 0;
            for (const auto& [audioId, audioShift] : audio_) {
                if (major == MFMediaType_Audio && audioId == id) shift += audioShift;
            }
            stream.Attach(new (std::nothrow) ShiftedStream(this, inner, shift, video, pcm, start_));
            if (stream && SUCCEEDED(stream->Start())) {
                streams_.push_back(stream);
            } else {
                stream.Reset();
            }
        }
        ReleaseSRWLockExclusive(&lock_);
        return stream;
    }

    const ComPtr<IMFMediaSource> inner_;
    ComPtr<IMFMediaSourceEx> ex_;  // null if the inner source has none, and then not asked for
    const LONGLONG video_, lead_;
    std::vector<std::pair<DWORD, LONGLONG>> audio_;  // each audio stream's id and own shift
    std::atomic<LONGLONG> start_ = 0;               // where playback last started
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<ComPtr<ShiftedStream>> streams_;
    bool shut_ = false;
};

bool HasExtension(const wchar_t* name, std::initializer_list<const wchar_t*> extensions) {
    const wchar_t* dot = wcsrchr(name, L'.');
    return dot && std::any_of(extensions.begin(), extensions.end(), [&](const wchar_t* e) { return !_wcsicmp(dot, e); });
}

// The file's shifts: from its edit lists or CodecDelays if it is MPEG-4 or Matroska, and the audio's lead.
Shifts ReadShifts(BSTR url, IMFByteStream* stream) {
    Shifts shifts;
    if (HasExtension(url, {L".mp4", L".m4v", L".mov"})) shifts = ReadMp4Shifts(stream);
    if (HasExtension(url, {L".mkv", L".webm"})) shifts.audio = MkvAudioDelays(url);
    shifts.lead = TakeAudioLead();
    return shifts;
}

class Extension final : public IMFMediaEngineExtension {
  public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineExtension)) {
            *object = static_cast<IMFMediaEngineExtension*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CanPlayType(BOOL, BSTR, MF_MEDIA_ENGINE_CANPLAY* answer) override {
        *answer = MF_MEDIA_ENGINE_CANPLAY_NOT_SUPPORTED;
        return S_OK;
    }
    // Creates the source at once; a file it leaves to the engine fails here, before any source is made.
    STDMETHODIMP BeginCreateObject(BSTR url, IMFByteStream* stream, MF_OBJECT_TYPE type, IUnknown** cancel,
                                   IMFAsyncCallback* callback, IUnknown* state) override {
        if (cancel) *cancel = nullptr;
        if (type != MF_OBJECT_MEDIASOURCE || !stream || !url) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
        const Shifts shifts = ReadShifts(url, stream);
        HRESULT hr = stream->SetCurrentPosition(0);
        if (FAILED(hr)) return hr;
        if (!shifts.Any()) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
        ComPtr<IMFSourceResolver> resolver;
        ComPtr<IUnknown> object;
        ComPtr<IMFMediaSource> inner;
        MF_OBJECT_TYPE created;
        hr = MFCreateSourceResolver(&resolver);
        if (SUCCEEDED(hr)) {
            hr = resolver->CreateObjectFromByteStream(stream, url, MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ,
                                                      nullptr, &created, &object);
        }
        if (SUCCEEDED(hr)) hr = object.As(&inner);
        if (FAILED(hr)) return hr;
        ComPtr<ShiftedSource> source;
        source.Attach(new (std::nothrow) ShiftedSource(inner.Get(), shifts));
        hr = source ? source->Start() : E_OUTOFMEMORY;
        ComPtr<IMFAsyncResult> result;
        if (SUCCEEDED(hr)) {
            hr = MFCreateAsyncResult(static_cast<IMFMediaSourceEx*>(source.Get()), callback, state, &result);
        }
        if (SUCCEEDED(hr)) hr = MFInvokeCallback(result.Get());
        if (FAILED(hr)) inner->Shutdown();
        return hr;
    }
    STDMETHODIMP CancelObjectCreation(IUnknown*) override { return S_OK; }
    STDMETHODIMP EndCreateObject(IMFAsyncResult* result, IUnknown** object) override {
        *object = nullptr;
        const HRESULT hr = result->GetStatus();
        return FAILED(hr) ? hr : result->GetObject(object);
    }
};

Extension extension;

}  // namespace

IMFMediaEngineExtension* SourceExtension() { return &extension; }

void PrepareSourceExtension() {
    const PTP_WORK work = CreateThreadpoolWork(
        [](PTP_CALLBACK_INSTANCE, void*, PTP_WORK) {
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            preparedLead = AudioLead();
            if (com) CoUninitialize();
        },
        nullptr, nullptr);
    if (!work) return;
    SubmitThreadpoolWork(work);
    leadWork = work;
}
