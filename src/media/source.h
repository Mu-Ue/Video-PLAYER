#pragma once

#include <windows.h>
#include <mfmediaengine.h>

// A media engine extension that hands the engine Media Foundation's own source for each file, with its samples'
// times fixed so the picture and the sound play in sync:
// - Tracks that start later than their first samples: B-frame video, whose MPEG-4 edit list starts it later, and
//   audio with an encoder delay (AAC's priming), which the MPEG-4 edit list or the Matroska CodecDelay skip. Media
//   Foundation ignores both.
// - The sound as a whole, moved earlier by the audio renderer's buffer, which the engine's clock runs ahead by.
// - Uncompressed audio, cut exactly where playback starts.
// Lives for the whole process.
IMFMediaEngineExtension* SourceExtension();
// Starts finding what the first file needs from the audio output, and making that file's source, on threads of
// their own, so its opening doesn't wait on that. path may be null for none; it is opened as a local file. Call once,
// before the first file.
void PrepareSourceExtension(const wchar_t* path);
