#pragma once

#include <windows.h>
#include <dcomp.h>

#include <string_view>
#include <vector>

#include "media/subtitles.h"

// Draws text in white with a soft black outline into a new surface, sets it as the visual's content and returns
// the surface's size in pixels. Empty text clears the visual. Lines wrap at maxWidth and are centered or
// left-aligned. Direct2D and DirectWrite load on the first call that draws.
SIZE DrawOutlinedText(IDCompositionDevice* dcomp, IDCompositionVisual* visual, std::wstring_view text,
                      const std::vector<StyleSpan>& spans, float fontSize, float maxWidth, bool center);
