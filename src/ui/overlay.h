#pragma once

#include <windows.h>
#include <d3d11.h>  // also compares RECTs
#include <dcomp.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "media/subtitles.h"

// The device to draw with: the one the DirectComposition device was created on. Call before drawing.
void SetOverlayDevice(ID3D11Device* device);

// Draws text in white with a soft black outline into the visual's surface, sets it as the visual's content and
// returns the surface's size in pixels. Empty text clears the visual. Lines wrap at maxWidth and are centered or
// left-aligned. Direct2D and DirectWrite load on the first call that draws.
SIZE DrawOutlinedText(IDCompositionDevice* dcomp, IDCompositionVisual* visual, std::wstring_view text,
                      const std::vector<StyleSpan>& spans, float fontSize, float maxWidth, bool center);

// Outlined text as DrawOutlinedText draws it, but into images drawn onto video frames, with a Direct2D device of its
// own for the thread that presents them.
class FramePainter {
  public:
    FramePainter();
    ~FramePainter();
    bool Open(ID3D11Device* device);  // false if Direct2D can't draw on it
    // Draws centered text into the image of slot 0 to 3 and returns its size in pixels; empty text clears it.
    // Slots 0 and 1 are drawn onto frames; 2 and 3 wait for Swap.
    SIZE Prepare(int slot, std::wstring_view text, const std::vector<StyleSpan>& spans, float fontSize, float maxWidth);
    void Swap();  // slots 0 and 1 with 2 and 3
    // Blends the images onto the target texture with their top left corners at at[slot]. Holds on to the texture
    // until DropTarget, for the next call with it.
    bool Draw(ID3D11Texture2D* target, const POINT at[2]);
    // Lets go of the texture, as resizing it needs; from any thread, but never during Draw.
    void DropTarget();

  private:
    struct Kit;
    std::unique_ptr<Kit> kit_;
};

// A button: text on the menu bar, an icon glyph on the controls, or a row of the track panel.
struct BarItem {
    RECT rect;  // in pixels from the surface's top left
    std::wstring text;
    int replay = 0;    // -1 or 1: a circular arrow back or forward around the text rather than a glyph
    bool hot = false;  // under the mouse
    bool dim = false;  // disabled
    bool mirror = false;  // flipped left to right
    bool checked = false;  // a check mark left of the text
    bool operator==(const BarItem&) const = default;
};

// Draws a dark bar with the items into the visual's surface and sets it as the visual's content. Zero size clears
// the visual. scale is the DPI over 96.
void DrawBar(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
             const std::vector<BarItem>& items);

// The playback controls laid over the bottom of the video.
struct Controls {
    RECT seekBar;        // the track runs along its middle
    double progress;     // 0..1 of the track played
    double playback;     // 0..1 of the track where the video plays while the knob is dragged; -1 if not dragging
    bool seekHot;        // the track is thicker
    RECT volumeTrack;    // the volume slider's track runs along its middle; none if empty
    double volumeLevel;  // 0..1 of the volume track filled
    double volumeFull;   // 0..1 of the volume track at full volume, red past it where boosted
    RECT speedTrack;     // the speed slider's, the same way
    double speedLevel;
    RECT timeRect, titleRect;
    std::wstring time, title;
    std::vector<BarItem> buttons;  // hot ones are drawn larger
    bool operator==(const Controls&) const = default;
};

// Draws the controls, over a gradient that darkens toward the bottom edge, into the visual's transparent surface
// and sets it as the visual's content.
void DrawControls(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
                  const Controls& controls);

// Draws an icon glyph in white on a translucent black circle filling the visual's size x size surface, with a
// slash through it if asked, and sets it as the visual's content.
void DrawFlash(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT size, std::wstring_view glyph, bool slash);

// Shows a time (digits and colons) in white on a dark pill fitted to it, the seek bar preview's, in the visual and
// returns its size in pixels. The digits are drawn once and shown by child visuals, so a new time draws nothing
// unless the pill's width changes. For one visual only.
SIZE DrawTimeLabel(IDCompositionDevice* dcomp, IDCompositionVisual* visual, float scale, std::wstring_view text);

// Draws the soft shadow of a frame x radius rounded rect, the seek bar preview's, into the visual's surface, sets it
// as the visual's content and returns how far the surface reaches past the rect on each side, or -1 if not drawn.
int DrawPreviewShadow(IDCompositionDevice* dcomp, IDCompositionVisual* visual, float scale, SIZE frame, float radius);

// A column of the track panel: a header over rows, which are clipped to the rows rect. In pixels from the panel's
// top left.
struct TrackColumn {
    RECT header;
    std::wstring title;
    RECT rows;
    std::vector<BarItem> items;
    bool more = false;  // rows below, faded out at the bottom
    RECT thumb{};       // the scrollbar's thumb; empty if the rows fit
};

// Draws the audio and subtitle track panel, a dark box with the columns side by side, into the visual's surface
// and sets it as the visual's content.
void DrawTrackPanel(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
                    const std::vector<TrackColumn>& columns);

// The width in pixels of a track panel row's text, or of a header's.
float TrackTextWidth(std::wstring_view text, float scale, bool header);
