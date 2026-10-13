#include "media/mkv.h"

#include <mfapi.h>
#include <wrl/implements.h>

#include <algorithm>
#include <memory>
#include <string_view>

#include "config.h"
#include "media/subtitles.h"

using Microsoft::WRL::ComPtr;

namespace {

// Element ids, with their length marker.
enum : UINT32 {
    kEbml = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB,
    kSeekId = 0x53AB, kSeekPosition = 0x53AC, kInfo = 0x1549A966, kTimestampScale = 0x2AD7B1,
    kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86,
    kLanguage = 0x22B59C, kLanguageBcp47 = 0x22B59D, kName = 0x536E, kDefaultDuration = 0x23E383,
    kContentEncodings = 0x6D80, kContentEncoding = 0x6240, kContentEncodingType = 0x5033,
    kContentCompression = 0x5034, kContentCompAlgo = 0x4254, kContentCompSettings = 0x4255,
    kCluster = 0x1F43B675, kTimestamp = 0xE7, kBlockGroup = 0xA0, kBlock = 0xA1, kSimpleBlock = 0xA3,
    kBlockDuration = 0x9B,
};
constexpr UINT64 kUnknown = ~UINT64(0);  // an element size that isn't given, or a position that isn't
constexpr UINT64 kMaxHeaderBytes = 1 << 22;  // of the elements read whole: the file header, Info and Tracks
constexpr UINT64 kMaxBlockBytes = 1 << 16;   // of a subtitle block (group)
constexpr size_t kMaxElementHeader = 12;     // an id of up to 4 bytes and a size of up to 8
constexpr size_t kTrackBytes = 16;           // enough to hold a block's track number, also in a group
constexpr size_t kMaxCursors = 8;
constexpr size_t kMaxHeads = 16;
constexpr size_t kMaxHeadBytes = 1 << 20;

// Reads an EBML variable-length integer at p, moving p past it: an id keeps its length marker, a size loses it
// (all ones: kUnknown).
bool Vint(const BYTE*& p, const BYTE* end, UINT64& value, bool id) {
    if (p >= end || !*p) return false;
    int length = 1;
    while (!(*p & (0x80 >> (length - 1)))) ++length;
    if (length > end - p || (id && length > 4)) return false;
    UINT64 v = id ? *p : *p & (0xFF >> length);
    bool ones = v == UINT64(0xFF >> length);
    for (int i = 1; i < length; ++i) {
        v = v << 8 | p[i];
        ones &= p[i] == 0xFF;
    }
    p += length;
    value = !id && ones ? kUnknown : v;
    return true;
}

bool ParseHeader(const BYTE*& p, const BYTE* end, UINT32& id, UINT64& size) {
    UINT64 v;
    if (!Vint(p, end, v, true) || !Vint(p, end, size, false)) return false;
    id = UINT32(v);
    return true;
}

UINT64 UInt(const BYTE* p, size_t size) {
    UINT64 v = 0;
    for (size_t i = 0; i < size && i < 8; ++i) v = v << 8 | p[i];
    return size <= 8 ? v : 0;
}

// Calls f(id, data, size) for each child element in [p, end), up to a malformed or truncated one.
template <class F>
void Children(const BYTE* p, const BYTE* end, F&& f) {
    UINT32 id;
    UINT64 size;
    while (p < end && ParseHeader(p, end, id, size) && size <= UINT64(end - p)) {
        f(id, p, size_t(size));
        p += size;
    }
}

std::wstring Widen(const BYTE* p, size_t size) {
    std::wstring text;
    const int len = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), int(size), nullptr, 0);
    if (len > 0) {
        text.resize(size_t(len));
        MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), int(size), text.data(), len);
    }
    return text;
}

bool Equals(const BYTE* p, size_t size, std::string_view s) {
    return size == s.size() && std::equal(s.begin(), s.end(), reinterpret_cast<const char*>(p));
}

struct TextTrack {
    UINT64 number;
    bool ass;                    // S_TEXT/ASS or S_TEXT/SSA: the text follows eight other fields
    std::string prefix;          // bytes stripped from the front of each block
    double defaultDuration = 0;  // of a block without its own, in seconds
    std::vector<Cue> batch;      // read but not yet posted
};

