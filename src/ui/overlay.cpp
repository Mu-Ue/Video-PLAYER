#include "ui/overlay.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "media/player.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Run {
    ComPtr<ID2D1PathGeometry> outline;
    D2D1_POINT_2F origin;
};

struct {
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> round;
    std::vector<Run> runs;  // of the text being drawn
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
    if (SUCCEEDED(hr)) hr = GpuDevice()->QueryInterface(IID_PPV_ARGS(&dxgi));
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
    if (FAILED(hr)) g.context.Reset();
    return SUCCEEDED(hr);
}

ComPtr<IDWriteTextLayout> CreateLayout(std::wstring_view text, const std::vector<StyleSpan>& spans, float fontSize,
                                       float maxWidth, bool center) {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = g.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"",
                                            &format);
    if (SUCCEEDED(hr)) {
        format->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        hr = g.dwrite->CreateTextLayout(text.data(), UINT32(text.size()), format.Get(), maxWidth, 0, &layout);
    }
    if (FAILED(hr)) return nullptr;
    for (const StyleSpan& s : spans) {
        const DWRITE_TEXT_RANGE range{s.start, s.length};
        if (s.italic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (s.bold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
    }
    return layout;
}

// A single line of text centered vertically in its box.
ComPtr<IDWriteTextFormat> CreateFormat(const wchar_t* family, float size, bool bold, DWRITE_TEXT_ALIGNMENT align) {
    ComPtr<IDWriteTextFormat> format;
    const DWRITE_FONT_WEIGHT weight = bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
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
    static const wchar_t* const font = [] {
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 index;
        BOOL found = FALSE;
        if (SUCCEEDED(g.dwrite->GetSystemFontCollection(&fonts))) {
            fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &found);
        }
        return found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    return font;
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

// Creates a w x h surface and draws into it with draw(context, transform to the surface's pixels).
template <typename Draw>
ComPtr<IDCompositionSurface> Paint(IDCompositionDevice* dcomp, UINT w, UINT h, Draw draw) {
    ComPtr<IDCompositionSurface> surface;
    if (FAILED(dcomp->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface))) {
        return nullptr;
    }
    DeviceLock lock;
    // The surface may be part of a larger shared texture, at offset.
    POINT offset;
    ComPtr<IDXGISurface> target;
    if (FAILED(surface->BeginDraw(nullptr, IID_PPV_ARGS(&target), &offset))) return nullptr;
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
    return drawn ? surface : nullptr;
}

}  // namespace

SIZE DrawOutlinedText(IDCompositionDevice* dcomp, IDCompositionVisual* visual, std::wstring_view text,
                      const std::vector<StyleSpan>& spans, float fontSize, float maxWidth, bool center) {
    visual->SetContent(nullptr);
    if (text.empty() || !Init()) return {};
    const ComPtr<IDWriteTextLayout> layout = CreateLayout(text, spans, fontSize, maxWidth, center);
    DWRITE_TEXT_METRICS m;
    DWRITE_OVERHANG_METRICS o;
    if (!layout || FAILED(layout->GetMetrics(&m)) || FAILED(layout->GetOverhangMetrics(&o))) return {};

    // The ink box (overhangs are relative to the maxWidth x 0 layout box) and the logical box, plus the
    // outline, which reaches 1.1 stroke widths past the glyphs.
    const float stroke = std::max(1.5f, fontSize * 0.08f);
    const float pad = std::ceil(stroke * 1.1f) + 1;
    const float left = std::min(m.left, -o.left), top = std::min(m.top, -o.top);
    const float right = std::max(m.left + m.width, maxWidth + o.right), bottom = std::max(m.top + m.height, o.bottom);
    const UINT w = UINT(std::ceil(right - left + 2 * pad)), h = UINT(std::ceil(bottom - top + 2 * pad));

    g.runs.clear();
    if (FAILED(layout->Draw(nullptr, &collector, pad - left, pad - top))) return {};
    const ComPtr<IDCompositionSurface> surface = Paint(dcomp, w, h, [&](ID2D1DeviceContext* dc,
                                                                        const D2D1_MATRIX_3X2_F& base) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // A wide faint halo, a solid outline, then the white fill.
        const struct {
            float alpha, width;
        } passes[] = {{0.35f, stroke * 2.2f}, {0.9f, stroke}, {1, 0}};
        for (const auto& pass : passes) {
            g.brush->SetColor(pass.width > 0 ? D2D1::ColorF(0, 0, 0, pass.alpha) : D2D1::ColorF(1, 1, 1, 1));
            for (const Run& r : g.runs) {
                dc->SetTransform(D2D1::Matrix3x2F::Translation(r.origin.x, r.origin.y) * base);
                if (pass.width > 0) {
                    dc->DrawGeometry(r.outline.Get(), g.brush.Get(), pass.width, g.round.Get());
                } else {
                    dc->FillGeometry(r.outline.Get(), g.brush.Get());
                }
            }
        }
    });
    g.runs.clear();
    if (!surface) return {};
    visual->SetContent(surface.Get());
    return {LONG(w), LONG(h)};
}

