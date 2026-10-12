#include "ui/overlay.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 kPanelColor = 0x202020;  // the dark title bar's

struct Run {
    ComPtr<ID2D1PathGeometry> outline;
    D2D1_POINT_2F origin;
};

// A visual's surface, kept and redrawn in place while the visual is redrawn.
struct Target {
    IDCompositionVisual* visual;
    ComPtr<IDCompositionVirtualSurface> surface;
    UINT width, height;
};

struct {
    ComPtr<ID3D11Device> device;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1LinearGradientBrush> shade;  // under the controls
    ComPtr<ID2D1LinearGradientBrush> fade;   // over the bottom of a track panel column that scrolls
    ComPtr<ID2D1StrokeStyle> round;
    ComPtr<ID2D1Effect> shadow;  // under the outlined text
    std::vector<Run> runs;  // of the text being drawn
    std::vector<Target> targets;
    // The text formats of the bars and controls, for one DPI scale.
    float scale = 0;
    ComPtr<IDWriteTextFormat> menu, time, title, icons, number, header, row, check;
    ComPtr<IDWriteInlineObject> ellipsis, rowEllipsis;
    // The outlined text's format, for one size.
    float textSize = 0;
    ComPtr<IDWriteTextFormat> text;
} g;

// Collects each glyph run's outline, so all outlines are stroked before any is filled and no stroke covers a
// neighbor's fill.
class OutlineCollector final : public IDWriteTextRenderer {
  public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
            riid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    STDMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        *transform = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    STDMETHODIMP GetPixelsPerDip(void*, FLOAT* pixels) override {
        *pixels = 1;
        return S_OK;
    }
    STDMETHODIMP DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                              const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        Run r{nullptr, {x, y}};
        ComPtr<ID2D1GeometrySink> sink;
        HRESULT hr = g.factory->CreatePathGeometry(&r.outline);
        if (SUCCEEDED(hr)) hr = r.outline->Open(&sink);
        if (SUCCEEDED(hr)) {
            hr = run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances,
                                                   run->glyphOffsets, run->glyphCount, run->isSideways,
                                                   run->bidiLevel % 2, sink.Get());
        }
        if (SUCCEEDED(hr)) hr = sink->Close();
        if (SUCCEEDED(hr)) g.runs.push_back(std::move(r));
        return hr;
    }
    STDMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    STDMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    STDMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
        return E_NOTIMPL;
    }
} collector;

bool Init() {
    if (g.context) return true;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g.factory.GetAddressOf());
    if (SUCCEEDED(hr)) hr = g.device.As(&dxgi);
    if (SUCCEEDED(hr)) hr = g.factory->CreateDevice(dxgi.Get(), &device);
    if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.context);
    if (SUCCEEDED(hr)) {
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(g.dwrite.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) hr = g.context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g.brush);
    if (SUCCEEDED(hr)) {
        hr = g.factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                      D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                          nullptr, 0, &g.round);
    }
    if (SUCCEEDED(hr)) hr = g.context->CreateEffect(CLSID_D2D1Shadow, &g.shadow);
    const D2D1_GRADIENT_STOP stops[] = {
        {0, D2D1::ColorF(0, 0, 0, 0)}, {0.5f, D2D1::ColorF(0, 0, 0, 0.5f)}, {1, D2D1::ColorF(0, 0, 0, 0.85f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    if (SUCCEEDED(hr)) hr = g.context->CreateGradientStopCollection(stops, UINT32(std::size(stops)), &collection);
    if (SUCCEEDED(hr)) {
        hr = g.context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, 1}),
                                                  collection.Get(), &g.shade);
    }
    const D2D1_GRADIENT_STOP fade[] = {{0, D2D1::ColorF(kPanelColor, 0)}, {1, D2D1::ColorF(kPanelColor, 1)}};
    collection.Reset();
    if (SUCCEEDED(hr)) hr = g.context->CreateGradientStopCollection(fade, UINT32(std::size(fade)), &collection);
    if (SUCCEEDED(hr)) {
        hr = g.context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, 1}),
                                                  collection.Get(), &g.fade);
    }
    if (FAILED(hr)) g.context.Reset();
    return SUCCEEDED(hr);
}

bool IsFontInstalled(const wchar_t* family) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index;
    BOOL found = FALSE;
    if (SUCCEEDED(g.dwrite->GetSystemFontCollection(&fonts))) fonts->FindFamilyName(family, &index, &found);
    return found;
}