// Where parsing goes on in the file. Playback reads it in pieces, mostly in order, but also elsewhere ahead of
// time and after a seek; each of those gets a cursor of its own.
struct Cursor {
    UINT64 next = kUnknown;  // where the next element starts
    UINT64 last = 0;         // where the one before it started
    bool inCluster = false;
    UINT64 clusterEnd = kUnknown, clusterTime = kUnknown;
    UINT64 carryAt = 0;      // an element cut off at the end of a piece, from there, until the next piece
    std::vector<BYTE> carry;
    UINT64 used = 0;         // the piece that last moved it on
};

// Picks the text tracks' cues out of the bytes playback reads from the file, at their offsets. Video and audio
// blocks are skipped by their size.
struct Parser {
    SRWLOCK lock = SRWLOCK_INIT;
    HWND hwnd = nullptr;  // none once stopped
    double scale = 1e-3;  // seconds per timestamp unit
    std::vector<TextTrack> tracks;
    std::vector<Cursor> cursors;
    UINT64 pieces = 0;
    // A piece no cursor followed starts within a cluster, and a new cursor parses it from the next. Its part before
    // that is kept for the cursor that gets there later.
    struct Head {
        UINT64 at;
        std::vector<BYTE> bytes;
    };
    std::vector<Head> heads;

    TextTrack* Find(UINT64 number) {
        for (TextTrack& t : tracks) {
            if (t.number == number) return &t;
        }
        return nullptr;
    }

    // Turns a block of a text track, with its cluster's time and the duration of its group, into a cue.
    void AddBlock(const BYTE* p, const BYTE* end, UINT64 clusterTime, UINT64 duration) {
        UINT64 number;
        if (!Vint(p, end, number, false) || end - p < 3 || (p[2] & 0x06)) return;  // laced: not text
        TextTrack* t = Find(number);
        if (!t) return;
        const double start = (double(clusterTime) + INT16(p[0] << 8 | p[1])) * scale;
        const double length = duration != kUnknown ? double(duration) * scale : t->defaultDuration;
        p += 3;
        std::string bytes = t->prefix;
        bytes.append(reinterpret_cast<const char*>(p), size_t(end - p));
        size_t from = 0;
        for (int fields = 0; t->ass && fields < 8; ++fields) {
            from = bytes.find(',', from);
            if (from == std::string::npos) return;
            ++from;
        }
        std::wstring raw = Widen(reinterpret_cast<const BYTE*>(bytes.data() + from), bytes.size() - from);
        std::erase(raw, L'\r');
        Cue cue{start, start + length};
        SetCueText(raw, cue);
        if (!cue.text.empty() && cue.end > cue.start) t->batch.push_back(std::move(cue));
    }

    // Whether a block of which `have` bytes are here may be a text track's: 1 yes, 0 no, -1 if that can't be told
    // yet. Its track number leads a SimpleBlock and a group's Block, normally its first child.
    int Text(UINT32 id, const BYTE* p, size_t have) {
        const BYTE* end = p + std::min(have, kTrackBytes);
        UINT32 child = kBlock;
        UINT64 size, number;
        if (id == kBlockGroup && !ParseHeader(p, end, child, size)) return have < kTrackBytes ? -1 : 0;
        if (child != kBlock) return 1;  // read whole to find out
        if (!Vint(p, end, number, false)) return have < kTrackBytes ? -1 : 0;
        return Find(number) ? 1 : 0;
    }

    // Parses the elements from c.next on that lie in the piece [base, base + n). An element cut off at its end is
    // kept until the next piece.
    void Walk(Cursor& c, UINT64 base, const BYTE* p, size_t n) {
        while (c.next != kUnknown && c.next >= base && c.next - base < n) {
            if (c.inCluster && c.clusterEnd != kUnknown && c.next >= c.clusterEnd) c.inCluster = false;
            const BYTE* q = p + (c.next - base);
            const BYTE* d = q;
            const size_t left = n - size_t(c.next - base);
            UINT32 id;
            UINT64 size;
            if (!ParseHeader(d, p + n, id, size)) {
                if (left < kMaxElementHeader) {
                    c.carryAt = c.next;
                    c.carry.assign(q, p + n);
                } else {
                    c.next = kUnknown;  // damaged
                }
                return;
            }
            const UINT64 data = c.next + UINT64(d - q);
            c.last = c.next;
            if (id == kCluster) {
                c.inCluster = true;
                c.clusterEnd = size == kUnknown ? kUnknown : data + size;
                c.clusterTime = kUnknown;
                c.next = data;  // into it
                continue;
            }
            if (size == kUnknown) {
                c.next = kUnknown;
                return;
            }
            const size_t have = left - size_t(d - q);
            const bool block = (id == kSimpleBlock || id == kBlockGroup) && c.clusterTime != kUnknown &&
                               size <= kMaxBlockBytes;
            if (c.inCluster && ((id == kTimestamp && size <= 8) || block)) {
                if (size > have) {
                    if (id == kTimestamp || Text(id, d, have)) {
                        c.carryAt = c.next;
                        c.carry.assign(q, p + n);
                        return;
                    }
                } else if (id == kTimestamp) {
                    c.clusterTime = UInt(d, size_t(size));
                } else if (id == kSimpleBlock) {
                    AddBlock(d, d + size, c.clusterTime, kUnknown);
                } else {
                    const BYTE* found = nullptr;
                    size_t foundSize = 0;
                    UINT64 duration = kUnknown;
                    Children(d, d + size, [&](UINT32 child, const BYTE* v, size_t s) {
                        if (child == kBlock) found = v, foundSize = s;
                        if (child == kBlockDuration) duration = UInt(v, s);
                    });
                    if (found) AddBlock(found, found + foundSize, c.clusterTime, duration);
                }
            }
            c.next = data + size;
        }
    }

