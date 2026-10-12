#pragma once

#include <windows.h>

#include <string>
#include <vector>

// A run of italic and/or bold text within a cue.
struct StyleSpan {
    UINT32 start, length;
    bool italic, bold;
};

struct Cue {
    double start, end;  // seconds
    std::wstring text;  // lines separated by '\n', tags removed
    std::vector<StyleSpan> spans;
    bool top;  // {\an7}, {\an8} or {\an9}: shown at the top of the video
};

struct Subtitles {
    std::vector<Cue> cues;     // sorted by start
    std::vector<double> reach;  // reach[i] = latest end of cues[0..i]
};

// Reads an SRT file in UTF-8, UTF-16 or the ANSI code page. Returns false if it can't be read or has no cues.
bool LoadSubtitles(const std::wstring& path, Subtitles& subs);

// Adds a cue in start order, unless the same one is already there; returns true if added.
bool AddCue(Subtitles& subs, Cue cue);

// Sets active to the indices of the cues showing at time t, in start order, and returns the next time this
// changes (+infinity if never).
double ActiveCues(const Subtitles& subs, double t, std::vector<size_t>& active);
