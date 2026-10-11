#pragma once

#include <windows.h>
#include <dcomp.h>

#include <string>
#include <string_view>
#include <vector>

#include "media/subtitles.h"

// Draws text in white with a soft black outline into a new surface, sets it as the visual's content and returns
// the surface's size in pixels. Empty text clears the visual. Lines wrap at maxWidth and are centered or
// left-aligned. Direct2D and DirectWrite load on the first call that draws.
SIZE DrawOutlinedText(IDCompositionDevice* dcomp, IDCompositionVisual* visual, std::wstring_view text,
                      const std::vector<StyleSpan>& spans, float fontSize, float maxWidth, bool center);

// A button or label on a bar.
struct BarItem {
    float x, width;  // in pixels from the bar's left edge
    std::wstring text;
    bool icon = false;   // a Segoe MDL2 Assets glyph rather than Segoe UI text
    bool label = false;  // left-aligned text rather than a centered button
    bool hot = false;    // under the mouse
    bool dim = false;    // disabled
};

// Draws a dark bar with the items into a new surface and sets it as the visual's content. Zero size clears the
// visual. scale is the DPI over 96.
void DrawBar(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
             const std::vector<BarItem>& items);