    // Finds the first cluster in the piece from p + from on, by its id, and parses from there; returns where it is,
    // if anywhere. The id can also occur by chance in other data, so a hit counts only if a cluster's header and
    // Timestamp follow in the piece.
    size_t Resync(Cursor& c, UINT64 base, const BYTE* p, size_t n, size_t from) {
        static constexpr BYTE kId[] = {0x1F, 0x43, 0xB6, 0x75};
        const BYTE* end = p + n;
        for (const BYTE* q = p + from; (q = std::search(q, end, std::begin(kId), std::end(kId))) != end; ++q) {
            const BYTE* d = q;
            UINT32 id;
            UINT64 size;
            if (!ParseHeader(d, end, id, size)) continue;
            // The Timestamp comes first, after a CRC-32 at most.
            bool timed = false;
            for (int i = 0; i < 2 && !timed && ParseHeader(d, end, id, size) && size <= UINT64(end - d); ++i) {
                timed = id == kTimestamp && size <= 8;
                d += size;
            }
            if (!timed) continue;
            c.next = base + UINT64(q - p);
            c.inCluster = false;
            Walk(c, base, p, n);
            return size_t(q - p);
        }
        return n;
    }

    // Moves the cursor on through the piece [at, at + n), if it continues there. Returns whether it did.
    bool Follow(Cursor& c, UINT64 at, const BYTE* p, size_t n) {
        bool moved = false;
        // An element cut off at the end of the last piece is joined with the start of this one.
        if (!c.carry.empty() && c.next == c.carryAt && at == c.carryAt + c.carry.size()) {
            std::vector<BYTE> joined = std::move(c.carry);
            const size_t take = std::min(n, size_t(kMaxBlockBytes + kMaxElementHeader));
            joined.insert(joined.end(), p, p + take);
            c.carry.clear();
            Walk(c, c.carryAt, joined.data(), joined.size());
            if (take < n) c.carry.clear();  // cut off for good
            moved = true;
        }
        if (c.next != kUnknown && c.next >= at && c.next - at < n) {
            Walk(c, at, p, n);
            if (c.next == kUnknown) Resync(c, at, p, n, size_t(std::max(c.last + 1, at) - at));
            moved = true;
        }
        return moved;
    }

