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
    ComPtr<IDWriteTextFormat> text, icons;
    HRESULT hr = g.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 12 * scale, L"", &text);
    if (SUCCEEDED(hr)) {
        hr = g.dwrite->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16 * scale, L"", &icons);
    }
    if (FAILED(hr)) return;
    text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    icons->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    icons->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    const float h = float(height), inset = std::round(3 * scale), radius = 4 * scale;
    const ComPtr<IDCompositionSurface> surface = Paint(dcomp, width, height, [&](ID2D1DeviceContext* dc,
                                                                                 const D2D1_MATRIX_3X2_F&) {
        dc->Clear(D2D1::ColorF(0x202020));  // the dark title bar's color
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        for (const BarItem& item : items) {
            const D2D1_RECT_F r = D2D1::RectF(item.x, 0, item.x + item.width, h);
            if (item.hot) {
                g.brush->SetColor(D2D1::ColorF(0x3a3a3a));
                dc->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(r.left, inset, r.right, h - inset), radius, radius), g.brush.Get());
            }
            IDWriteTextFormat* format = item.icon ? icons.Get() : text.Get();
            if (!item.icon) {
                format->SetTextAlignment(item.label ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            g.brush->SetColor(D2D1::ColorF(item.dim ? 0x707070 : 0xffffff));
            dc->DrawText(item.text.data(), UINT32(item.text.size()), format, r, g.brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    });
    visual->SetContent(surface.Get());
}
