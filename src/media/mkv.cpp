#include "media/mkv.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_set>

#include "config.h"
#include "media/subtitles.h"

namespace {

// Element ids, with their length marker.
enum : UINT32 {
    kEbml = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB,
    kSeekId = 0x53AB, kSeekPosition = 0x53AC, kInfo = 0x1549A966, kTimestampScale = 0x2AD7B1,
    kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86,
    kLanguage = 0x22B59C, kLanguageBcp47 = 0x22B59D, kName = 0x536E, kDefaultDuration = 0x23E383,
    kContentEncodings = 0x6D80, kContentEncoding = 0x6240, kContentEncodingType = 0x5033,
    kContentCompression = 0x5034, kContentCompAlgo = 0x4254, kContentCompSettings = 0x4255,
    kCues = 0x1C53BB6B, kCuePoint = 0xBB, kCueTime = 0xB3, kCueTrackPositions = 0xB7, kCueTrack = 0xF7, kCueClusterPosition = 0xF1,
    kCueRelativePosition = 0xF0, kCluster = 0x1F43B675, kTimestamp = 0xE7, kBlockGroup = 0xA0, kBlock = 0xA1,
    kSimpleBlock = 0xA3, kBlockDuration = 0x9B,
};
constexpr UINT64 kUnknown = ~UINT64(0);  // an element size that isn't given, or a position that isn't
constexpr UINT64 kMaxHeaderBytes = 1 << 22;  // of the elements read whole: the file header, Info and Tracks
constexpr UINT64 kMaxBlockBytes = 1 << 16;   // of a subtitle block (group)

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
    bool listed = false;         // the index lists its blocks
    size_t count = 0;            // cues posted
    std::vector<Cue> batch;      // read but not yet posted
    size_t batchSize = 1;        // posted once it has this many, doubling each time
};

// An element in the file: its id, where it starts and its data's position and size.
struct Element {
    UINT32 id;
    UINT64 at, data, size;
};

// A position the index lists: its time, its cluster and the block's position in that, if given.
struct Entry {
    UINT64 time, cluster, relative;
};

std::atomic<double> seekTarget = -1;  // the seek the reader is yet to take, in seconds, if not negative

struct Reader {
    HANDLE file = INVALID_HANDLE_VALUE;
    UINT64 fileSize = 0;
    HWND hwnd = nullptr;
    UINT64 segment = 0, segmentEnd = 0;  // the Segment's data
    UINT64 cues = kUnknown, firstCluster = kUnknown;
    double scale = 1e-3;  // seconds per timestamp unit
    std::vector<TextTrack> tracks;
    std::unordered_set<UINT64> seen;  // text blocks read and clusters walked
    UINT64 timeCluster = kUnknown, timeData = 0, time = 0;  // the last cluster ClusterTime found
    const TextTrack* lastTrack = nullptr;  // of the last cue AddBlock read, and its end
    double lastEnd = 0;
    std::vector<BYTE> scan;  // Resync's buffer

    ~Reader() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    bool Read(UINT64 at, void* out, size_t size) {
        if (at > fileSize || size > fileSize - at) return false;
        OVERLAPPED o{};
        o.Offset = DWORD(at);
        o.OffsetHigh = DWORD(at >> 32);
        DWORD read = 0;
        return ReadFile(file, out, DWORD(size), &read, &o) && read == size;
    }

    bool Header(UINT64 at, Element& e) {
        BYTE head[12];
        if (at >= fileSize) return false;
        const size_t n = size_t(std::min<UINT64>(sizeof head, fileSize - at));
        if (!Read(at, head, n)) return false;
        const BYTE* p = head;
        if (!ParseHeader(p, head + n, e.id, e.size)) return false;
        e.at = at;
        e.data = at + UINT64(p - head);
        return true;
    }

    bool Data(const Element& e, std::vector<BYTE>& out, UINT64 max) {
        if (e.size > max) return false;
        out.resize(size_t(e.size));
        return Read(e.data, out.data(), out.size());
    }

    // The element with this id at `at`, read whole.
    bool ElementAt(UINT64 at, UINT32 id, std::vector<BYTE>& out, UINT64 max) {
        Element e;
        return Header(at, e) && e.id == id && Data(e, out, max);
    }

    TextTrack* Find(UINT64 number) {
        for (TextTrack& t : tracks) {
            if (t.number == number) return &t;
        }
        return nullptr;
    }