void DrawBar(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
             const std::vector<BarItem>& items) {
    visual->SetContent(nullptr);
    if (width == 0 || height == 0 || !Init()) return;
    const ComPtr<IDWriteTextFormat> text = CreateFormat(L"Segoe UI", 12 * scale, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!text) return;

    const float inset = std::round(3 * scale), radius = 4 * scale;
    const ComPtr<IDCompositionSurface> surface = Paint(dcomp, width, height, [&](ID2D1DeviceContext* dc,
                                                                                 const D2D1_MATRIX_3X2_F&) {
        dc->Clear(D2D1::ColorF(0x202020));  // the dark title bar's color
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
            dc->DrawText(item.text.data(), UINT32(item.text.size()), text.Get(), r, g.brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    });
    visual->SetContent(surface.Get());
}

void DrawControls(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT width, UINT height, float scale,
                  const Controls& c) {
    visual->SetContent(nullptr);
    if (width == 0 || height == 0 || !Init()) return;
    const auto time = CreateFormat(L"Segoe UI", 13 * scale, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
    const auto title = CreateFormat(L"Segoe UI", 15 * scale, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    const auto icons = CreateFormat(IconFont(), 24 * scale, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    const auto number = CreateFormat(L"Segoe UI", 9 * scale, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (!time || !title || !icons || !number || FAILED(g.dwrite->CreateEllipsisTrimmingSign(title.Get(), &ellipsis))) {
        return;
    }
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    title->SetTrimming(&trimming, ellipsis.Get());

    const ComPtr<IDCompositionSurface> surface = Paint(dcomp, width, height, [&](ID2D1DeviceContext* dc,
                                                                                 const D2D1_MATRIX_3X2_F& base) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const D2D1_GRADIENT_STOP stops[] = {
            {0, D2D1::ColorF(0, 0, 0, 0)}, {0.5f, D2D1::ColorF(0, 0, 0, 0.5f)}, {1, D2D1::ColorF(0, 0, 0, 0.85f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> shade;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, UINT32(std::size(stops)), &collection)) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, float(height)}),
                                                    collection.Get(), &shade))) {
            dc->FillRectangle(D2D1::RectF(0, 0, float(width), float(height)), shade.Get());
        }

        // The rest of the track, the played part and the knob.
        const D2D1_RECT_F seek = RectF(c.seekBar);
        const float y = (seek.top + seek.bottom) / 2, half = (c.seekHot ? 3 : 2) * scale;
        const float x = seek.left + float(c.progress) * (seek.right - seek.left), knob = (c.seekHot ? 8 : 6) * scale;
        g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        dc->FillRectangle(D2D1::RectF(x, y - half, seek.right, y + half), g.brush.Get());
        g.brush->SetColor(D2D1::ColorF(0xe50914));
        dc->FillRectangle(D2D1::RectF(seek.left, y - half, x, y + half), g.brush.Get());
        dc->FillEllipse(D2D1::Ellipse({x, y}, knob, knob), g.brush.Get());

        g.brush->SetColor(D2D1::ColorF(0xffffff));
        dc->DrawText(c.time.data(), UINT32(c.time.size()), time.Get(), RectF(c.timeRect), g.brush.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->DrawText(c.title.data(), UINT32(c.title.size()), title.Get(), RectF(c.titleRect), g.brush.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        for (const BarItem& item : c.buttons) {
            const D2D1_RECT_F r = RectF(item.rect);
            const D2D1_POINT_2F center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            dc->SetTransform(item.hot ? D2D1::Matrix3x2F::Scale(1.2f, 1.2f, center) * base : base);
            g.brush->SetColor(D2D1::ColorF(item.dim ? 0x808080 : 0xffffff));
            if (item.replay) DrawReplayArrow(dc, center, 11 * scale, 2 * scale, item.replay);
            dc->DrawText(item.text.data(), UINT32(item.text.size()), item.replay ? number.Get() : icons.Get(), r,
                         g.brush.Get());
        }
    });
    visual->SetContent(surface.Get());
}
