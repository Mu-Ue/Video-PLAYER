#pragma once

#include <windows.h>

#include <string>

// The file dialog loads ~50 shell DLLs and caches that stay in the process until it exits, so it runs
// in a short-lived copy of this exe ("--dialog <owner>") that writes the chosen path to its stdout.

// Entry point of the helper process.
int DialogMain();

// Shows the open dialog in a helper process, modal to owner, for subtitle files only if subtitles is set; returns
// the chosen path, or an empty string.
std::wstring RunFileDialog(HWND owner, bool subtitles = false);