    // Takes a piece the player read, at its offset in the file, and posts the cues in it.
    void Feed(UINT64 at, const BYTE* p, size_t n) {
        AcquireSRWLockExclusive(&lock);
        if (hwnd && n) {
            ++pieces;
            bool followed = false;  // by a cursor, or inside an element one skips
            for (Cursor& c : cursors) {
                if (Follow(c, at, p, n)) {
                    c.used = pieces;
                    followed = true;
                    // On through the kept parts it gets to.
                    for (size_t i = 0; i < heads.size();) {
                        if (Follow(c, heads[i].at, heads[i].bytes.data(), heads[i].bytes.size())) {
                            heads.erase(heads.begin() + ptrdiff_t(i));
                            i = 0;
                        } else {
                            ++i;
                        }
                    }
                } else if (at + n <= c.next && at + n > c.last) {
                    followed = true;
                }
            }
            if (!followed) {
                Cursor c;
                const size_t head = Resync(c, at, p, n, 0);
                if (head) {
                    if (heads.size() >= kMaxHeads) heads.erase(heads.begin());
                    heads.push_back({at, std::vector<BYTE>(p, p + std::min(head, kMaxHeadBytes))});
                }
                if (c.next != kUnknown) {
                    c.used = pieces;
                    if (cursors.size() >= kMaxCursors) {
                        cursors.erase(std::min_element(cursors.begin(), cursors.end(),
                                                       [](const Cursor& a, const Cursor& b) { return a.used < b.used; }));
                    }
                    cursors.push_back(std::move(c));
                }
            }
            // A cursor that lost its place, or met another, ends.
            for (size_t i = 0; i < cursors.size();) {
                const UINT64 next = cursors[i].next;
                if (next == kUnknown || std::any_of(cursors.begin(), cursors.begin() + ptrdiff_t(i),
                                                    [&](const Cursor& c) { return c.next == next; })) {
                    cursors.erase(cursors.begin() + ptrdiff_t(i));
                } else {
                    ++i;
                }
            }
            for (TextTrack& t : tracks) {
                if (t.batch.empty()) continue;
                auto* batch = new std::vector<Cue>(std::move(t.batch));
                t.batch = {};
                if (!PostMessageW(hwnd, WM_APP_SUBTITLE_CUES, WPARAM(t.number), LPARAM(batch))) delete batch;
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
};

// Reads the file at open: the header, Info and Tracks.
struct File {
    HANDLE file = INVALID_HANDLE_VALUE;
    UINT64 size = 0;

    ~File() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    bool Read(UINT64 at, void* out, size_t n) {
        if (at > size || n > size - at) return false;
        OVERLAPPED o{};
        o.Offset = DWORD(at);
        o.OffsetHigh = DWORD(at >> 32);
        DWORD read = 0;
        return ReadFile(file, out, DWORD(n), &read, &o) && read == n;
    }

    // The header of the element at `at`: its id, the position of its data and its size.
    bool Header(UINT64 at, UINT32& id, UINT64& data, UINT64& length) {
        BYTE head[kMaxElementHeader];
        if (at >= size) return false;
        const size_t n = size_t(std::min<UINT64>(sizeof head, size - at));
        if (!Read(at, head, n)) return false;
        const BYTE* p = head;
        if (!ParseHeader(p, head + n, id, length)) return false;
        data = at + UINT64(p - head);
        return true;
    }

    // The element with this id at `at`, read whole.
    bool ElementAt(UINT64 at, UINT32 id, std::vector<BYTE>& out) {
        UINT32 found;
        UINT64 data, length;
        if (!Header(at, found, data, length) || found != id || length > kMaxHeaderBytes) return false;
        out.resize(size_t(length));
        return Read(data, out.data(), out.size());
    }
};

// Reads the file header, Info and Tracks, which come before the first cluster, and finds that.
bool ReadHeaders(File& f, Parser& r, std::vector<Track>& out) {
    std::vector<BYTE> data;
    UINT32 id;
    UINT64 at, length;
    if (!f.ElementAt(0, kEbml, data) || !f.Header(0, id, at, length)) return false;
    bool matroska = false;
    Children(data.data(), data.data() + data.size(), [&](UINT32 field, const BYTE* d, size_t s) {
        if (field == kDocType) matroska = Equals(d, s, "matroska") || Equals(d, s, "webm");
    });
    if (!matroska || !f.Header(at + length, id, at, length) || id != kSegment) return false;
    const UINT64 segment = at;
    const UINT64 segmentEnd = length != kUnknown && length <= f.size - at ? at + length : f.size;

    UINT64 info = kUnknown, tracks = kUnknown;
    for (UINT64 p = segment; p < segmentEnd && f.Header(p, id, at, length); p = at + length) {
        if (id == kCluster) {
            r.cursors.push_back({p});
            break;
        }
        if (length == kUnknown) break;
        if (id == kInfo) info = p;
        if (id == kTracks) tracks = p;
        if (id == kSeekHead && f.ElementAt(p, kSeekHead, data)) {
            Children(data.data(), data.data() + data.size(), [&](UINT32 field, const BYTE* d, size_t s) {
                if (field != kSeek) return;
                UINT64 target = 0, position = kUnknown;
                Children(d, d + s, [&](UINT32 field2, const BYTE* v, size_t n) {
                    if (field2 == kSeekId) target = UInt(v, n);
                    if (field2 == kSeekPosition) position = segment + UInt(v, n);
                });
                if (position == kUnknown) return;
                if (target == kInfo && info == kUnknown) info = position;
                if (target == kTracks && tracks == kUnknown) tracks = position;
            });
        }
    }
    if (info != kUnknown && f.ElementAt(info, kInfo, data)) {
        Children(data.data(), data.data() + data.size(), [&](UINT32 field, const BYTE* d, size_t s) {
            if (field == kTimestampScale && UInt(d, s)) r.scale = double(UInt(d, s)) * 1e-9;
        });
    }
    if (tracks == kUnknown || !f.ElementAt(tracks, kTracks, data)) return false;

    bool readable = true;
    Children(data.data(), data.data() + data.size(), [&](UINT32 field, const BYTE* d, size_t s) {
        if (field != kTrackEntry) return;
        TextTrack t{};
        UINT64 type = 0;
        bool text = false, encoded = false;
        Track track{};
        std::wstring language, bcp47;
        Children(d, d + s, [&](UINT32 entry, const BYTE* v, size_t n) {
            switch (entry) {
                case kTrackNumber: t.number = UInt(v, n); break;
                case kTrackType: type = UInt(v, n); break;
                case kCodecId:
                    text = Equals(v, n, "S_TEXT/UTF8") || Equals(v, n, "S_TEXT/WEBVTT") ||
                           Equals(v, n, "S_TEXT/ASS") || Equals(v, n, "S_TEXT/SSA");
                    t.ass = Equals(v, n, "S_TEXT/ASS") || Equals(v, n, "S_TEXT/SSA");
                    break;
                case kLanguage: language = Widen(v, n); break;
                case kLanguageBcp47: bcp47 = Widen(v, n); break;
                case kName: track.name = Widen(v, n); break;
                case kDefaultDuration: t.defaultDuration = double(UInt(v, n)) * 1e-9; break;
                case kContentEncodings:
                    // Only header stripping, which keeps the rest of each block as it is, can be undone here.
                    Children(v, v + n, [&](UINT32 id2, const BYTE* d2, size_t s2) {
                        if (id2 != kContentEncoding) return;
                        bool stripped = false;
                        UINT64 encodingType = 0;
                        Children(d2, d2 + s2, [&](UINT32 id3, const BYTE* d3, size_t s3) {
                            if (id3 == kContentEncodingType) encodingType = UInt(d3, s3);
                            if (id3 != kContentCompression) return;
                            UINT64 algorithm = 0;
                            Children(d3, d3 + s3, [&](UINT32 id4, const BYTE* d4, size_t s4) {
                                if (id4 == kContentCompAlgo) algorithm = UInt(d4, s4);
                                if (id4 == kContentCompSettings) t.prefix.assign(reinterpret_cast<const char*>(d4), s4);
                            });
                            stripped = algorithm == 3;
                        });
                        encoded |= encodingType != 0 || !stripped;
                    });
                    break;
            }
        });
        if (type != 0x11 || !text) return;
        if (encoded || !t.number || t.number > MAXDWORD || r.Find(t.number) || t.prefix.size() > kMaxBlockBytes) {
            readable = false;
            return;
        }
        track.language = bcp47.empty() ? language.empty() ? L"eng" : language : bcp47;  // Matroska's default
        track.id = DWORD(t.number);
        out.push_back(std::move(track));
        r.tracks.push_back(std::move(t));
    });
    return readable && !r.tracks.empty();
}

// A read the player started, which hands the bytes to the parser when it completes and then completes the
// player's own.
class ReadOp final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IMFAsyncCallback> {
  public:
    ReadOp(std::shared_ptr<Parser> parser, IMFByteStream* inner, BYTE* buffer, UINT64 at, IMFAsyncCallback* callback,
           IUnknown* state)
        : parser_(std::move(parser)), inner_(inner), buffer_(buffer), at_(at), callback_(callback), state_(state) {}

    ULONG read = 0;

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        const HRESULT hr = inner_->EndRead(result, &read);
        if (SUCCEEDED(hr)) parser_->Feed(at_, buffer_, read);
        ComPtr<IMFAsyncResult> done;
        if (SUCCEEDED(MFCreateAsyncResult(static_cast<IMFAsyncCallback*>(this), callback_.Get(), state_.Get(),
                                          &done))) {
            done->SetStatus(hr);
            MFInvokeCallback(done.Get());
        }
        return S_OK;
    }

  private:
    std::shared_ptr<Parser> parser_;
    ComPtr<IMFByteStream> inner_;
    BYTE* buffer_;
    UINT64 at_;
    ComPtr<IMFAsyncCallback> callback_;
    ComPtr<IUnknown> state_;
};

// The file as the player reads it, passing what it reads to the parser too.
class Stream final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IMFByteStream> {
  public:
    Stream(IMFByteStream* inner, std::shared_ptr<Parser> parser) : inner_(inner), parser_(std::move(parser)) {}

    STDMETHODIMP GetCapabilities(DWORD* capabilities) override { return inner_->GetCapabilities(capabilities); }
    STDMETHODIMP GetLength(QWORD* length) override { return inner_->GetLength(length); }
    STDMETHODIMP SetLength(QWORD length) override { return inner_->SetLength(length); }
    STDMETHODIMP GetCurrentPosition(QWORD* position) override { return inner_->GetCurrentPosition(position); }
    STDMETHODIMP SetCurrentPosition(QWORD position) override { return inner_->SetCurrentPosition(position); }
    STDMETHODIMP IsEndOfStream(BOOL* end) override { return inner_->IsEndOfStream(end); }
    STDMETHODIMP Read(BYTE* buffer, ULONG size, ULONG* read) override {
        QWORD at = 0;
        const bool known = SUCCEEDED(inner_->GetCurrentPosition(&at));
        const HRESULT hr = inner_->Read(buffer, size, read);
        if (SUCCEEDED(hr) && known && read) parser_->Feed(at, buffer, *read);
        return hr;
    }
    STDMETHODIMP BeginRead(BYTE* buffer, ULONG size, IMFAsyncCallback* callback, IUnknown* state) override {
        QWORD at = 0;
        HRESULT hr = inner_->GetCurrentPosition(&at);
        if (FAILED(hr)) return hr;
        const ComPtr<ReadOp> op = Microsoft::WRL::Make<ReadOp>(parser_, inner_.Get(), buffer, at, callback, state);
        return op ? inner_->BeginRead(buffer, size, op.Get(), nullptr) : E_OUTOFMEMORY;
    }
    STDMETHODIMP EndRead(IMFAsyncResult* result, ULONG* read) override {
        if (!result || !read) return E_INVALIDARG;
        ComPtr<IUnknown> object;
        const HRESULT hr = result->GetObject(&object);
        if (FAILED(hr)) return hr;
        *read = static_cast<ReadOp*>(static_cast<IMFAsyncCallback*>(object.Get()))->read;
        return result->GetStatus();
    }
    STDMETHODIMP Write(const BYTE* buffer, ULONG size, ULONG* written) override {
        return inner_->Write(buffer, size, written);
    }
    STDMETHODIMP BeginWrite(const BYTE* buffer, ULONG size, IMFAsyncCallback* callback, IUnknown* state) override {
        return inner_->BeginWrite(buffer, size, callback, state);
    }
    STDMETHODIMP EndWrite(IMFAsyncResult* result, ULONG* written) override {
        return inner_->EndWrite(result, written);
    }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD flags, QWORD* position) override {
        return inner_->Seek(origin, offset, flags, position);
    }
    STDMETHODIMP Flush() override { return inner_->Flush(); }
    STDMETHODIMP Close() override { return inner_->Close(); }

  private:
    ComPtr<IMFByteStream> inner_;
    std::shared_ptr<Parser> parser_;
};

std::shared_ptr<Parser> current;  // of the video being opened or played

}  // namespace

std::vector<Track> StartMkvSubtitles(const std::wstring& path, HWND hwnd) {
    StopMkvSubtitles();
    File f;
    f.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER size{};
    auto r = std::make_shared<Parser>();
    std::vector<Track> tracks;
    if (f.file == INVALID_HANDLE_VALUE || GetFileType(f.file) != FILE_TYPE_DISK || !GetFileSizeEx(f.file, &size) ||
        (f.size = UINT64(size.QuadPart), !ReadHeaders(f, *r, tracks))) {
        return {};
    }
    r->hwnd = hwnd;
    current = std::move(r);
    return tracks;
}

void WrapMkvStream(ComPtr<IMFByteStream>& stream) {
    if (current) stream = Microsoft::WRL::Make<Stream>(stream.Get(), current);
}

void StopMkvSubtitles() {
    if (!current) return;
    AcquireSRWLockExclusive(&current->lock);
    current->hwnd = nullptr;
    ReleaseSRWLockExclusive(&current->lock);
    current.reset();
}