    void Post(TextTrack& t) {
        auto* batch = new std::vector<Cue>(std::move(t.batch));
        t.batch = {};
        if (!PostMessageW(hwnd, WM_APP_SUBTITLE_CUES, WPARAM(t.number), LPARAM(batch))) delete batch;
    }

    // Turns a block of a text track, with the duration of its group, into a cue.
    void AddBlock(const BYTE* p, const BYTE* end, UINT64 clusterTime, UINT64 duration) {
        UINT64 number;
        if (!Vint(p, end, number, false) || end - p < 3 || (p[2] & 0x06)) return;  // laced: not text
        TextTrack* t = Find(number);
        if (!t || t->count >= kMaxCues) return;
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
        if (cue.text.empty() || !(cue.end > cue.start)) return;
        ++t->count;
        lastTrack = t;
        lastEnd = cue.end;
        t->batch.push_back(std::move(cue));
        if (t->batch.size() >= t->batchSize) {
            Post(*t);
            t->batchSize *= 2;
        }
    }

    // Reads a SimpleBlock or BlockGroup if it is of a text track.
    void Block(const Element& e, UINT64 clusterTime) {
        // A text track's number, which leads a SimpleBlock and a group's Block, normally its first child, rules out
        // the video and audio without reading them.
        BYTE head[16];
        const size_t n = size_t(std::min<UINT64>(sizeof head, e.size));
        if (!Read(e.data, head, n)) return;
        const BYTE* p = head;
        UINT32 id = kBlock;
        UINT64 size, number;
        if (e.id == kBlockGroup && !ParseHeader(p, head + n, id, size)) return;
        if (id == kBlock && (!Vint(p, head + n, number, false) || !Find(number))) return;
        std::vector<BYTE> data;
        if (e.size > kMaxBlockBytes || !seen.insert(e.at).second || !Data(e, data, kMaxBlockBytes)) return;
        if (e.id == kSimpleBlock) {
            AddBlock(data.data(), data.data() + data.size(), clusterTime, kUnknown);
            return;
        }
        const BYTE* block = nullptr;
        size_t blockSize = 0;
        UINT64 duration = kUnknown;
        Children(data.data(), data.data() + data.size(), [&](UINT32 child, const BYTE* d, size_t s) {
            if (child == kBlock) block = d, blockSize = s;
            if (child == kBlockDuration) duration = UInt(d, s);
        });
        if (block) AddBlock(block, block + blockSize, clusterTime, duration);
    }

    // Reads the text blocks of the cluster at `at`, or skips the other element there. Returns where the next one
    // starts, or kUnknown if that can't be told.
    UINT64 Step(UINT64 at, const std::atomic<bool>& stop) {
        Element e;
        if (!Header(at, e) || e.data > segmentEnd) return kUnknown;
        const bool sized = e.size != kUnknown;
        const UINT64 end = sized ? std::min(segmentEnd, e.data + e.size) : segmentEnd;
        if (e.id != kCluster) return sized ? end : kUnknown;
        UINT64 clusterTime = kUnknown;
        for (UINT64 p = e.data; p < end && !stop.load(std::memory_order_relaxed); p = e.data + e.size) {
            if (!Header(p, e) || e.data > end) break;
            if (e.id == kCluster) return p;  // one whose size isn't given ends at the next
            if (e.size == kUnknown || e.size > end - e.data) break;
            if (e.id == kTimestamp && e.size <= 8) {
                BYTE v[8];
                if (!Read(e.data, v, size_t(e.size))) break;
                clusterTime = UInt(v, size_t(e.size));
            } else if ((e.id == kBlockGroup || e.id == kSimpleBlock) && clusterTime != kUnknown) {
                Block(e, clusterTime);
            }
        }
        return sized ? end : kUnknown;
    }

    // Finds the data position and Timestamp of the cluster at `at`: timeData and time.
    bool ClusterTime(UINT64 at) {
        if (timeCluster == at) return true;
        Element cluster, e;
        if (!Header(at, cluster) || cluster.id != kCluster) return false;
        // The Timestamp comes first, after a CRC-32 at most.
        UINT64 child = cluster.data;
        for (int i = 0; i < 2 && Header(child, e) && e.size != kUnknown; ++i, child = e.data + e.size) {
            if (e.id != kTimestamp) continue;
            BYTE v[8];
            if (e.size > 8 || !Read(e.data, v, size_t(e.size))) return false;
            timeCluster = at;
            timeData = cluster.data;
            time = UInt(v, size_t(e.size));
            return true;
        }
        return false;
    }

