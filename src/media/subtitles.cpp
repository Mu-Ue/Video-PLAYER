#include "media/subtitles.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "config.h"

namespace {

bool ReadAll(const std::wstring& path, std::string& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileType(file) == FILE_TYPE_DISK && GetFileSizeEx(file, &size) && size.QuadPart <= kMaxSubtitleBytes;
    if (ok) {
        bytes.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size();
    }
    CloseHandle(file);
    return ok;
}

// UTF-16 with a byte order mark, else UTF-8 (with or without one) if it is valid, else the ANSI code page.
std::wstring Decode(const std::string& bytes) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
        std::wstring text((n - 2) / 2, L'\0');
        memcpy(text.data(), p + 2, text.size() * sizeof(wchar_t));
        if (p[0] == 0xFE) {
            for (wchar_t& c : text) c = wchar_t(_byteswap_ushort(c));
        }
        return text;
    }
    const size_t skip = n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF ? 3 : 0;
    const char* src = bytes.data() + skip;
    const int srcLen = int(n - skip);
    UINT codePage = CP_UTF8;
    int len = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, src, srcLen, nullptr, 0);
    if (len == 0) {
        codePage = CP_ACP;
        len = MultiByteToWideChar(codePage, 0, src, srcLen, nullptr, 0);
    }
    std::wstring text(size_t(std::max(len, 0)), L'\0');
    if (len > 0) MultiByteToWideChar(codePage, 0, src, srcLen, text.data(), len);
    return text;
}

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool IsAsciiLetter(wchar_t c) { return (c | 0x20) >= L'a' && (c | 0x20) <= L'z'; }

void SkipSpaces(std::wstring_view s, size_t& i) {
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
}

// Parses "[hh:]mm:ss[,fff]" (or ".fff") at s[i] into seconds, moving i past it.
bool ParseTime(std::wstring_view s, size_t& i, double& seconds) {
    double total = 0;
    int parts = 0;
    for (;;) {
        int digits = 0;
        UINT64 value = 0;
        for (; i < s.size() && IsDigit(s[i]) && digits < 9; ++i, ++digits) value = value * 10 + (s[i] - L'0');
        if (digits == 0) return false;
        total = total * 60 + double(value);
        if (++parts < 3 && i < s.size() && s[i] == L':') {
            ++i;
            continue;
        }
        break;
    }
    if (parts < 2) return false;
    if (i < s.size() && (s[i] == L',' || s[i] == L'.')) {
        double scale = 0.1;
        for (++i; i < s.size() && IsDigit(s[i]); ++i, scale /= 10) total += (s[i] - L'0') * scale;
    }
    seconds = total;
    return true;
}

// Parses "start --> end", ignoring anything after it (such as position coordinates).
bool ParseTiming(std::wstring_view line, double& start, double& end) {
    size_t i = 0;
    SkipSpaces(line, i);
    if (!ParseTime(line, i, start)) return false;
    SkipSpaces(line, i);
    if (line.substr(i, 3) != L"-->") return false;
    i += 3;
    SkipSpaces(line, i);
    return ParseTime(line, i, end);
}

// Applies ASS-style override codes such as {\an8} or {\i1}.
void ApplyOverrides(std::wstring_view codes, Cue& cue, bool& italic, bool& bold) {
    for (size_t i = codes.find(L'\\'); i != std::wstring_view::npos; i = codes.find(L'\\', i + 1)) {
        const std::wstring_view code = codes.substr(i + 1, 3);
        if (code == L"an7" || code == L"an8" || code == L"an9") cue.top = true;
        if (code.size() >= 2 && (code[0] == L'i' || code[0] == L'b') && (code[1] == L'0' || code[1] == L'1')) {
            (code[0] == L'i' ? italic : bold) = code[1] == L'1';
        }
    }
}

}  // namespace

void SetCueText(std::wstring_view raw, Cue& cue) {
    std::wstring& out = cue.text;
    bool italic = false, bold = false;      // style of the text that follows
    bool spanItalic = false, spanBold = false;  // style of the text since spanStart
    size_t spanStart = 0;
    const auto flush = [&] {
        if (out.size() > spanStart && (spanItalic || spanBold)) {
            cue.spans.push_back({UINT32(spanStart), UINT32(out.size() - spanStart), spanItalic, spanBold});
        }
        spanStart = out.size();
        spanItalic = italic;
        spanBold = bold;
    };
    for (size_t i = 0; i < raw.size() && out.size() < kMaxCueChars;) {
        const wchar_t c = raw[i];
        const wchar_t close = c == L'<' ? L'>' : c == L'{' ? L'}' : 0;
        if (close) {
            const size_t end = raw.find(close, i + 1);
            if (end != std::wstring_view::npos && end - i <= 128) {
                std::wstring_view tag = raw.substr(i + 1, end - i - 1);
                bool consumed = true;
                if (c == L'<' && (tag.empty() || !(tag[0] == L'/' || IsAsciiLetter(tag[0])))) {
                    consumed = false;  // not a tag, like "<3"
                } else if (c == L'<') {
                    const bool closing = !tag.empty() && tag[0] == L'/';
                    if (closing) tag.remove_prefix(1);
                    if (tag == L"i" || tag == L"I") italic = !closing;
                    if (tag == L"b" || tag == L"B") bold = !closing;
                } else if (!tag.empty() && tag[0] == L'\\') {
                    ApplyOverrides(tag, cue, italic, bold);
                } else {
                    consumed = false;  // plain text in braces
                }
                if (consumed) {
                    i = end + 1;
                    if (italic != spanItalic || bold != spanBold) flush();
                    continue;
                }
            }
        } else if (c == L'\\' && i + 1 < raw.size() && (raw[i + 1] == L'N' || raw[i + 1] == L'n')) {
            out += L'\n';
            i += 2;
            continue;
        }
        out += c;
        ++i;
    }
    flush();
    while (!out.empty() && (out.back() == L'\n' || out.back() == L' ' || out.back() == L'\t')) out.pop_back();
    for (StyleSpan& s : cue.spans) s.length = std::min(s.length, UINT32(std::max(out.size(), size_t(s.start)) - s.start));
}

