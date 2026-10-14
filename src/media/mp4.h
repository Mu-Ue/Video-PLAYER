#pragma once

#include <windows.h>
#include <mfmediaengine.h>

// A media engine extension for MPEG-4 files whose video track's edit list starts it later than its first sample,
// as B-frame files do. Media Foundation ignores the edit, which leaves the video late against the sound. The
// extension hands the engine a source that moves the video's samples earlier by the edit; it leaves every other
// file to the engine. Lives for the whole process.
IMFMediaEngineExtension* Mp4Extension();
