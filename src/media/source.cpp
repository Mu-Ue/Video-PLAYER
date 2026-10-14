#include "media/source.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

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

// How much earlier the video track's edit list starts it than its samples say, in 100 ns units; 0 if the edit
// doesn't, delays the track instead or plays it at another rate, or the file has more than one video track.
LONGLONG ReadVideoShift(IMFByteStream* s) {
    QWORD length;
    if (FAILED(s->GetLength(&length))) return 0;
    Reader r(s);
    LONGLONG shift = 0;
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
            if (handler != Type("vide")) return true;
            // Rounded down: Media Foundation rounds the sample times either way, and the first frame mustn't land
            // before 0.
            if (++videos == 1 && start > 0 && scale) shift = MFllMulDiv(start, 10000000, scale, 0);
            return true;
        });
        return false;
    });
    return videos == 1 ? shift : 0;
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

// One of the source's streams, whose samples it moves shift earlier.
class ShiftedStream final : public Relay<IMFMediaStream> {
  public:
    ShiftedStream(IMFMediaSource* source, IMFMediaStream* inner, LONGLONG shift)
        : source_(source), inner_(inner), shift_(shift) {}
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
        if (!shift_ || FAILED(event->GetType(&type)) || type != MEMediaSample) return;
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
        }
        PropVariantClear(&value);
    }

    const ComPtr<IMFMediaSource> source_;
    const ComPtr<IMFMediaStream> inner_;
    LONGLONG shift_;  // only touched while passing on events, one at a time
    bool first_ = true;
};

// Media Foundation's own source for the file, with the video track's samples moved earlier.
class ShiftedSource final : public Relay<IMFMediaSourceEx>, public IMFGetService {
  public:
    ShiftedSource(IMFMediaSource* inner, LONGLONG shift) : inner_(inner), shift_(shift) {
        inner->QueryInterface(IID_PPV_ARGS(&ex_));
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
            // The stream ids aren't the file's track ids, but the file has only the one video track.
            ComPtr<IMFStreamDescriptor> descriptor;
            ComPtr<IMFMediaTypeHandler> handler;
            GUID major = GUID_NULL;
            if (SUCCEEDED(inner->GetStreamDescriptor(&descriptor)) &&
                SUCCEEDED(descriptor->GetMediaTypeHandler(&handler))) {
                handler->GetMajorType(&major);
            }
            stream.Attach(new (std::nothrow) ShiftedStream(this, inner, major == MFMediaType_Video ? shift_ : 0));
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
    const LONGLONG shift_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<ComPtr<ShiftedStream>> streams_;
    bool shut_ = false;
};

bool IsMp4(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    return dot && (!_wcsicmp(dot, L".mp4") || !_wcsicmp(dot, L".m4v") || !_wcsicmp(dot, L".mov"));
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
        if (type != MF_OBJECT_MEDIASOURCE || !stream || !url || !IsMp4(url)) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
        const LONGLONG shift = ReadVideoShift(stream);
        HRESULT hr = stream->SetCurrentPosition(0);
        if (FAILED(hr)) return hr;
        if (!shift) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
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
        source.Attach(new (std::nothrow) ShiftedSource(inner.Get(), shift));
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
