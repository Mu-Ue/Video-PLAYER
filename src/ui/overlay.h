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

// A button: text on the menu bar, an icon glyph on the controls.
struct BarItem {
    RECT rect;  // in pixels from the surface's top left
    std::wstring text;
    int replay = 0;    // -1 or 1: a circular arrow back or forward around the text rather than a glyph
    bool hot = false;  // under the mouse
    bool dim = false;  // disabled
};

// Draws a dark bar with the items into a new surface and sets it as the visual's content. Zero size clears the
// visual. scale is the DPI over 96.
void DrawBar(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
             const std::vector<BarItem>& items);

// The playback controls laid over the bottom of the video.
struct Controls {
    RECT seekBar;     // the track runs along its middle
    double progress;  // 0..1 of the track played
    bool seekHot;     // the track is thicker
    RECT timeRect, titleRect;
    std::wstring time, title;
    std::vector<BarItem> buttons;  // hot ones are drawn larger
};

// Draws the controls, over a gradient that darkens toward the bottom edge, into a new transparent surface and
// sets it as the visual's content.
void DrawControls(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
                  const Controls& controls);