// Netflix's font, for the subtitles and the track panel, if installed, else the closest one Windows has.
const wchar_t* SubtitleFont() {
    static const wchar_t* const font = IsFontInstalled(L"Netflix Sans") ? L"Netflix Sans" : L"Segoe UI";
    return font;
}

ComPtr<IDWriteTextLayout> CreateLayout(std::wstring_view text, const std::vector<StyleSpan>& spans, float fontSize,
                                       float maxWidth, bool center) {
    if (!g.text || g.textSize != fontSize) {
        g.text.Reset();
        if (FAILED(g.dwrite->CreateTextFormat(SubtitleFont(), nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"",
                                              &g.text))) {
            return nullptr;
        }
        g.textSize = fontSize;
    }
    ComPtr<IDWriteTextLayout> layout;
    g.text->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    const HRESULT hr =
        g.dwrite->CreateTextLayout(text.data(), UINT32(text.size()), g.text.Get(), maxWidth, 0, &layout);
    if (FAILED(hr)) return nullptr;
    for (const StyleSpan& s : spans) {
        const DWRITE_TEXT_RANGE range{s.start, s.length};
        if (s.italic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (s.bold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_BLACK, range);
    }
    return layout;
}

// A single line of text centered vertically in its box.
ComPtr<IDWriteTextFormat> CreateFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                       DWRITE_TEXT_ALIGNMENT align) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(g.dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          size, L"", &format))) {
        return nullptr;
    }
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return format;
}

// Windows 11's icon font, or Windows 10's, which has the same code points.
const wchar_t* IconFont() {
    static const wchar_t* const font =
        IsFontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    return font;
}

// Creates the bars' and controls' text formats for a DPI scale, unless they already are for it.
bool SetScale(float scale) {
    if (scale == g.scale) return true;
    g.scale = 0;
    constexpr auto normal = DWRITE_FONT_WEIGHT_NORMAL, semibold = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    g.menu = CreateFormat(L"Segoe UI", 12 * scale, normal, DWRITE_TEXT_ALIGNMENT_CENTER);
    g.time = CreateFormat(L"Segoe UI", 13 * scale, normal, DWRITE_TEXT_ALIGNMENT_LEADING);
    g.title = CreateFormat(L"Segoe UI", 15 * scale, semibold, DWRITE_TEXT_ALIGNMENT_CENTER);
    g.icons = CreateFormat(IconFont(), 24 * scale, normal, DWRITE_TEXT_ALIGNMENT_CENTER);
    g.number = CreateFormat(L"Segoe UI", 9 * scale, semibold, DWRITE_TEXT_ALIGNMENT_CENTER);
    // Netflix's proportions: headers 1.4 times the rows.
    g.header = CreateFormat(SubtitleFont(), 21 * scale, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    g.row = CreateFormat(SubtitleFont(), 15 * scale, normal, DWRITE_TEXT_ALIGNMENT_LEADING);
    g.check = CreateFormat(IconFont(), 16 * scale, normal, DWRITE_TEXT_ALIGNMENT_CENTER);
    g.ellipsis.Reset();
    g.rowEllipsis.Reset();
    if (!g.menu || !g.time || !g.title || !g.icons || !g.number || !g.header || !g.row || !g.check ||
        FAILED(g.dwrite->CreateEllipsisTrimmingSign(g.title.Get(), &g.ellipsis)) ||
        FAILED(g.dwrite->CreateEllipsisTrimmingSign(g.row.Get(), &g.rowEllipsis))) {
        return false;
    }
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    g.title->SetTrimming(&trimming, g.ellipsis.Get());
    g.row->SetTrimming(&trimming, g.rowEllipsis.Get());
    g.scale = scale;
    return true;
}

D2D1_RECT_F RectF(const RECT& r) { return D2D1::RectF(float(r.left), float(r.top), float(r.right), float(r.bottom)); }

// A circular arrow around center, open at the top, with its head at the top pointing back (dir -1) or forward.
void DrawReplayArrow(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float radius, float stroke, int dir) {
    const float m = float(-dir), x = center.x, y = center.y - radius, head = stroke * 2;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(g.factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    sink->BeginFigure({x, y}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment({x - m * radius * 0.866f, center.y - radius * 0.5f}, {radius, radius}, 0,
                                  m > 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                  D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({x - m * head, y}, D2D1_FIGURE_BEGIN_FILLED);
    const D2D1_POINT_2F base[] = {{x + m * head * 0.5f, y - head}, {x + m * head * 0.5f, y + head}};
    sink->AddLines(base, 2);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;
    dc->DrawGeometry(path.Get(), g.brush.Get(), stroke, g.round.Get());
    dc->FillGeometry(path.Get(), g.brush.Get());
}

// The visual's surface, sized w x h: the one it had, resized if need be, or a new one.
IDCompositionSurface* SurfaceFor(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT w, UINT h) {
    auto it = std::find_if(g.targets.begin(), g.targets.end(), [&](const Target& t) { return t.visual == visual; });
    if (it == g.targets.end()) {
        ComPtr<IDCompositionVirtualSurface> surface;
        if (FAILED(dcomp->CreateVirtualSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                                               &surface))) {
            return nullptr;
        }
        g.targets.push_back({visual, std::move(surface), w, h});
        return g.targets.back().surface.Get();
    }
    if ((it->width != w || it->height != h) && FAILED(it->surface->Resize(w, h))) return nullptr;
    it->width = w;
    it->height = h;
    return it->surface.Get();
}

// Redraws the visual's w x h surface with draw(context, transform to the surface's pixels) and shows it, or
// clears the visual and returns false if that fails.
template <typename Draw>
bool Paint(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT w, UINT h, Draw draw) {
    IDCompositionSurface* surface = SurfaceFor(dcomp, visual, w, h);
    // The surface may be part of a larger shared texture, at offset.
    POINT offset;
    ComPtr<IDXGISurface> target;
    if (!surface || FAILED(surface->BeginDraw(nullptr, IID_PPV_ARGS(&target), &offset))) {
        visual->SetContent(nullptr);
        return false;
    }
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> bitmap;
    const bool drawn = SUCCEEDED(g.context->CreateBitmapFromDxgiSurface(target.Get(), &props, &bitmap));
    if (drawn) {
        ID2D1DeviceContext* dc = g.context.Get();
        dc->SetTarget(bitmap.Get());
        dc->BeginDraw();
        const D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Translation(float(offset.x), float(offset.y));
        dc->SetTransform(base);
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, float(w), float(h)), D2D1_ANTIALIAS_MODE_ALIASED);
        draw(dc, base);
        dc->SetTransform(base);
        dc->PopAxisAlignedClip();
        dc->EndDraw();
        dc->SetTarget(nullptr);
    }
    surface->EndDraw();
    visual->SetContent(drawn ? surface : nullptr);
    return drawn;
}

}  // namespace