bool LoadSubtitles(const std::wstring& path, Subtitles& subs) {
    std::string bytes;
    if (!ReadAll(path, bytes)) return false;
    const std::wstring text = Decode(bytes);
    bytes = {};

    Subtitles parsed;
    Cue cue{};
    std::wstring raw;
    bool inCue = false;
    const auto finish = [&] {
        if (inCue && cue.end > cue.start) {
            SetCueText(raw, cue);
            if (!cue.text.empty()) parsed.cues.push_back(std::move(cue));
        }
        inCue = false;
    };
    // Tolerant: a timing line starts a cue (the index line before it is not needed) and a blank line ends it.
    for (size_t pos = 0; pos < text.size() && parsed.cues.size() < kMaxCues;) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring_view line(text.data() + pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);

        double start, end;
        if (line.find(L"-->") != std::wstring_view::npos && ParseTiming(line, start, end)) {
            finish();
            cue = {start, end};
            raw.clear();
            inCue = true;
        } else if (line.find_first_not_of(L" \t") == std::wstring_view::npos) {
            finish();
        } else if (inCue && raw.size() < kMaxCueChars * 4) {
            if (!raw.empty()) raw += L'\n';
            raw += line;
        }
    }
    finish();
    if (parsed.cues.empty()) return false;

    std::stable_sort(parsed.cues.begin(), parsed.cues.end(),
                     [](const Cue& a, const Cue& b) { return a.start < b.start; });
    parsed.reach.resize(parsed.cues.size());
    double reach = 0;
    for (size_t i = 0; i < parsed.cues.size(); ++i) parsed.reach[i] = reach = std::max(reach, parsed.cues[i].end);
    subs = std::move(parsed);
    return true;
}

bool AddCues(Subtitles& subs, std::vector<Cue> added) {
    auto& cues = subs.cues;
    const auto byStart = [](const Cue& a, const Cue& b) { return a.start < b.start; };
    std::stable_sort(added.begin(), added.end(), byStart);
    // They are appended, then merged in at once, as inserting each would move all after it.
    const size_t old = cues.size();
    size_t from = old;  // where the first one goes, from which on reach changes
    for (Cue& cue : added) {
        const auto same = [&](const Cue& c) { return c.end == cue.end && c.top == cue.top && c.text == cue.text; };
        const auto at = std::upper_bound(cues.begin(), cues.begin() + old, cue.start,
                                         [](double time, const Cue& c) { return time < c.start; });
        bool there = false;
        for (auto it = at; !there && it != cues.begin() && (it - 1)->start == cue.start;) there = same(*--it);
        for (size_t k = cues.size(); !there && k > old && cues[k - 1].start == cue.start;) there = same(cues[--k]);
        if (there) continue;
        from = std::min(from, size_t(at - cues.begin()));
        cues.push_back(std::move(cue));
    }
    if (cues.size() == old) return false;
    std::inplace_merge(cues.begin(), cues.begin() + old, cues.end(), byStart);
    subs.reach.resize(cues.size());
    for (size_t k = from; k < cues.size(); ++k) subs.reach[k] = std::max(k ? subs.reach[k - 1] : 0.0, cues[k].end);
    return true;
}

double ActiveCues(const Subtitles& subs, double t, std::vector<size_t>& active) {
    active.clear();
    const auto& cues = subs.cues;
    const size_t later = size_t(std::upper_bound(cues.begin(), cues.end(), t,
                                                 [](double time, const Cue& c) { return time < c.start; }) -
                                cues.begin());
    double next = later < cues.size() ? cues[later].start : INFINITY;
    // Cues before `later` have started; stop once none of the remaining ones lasts past t.
    for (size_t i = later; i-- > 0 && subs.reach[i] > t;) {
        if (cues[i].end > t) {
            active.push_back(i);
            next = std::min(next, cues[i].end);
        }
    }
    std::reverse(active.begin(), active.end());
    return next;
}