    // The first cluster that starts in [from, to), found by its id. The id can also occur by chance in other data,
    // so a hit counts only if a cluster's header and Timestamp follow.
    UINT64 Resync(UINT64 from, UINT64 to, const std::atomic<bool>& stop) {
        static constexpr BYTE kId[] = {0x1F, 0x43, 0xB6, 0x75};
        scan.resize(1 << 16);
        while (from < to && from < fileSize && !stop.load(std::memory_order_relaxed)) {
            const size_t n = size_t(std::min<UINT64>(scan.size(), fileSize - from));
            if (n < sizeof kId || !Read(from, scan.data(), n)) return kUnknown;
            const BYTE* end = scan.data() + n;
            for (const BYTE* p = scan.data(); (p = std::search(p, end, kId, std::end(kId))) != end; ++p) {
                const UINT64 at = from + UINT64(p - scan.data());
                if (at >= to) return kUnknown;
                if (ClusterTime(at)) return at;
            }
            from += n - (sizeof kId - 1);  // an id cut off at the end is read whole next time
        }
        return kUnknown;
    }

    // The last cluster whose Timestamp is at most units, found without an index by halving the bytes it can be in.
    UINT64 Bisect(UINT64 units, const std::atomic<bool>& stop) {
        UINT64 lo = firstCluster, hi = segmentEnd;  // lo is a cluster at most at units, the ones from hi on are later
        while (hi - lo > 1 && !stop.load(std::memory_order_relaxed)) {
            const UINT64 mid = lo + (hi - lo) / 2;
            const UINT64 at = Resync(mid, hi, stop);
            if (at != kUnknown && time <= units) {
                lo = at;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    // The seek the window asked for, in seconds, or a negative number. Posts what was read before it, and starts the
    // batches small again so the cues there arrive at once.
    double TakeSeek() {
        const double t = seekTarget.exchange(-1);
        if (t < 0) return t;
        for (TextTrack& track : tracks) {
            if (!track.batch.empty()) Post(track);
            track.batchSize = 1;
        }
        return t;
    }

    UINT64 Units(double seconds) const { return UINT64(std::min(seconds / scale, 1e18)); }

    // Reads the blocks the index lists, from the seek position on, and the ones before it after the end.
    void ReadBlocks(const std::vector<Entry>& blocks, const std::atomic<bool>& stop) {
        std::vector<bool> done(blocks.size());
        std::vector<std::pair<const TextTrack*, double>> last(blocks.size());  // the track and end of each's last cue
        size_t left = blocks.size(), next = 0;
        const auto read = [&](size_t i) {
            if (done[i]) return;
            done[i] = true;
            --left;
            lastTrack = nullptr;
            const Entry& entry = blocks[i];
            Element e;
            if (entry.relative != kUnknown && ClusterTime(entry.cluster) && Header(timeData + entry.relative, e) &&
                (e.id == kBlockGroup || e.id == kSimpleBlock)) {
                Block(e, time);
            } else if (seen.insert(entry.cluster).second) {
                Step(entry.cluster, stop);
            }
            last[i] = {lastTrack, lastEnd};
        };
        while (left && !stop.load(std::memory_order_relaxed)) {
            const double t = TakeSeek();
            if (t >= 0) {
                const UINT64 units = Units(t);
                next = size_t(std::upper_bound(blocks.begin(), blocks.end(), units,
                                               [](UINT64 u, const Entry& b) { return u < b.time; }) -
                              blocks.begin());
                // Back from there until each track has a cue that ended by then, for one that still shows.
                std::vector<const TextTrack*> ended;
                for (size_t i = next; i-- > 0 && ended.size() < tracks.size() && seekTarget < 0 &&
                                      !stop.load(std::memory_order_relaxed);) {
                    read(i);
                    const auto [track, end] = last[i];
                    if (track && end <= t && std::find(ended.begin(), ended.end(), track) == ended.end()) {
                        ended.push_back(track);
                    }
                }
            }
            if (next >= blocks.size()) next = 0;
            read(next++);
        }
    }

    // Reads the clusters from the seek position on, and the ones not yet read after the end, without the subtitles'
    // index. The clusters the index lists for other tracks, if any, give the position.
    void ReadClusters(const std::vector<Entry>& clusters, const std::atomic<bool>& stop) {
        std::map<UINT64, UINT64> walked;  // byte ranges read, merged: start to end
        const auto unread = [&](UINT64 at) {
            const auto it = walked.upper_bound(at);
            return it != walked.begin() && std::prev(it)->second > at ? std::prev(it)->second : at;
        };
        UINT64 at = firstCluster;
        while (!stop.load(std::memory_order_relaxed)) {
            const double t = TakeSeek();
            if (t >= 0) {
                // One cluster before the one at t, for a cue that started there and still shows.
                const UINT64 units = Units(t);
                if (!clusters.empty()) {
                    const size_t n = size_t(std::upper_bound(clusters.begin(), clusters.end(), units,
                                                             [](UINT64 u, const Entry& c) { return u < c.time; }) -
                                            clusters.begin());
                    at = n >= 2 ? clusters[n - 2].cluster : firstCluster;
                } else {
                    at = Bisect(units, stop);
                    if (at != firstCluster && ClusterTime(at) && time) at = Bisect(time - 1, stop);
                }
            }
            at = unread(at);
            if (at >= segmentEnd) at = unread(firstCluster);
            if (at >= segmentEnd) return;
            UINT64 next = Step(at, stop);
            if (stop.load(std::memory_order_relaxed)) return;
            if (next == kUnknown || next <= at) next = Resync(at + 1, segmentEnd, stop);
            next = std::min(next, segmentEnd);
            // Marks [at, next) read, merged with the ranges it touches.
            UINT64 from = at, to = next;
            auto it = walked.lower_bound(from);
            if (it != walked.begin() && std::prev(it)->second >= from) from = (--it)->first;
            for (; it != walked.end() && it->first <= to; it = walked.erase(it)) to = std::max(to, it->second);
            walked[from] = to;
            at = next;
        }
    }

    void Run(const std::atomic<bool>& stop) {
        std::vector<Entry> blocks, clusters;  // the index's text blocks and clusters of any track, by time
        std::vector<BYTE> index;
        if (cues != kUnknown && ElementAt(cues, kCues, index, kMaxSubtitleBytes)) {
            Children(index.data(), index.data() + index.size(), [&](UINT32 id, const BYTE* d, size_t s) {
                if (id != kCuePoint) return;
                UINT64 cueTime = kUnknown;
                const size_t firstBlock = blocks.size(), firstPoint = clusters.size();
                Children(d, d + s, [&](UINT32 id2, const BYTE* d2, size_t s2) {
                    if (id2 == kCueTime) cueTime = UInt(d2, s2);
                    if (id2 != kCueTrackPositions) return;
                    TextTrack* t = nullptr;
                    UINT64 cluster = kUnknown, relative = kUnknown;
                    Children(d2, d2 + s2, [&](UINT32 field, const BYTE* v, size_t n) {
                        if (field == kCueTrack) t = Find(UInt(v, n));
                        if (field == kCueClusterPosition) cluster = segment + UInt(v, n);
                        if (field == kCueRelativePosition) relative = UInt(v, n);
                    });
                    if (cluster == kUnknown) return;
                    clusters.push_back({0, cluster, kUnknown});
                    if (!t) return;
                    t->listed = true;
                    blocks.push_back({0, cluster, relative});
                });
                for (size_t i = firstBlock; i < blocks.size(); ++i) blocks[i].time = cueTime;
                for (size_t i = firstPoint; i < clusters.size(); ++i) clusters[i].time = cueTime;
            });
        }
        index = {};
        const auto byTime = [](const Entry& a, const Entry& b) { return a.time < b.time; };
        if (std::all_of(tracks.begin(), tracks.end(), [](const TextTrack& t) { return t.listed; })) {
            clusters = {};
            std::stable_sort(blocks.begin(), blocks.end(), byTime);
            ReadBlocks(blocks, stop);
        } else if (firstCluster != kUnknown) {
            blocks = {};
            std::stable_sort(clusters.begin(), clusters.end(), byTime);
            clusters.erase(std::unique(clusters.begin(), clusters.end(),
                                       [](const Entry& a, const Entry& b) { return a.cluster == b.cluster; }),
                           clusters.end());
            ReadClusters(clusters, stop);
        }
        for (TextTrack& t : tracks) {
            if (!t.batch.empty() && !stop.load(std::memory_order_relaxed)) Post(t);
        }
    }
};

// Reads the file header, Info and Tracks, which come before the first cluster, and finds the index.
bool ReadHeaders(Reader& r, std::vector<Track>& out) {
    Element e;
    std::vector<BYTE> data;
    if (!r.Header(0, e) || e.id != kEbml || !r.Data(e, data, kMaxHeaderBytes)) return false;
    bool matroska = false;
    Children(data.data(), data.data() + data.size(), [&](UINT32 id, const BYTE* d, size_t s) {
        if (id == kDocType) matroska = Equals(d, s, "matroska") || Equals(d, s, "webm");
    });
    if (!matroska || !r.Header(e.data + e.size, e) || e.id != kSegment) return false;
    r.segment = e.data;
    r.segmentEnd = e.size != kUnknown && e.size <= r.fileSize - e.data ? e.data + e.size : r.fileSize;

    UINT64 info = kUnknown, tracks = kUnknown;
    for (UINT64 at = r.segment; at < r.segmentEnd && r.Header(at, e); at = e.data + e.size) {
        if (e.id == kCluster) {
            r.firstCluster = at;
            break;
        }
        if (e.size == kUnknown) break;
        if (e.id == kInfo) info = at;
        if (e.id == kTracks) tracks = at;
        if (e.id == kCues) r.cues = at;
        if (e.id == kSeekHead && r.Data(e, data, kMaxHeaderBytes)) {
            Children(data.data(), data.data() + data.size(), [&](UINT32 id, const BYTE* d, size_t s) {
                if (id != kSeek) return;
                UINT64 target = 0, position = kUnknown;
                Children(d, d + s, [&](UINT32 field, const BYTE* v, size_t n) {
                    if (field == kSeekId) target = UInt(v, n);
                    if (field == kSeekPosition) position = r.segment + UInt(v, n);
                });
                if (position == kUnknown) return;
                if (target == kInfo && info == kUnknown) info = position;
                if (target == kTracks && tracks == kUnknown) tracks = position;
                if (target == kCues && r.cues == kUnknown) r.cues = position;
            });
        }
    }
    if (info != kUnknown && r.ElementAt(info, kInfo, data, kMaxHeaderBytes)) {
        Children(data.data(), data.data() + data.size(), [&](UINT32 id, const BYTE* d, size_t s) {
            if (id == kTimestampScale && UInt(d, s)) r.scale = double(UInt(d, s)) * 1e-9;
        });
    }
    if (tracks == kUnknown || !r.ElementAt(tracks, kTracks, data, kMaxHeaderBytes)) return false;

    bool readable = true;
    Children(data.data(), data.data() + data.size(), [&](UINT32 id, const BYTE* d, size_t s) {
        if (id != kTrackEntry) return;
        TextTrack t{};
        UINT64 type = 0;
        bool text = false, encoded = false;
        Track track{};
        std::wstring language, bcp47;
        Children(d, d + s, [&](UINT32 field, const BYTE* v, size_t n) {
            switch (field) {
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

std::thread reader;
std::atomic<bool> stopReader = false;

}  // namespace

std::vector<Track> StartMkvSubtitles(const std::wstring& path, HWND hwnd) {
    StopMkvSubtitles();
    auto r = std::make_unique<Reader>();
    r->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    LARGE_INTEGER size{};
    std::vector<Track> tracks;
    if (r->file == INVALID_HANDLE_VALUE || GetFileType(r->file) != FILE_TYPE_DISK || !GetFileSizeEx(r->file, &size) ||
        (r->fileSize = UINT64(size.QuadPart), !ReadHeaders(*r, tracks))) {
        return {};
    }
    r->hwnd = hwnd;
    reader = std::thread([r = std::move(r)] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // its reads yield to playback's
        r->Run(stopReader);
    });
    return tracks;
}

void SeekMkvSubtitles(double seconds) {
    seekTarget = std::max(0.0, seconds);
}

void StopMkvSubtitles() {
    seekTarget = -1;
    if (!reader.joinable()) return;
    stopReader = true;
    reader.join();
    stopReader = false;
}