void SetOverlayDevice(ID3D11Device* device) { g.device = device; }

SIZE DrawOutlinedText(IDCompositionDevice* dcomp, IDCompositionVisual* visual, std::wstring_view text,
                      const std::vector<StyleSpan>& spans, float fontSize, float maxWidth, bool center) {
    const ComPtr<IDWriteTextLayout> layout =
        text.empty() || !Init() ? nullptr : CreateLayout(text, spans, fontSize, maxWidth, center);
    DWRITE_TEXT_METRICS m;
    DWRITE_OVERHANG_METRICS o;
    if (!layout || FAILED(layout->GetMetrics(&m)) || FAILED(layout->GetOverhangMetrics(&o))) {
        visual->SetContent(nullptr);
        return {};
    }

    // The ink box (overhangs are relative to the maxWidth x 0 layout box) and the logical box, plus the shadow,
    // which reaches 3 standard deviations of its blur past its offset glyphs.
    const float stroke = std::max(1.0f, fontSize * 0.03f), blur = std::max(1.5f, fontSize * 0.06f);
    const float drop = std::round(fontSize * 0.03f);
    const float pad = std::ceil(blur * 3 + drop) + 1;
    const float left = std::min(m.left, -o.left), top = std::min(m.top, -o.top);
    const float right = std::max(m.left + m.width, maxWidth + o.right), bottom = std::max(m.top + m.height, o.bottom);
    const UINT w = UINT(std::ceil(right - left + 2 * pad)), h = UINT(std::ceil(bottom - top + 2 * pad));

    g.runs.clear();
    if (FAILED(layout->Draw(nullptr, &collector, pad - left, pad - top))) {
        visual->SetContent(nullptr);
        return {};
    }
    // Each pass over all runs: the glyphs filled (width 0) or stroked.
    const auto drawRuns = [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base, float width) {
        for (const Run& r : g.runs) {
            dc->SetTransform(D2D1::Matrix3x2F::Translation(r.origin.x, r.origin.y) * base);
            if (width > 0) {
                dc->DrawGeometry(r.outline.Get(), g.brush.Get(), width, g.round.Get());
            } else {
                dc->FillGeometry(r.outline.Get(), g.brush.Get());
            }
        }
    };
    // The glyphs' shapes, for the shadow effect to blur.
    ComPtr<ID2D1CommandList> shapes;
    ID2D1DeviceContext* context = g.context.Get();
    if (SUCCEEDED(context->CreateCommandList(&shapes))) {
        context->SetTarget(shapes.Get());
        context->BeginDraw();
        g.brush->SetColor(D2D1::ColorF(0, 0, 0, 1));
        drawRuns(context, D2D1::Matrix3x2F::Identity(), stroke);
        drawRuns(context, D2D1::Matrix3x2F::Identity(), 0);
        if (FAILED(context->EndDraw()) || FAILED(shapes->Close())) shapes.Reset();
        context->SetTarget(nullptr);
    }
    const bool drawn = Paint(dcomp, visual, w, h, [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // A soft drop shadow, a faint thin outline that keeps the edges on bright video, then the white fill.
        if (shapes) {
            g.shadow->SetInput(0, shapes.Get());
            g.shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur);
            g.shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.9f));
            dc->SetTransform(D2D1::Matrix3x2F::Translation(drop * 0.5f, drop) * base);
            dc->DrawImage(g.shadow.Get());
            g.shadow->SetInput(0, nullptr);
        }
        g.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
        drawRuns(dc, base, stroke);
        g.brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
        drawRuns(dc, base, 0);
    });
    g.runs.clear();
    return drawn ? SIZE{LONG(w), LONG(h)} : SIZE{};
}

void DrawBar(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
             const std::vector<BarItem>& items) {
    if (width == 0 || height == 0 || !Init() || !SetScale(scale)) {
        visual->SetContent(nullptr);
        return;
    }
    const float inset = std::round(3 * scale), radius = 4 * scale;
    Paint(dcomp, visual, width, height, [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F&) {
        dc->Clear(D2D1::ColorF(kPanelColor));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        for (const BarItem& item : items) {
            const D2D1_RECT_F r = RectF(item.rect);
            if (item.hot) {
                g.brush->SetColor(D2D1::ColorF(0x3a3a3a));
                dc->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(r.left, r.top + inset, r.right, r.bottom - inset), radius, radius),
                    g.brush.Get());
            }
            g.brush->SetColor(D2D1::ColorF(item.dim ? 0x707070 : 0xffffff));
            dc->DrawText(item.text.data(), UINT32(item.text.size()), g.menu.Get(), r, g.brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    });
}

void DrawControls(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
                  const Controls& c) {
    if (width == 0 || height == 0 || !Init() || !SetScale(scale)) {
        visual->SetContent(nullptr);
        return;
    }
    Paint(dcomp, visual, width, height, [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g.shade->SetEndPoint({0, float(height)});
        dc->FillRectangle(D2D1::RectF(0, 0, float(width), float(height)), g.shade.Get());

        // The rest of the track, the played part and the knob.
        const D2D1_RECT_F seek = RectF(c.seekBar);
        const float y = (seek.top + seek.bottom) / 2, half = (c.seekHot ? 3 : 2) * scale;
        const float x = seek.left + float(c.progress) * (seek.right - seek.left), knob = (c.seekHot ? 8 : 6) * scale;
        g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        dc->FillRectangle(D2D1::RectF(x, y - half, seek.right, y + half), g.brush.Get());
        g.brush->SetColor(D2D1::ColorF(0xe50914));
        dc->FillRectangle(D2D1::RectF(seek.left, y - half, x, y + half), g.brush.Get());
        dc->FillEllipse(D2D1::Ellipse({x, y}, knob, knob), g.brush.Get());

        // The volume slider, the same way in white.
        if (c.volumeTrack.right > c.volumeTrack.left) {
            const D2D1_RECT_F v = RectF(c.volumeTrack);
            const float vy = (v.top + v.bottom) / 2, vhalf = 1.5f * scale;
            const float vx = v.left + float(c.volumeLevel) * (v.right - v.left);
            g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
            dc->FillRectangle(D2D1::RectF(vx, vy - vhalf, v.right, vy + vhalf), g.brush.Get());
            g.brush->SetColor(D2D1::ColorF(0xffffff));
            dc->FillRectangle(D2D1::RectF(v.left, vy - vhalf, vx, vy + vhalf), g.brush.Get());
            dc->FillEllipse(D2D1::Ellipse({vx, vy}, 6 * scale, 6 * scale), g.brush.Get());
        }

        g.brush->SetColor(D2D1::ColorF(0xffffff));
        dc->DrawText(c.time.data(), UINT32(c.time.size()), g.time.Get(), RectF(c.timeRect), g.brush.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->DrawText(c.title.data(), UINT32(c.title.size()), g.title.Get(), RectF(c.titleRect), g.brush.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        for (const BarItem& item : c.buttons) {
            const D2D1_RECT_F r = RectF(item.rect);
            const D2D1_POINT_2F center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            const float zoom = item.hot ? 1.2f : 1;
            dc->SetTransform(D2D1::Matrix3x2F::Scale(item.mirror ? -zoom : zoom, zoom, center) * base);
            g.brush->SetColor(D2D1::ColorF(item.dim ? 0x808080 : 0xffffff));
            if (item.replay) DrawReplayArrow(dc, center, 11 * scale, 2 * scale, item.replay);
            dc->DrawText(item.text.data(), UINT32(item.text.size()), item.replay ? g.number.Get() : g.icons.Get(),
                         r, g.brush.Get());
        }
    });
}

void DrawTrackPanel(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
                    const std::vector<TrackColumn>& columns) {
    if (width == 0 || height == 0 || !Init() || !SetScale(scale)) {
        visual->SetContent(nullptr);
        return;
    }
    // Like Netflix's: a plain box, white headers, gray rows that turn white when selected or under the mouse.
    const float check = 30 * scale, fade = 50 * scale;
    Paint(dcomp, visual, width, height, [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F&) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g.brush->SetColor(D2D1::ColorF(kPanelColor));
        // Rounded like the menus, whose corners Windows rounds by 8 pixels.
        const float radius = 8 * scale;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, float(width), float(height)), radius, radius),
                                 g.brush.Get());
        for (const TrackColumn& c : columns) {
            const D2D1_RECT_F rows = RectF(c.rows);
            D2D1_RECT_F header = RectF(c.header);
            header.left += check;
            g.brush->SetColor(D2D1::ColorF(0xffffff));
            dc->DrawText(c.title.data(), UINT32(c.title.size()), g.header.Get(), header, g.brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
            dc->PushAxisAlignedClip(rows, D2D1_ANTIALIAS_MODE_ALIASED);
            for (const BarItem& item : c.items) {
                const D2D1_RECT_F r = RectF(item.rect);
                g.brush->SetColor(D2D1::ColorF(item.checked || item.hot ? 0xffffff : 0xb3b3b3));
                if (item.checked) {
                    dc->DrawText(L"\xE73E", 1, g.check.Get(), D2D1::RectF(r.left, r.top, r.left + check, r.bottom),
                                 g.brush.Get());
                }
                dc->DrawText(item.text.data(), UINT32(item.text.size()), g.row.Get(),
                             D2D1::RectF(r.left + check, r.top, r.right - 8 * scale, r.bottom), g.brush.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (c.more) {  // the rows below fade into the box
                g.fade->SetStartPoint({0, rows.bottom - fade});
                g.fade->SetEndPoint({0, rows.bottom});
                dc->FillRectangle(D2D1::RectF(rows.left, rows.bottom - fade, rows.right, rows.bottom), g.fade.Get());
            }
            dc->PopAxisAlignedClip();
            if (c.thumb.bottom > c.thumb.top) {
                g.brush->SetColor(D2D1::ColorF(0x414141));
                dc->FillRectangle(RectF(c.thumb), g.brush.Get());
            }
        }
    });
}

float TrackTextWidth(std::wstring_view text, float scale, bool header) {
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS m;
    if (!Init() || !SetScale(scale) ||
        FAILED(g.dwrite->CreateTextLayout(text.data(), UINT32(text.size()), (header ? g.header : g.row).Get(), 1e6f,
                                          0, &layout)) ||
        FAILED(layout->GetMetrics(&m))) {
        return 0;
    }
    return m.widthIncludingTrailingWhitespace;
}
