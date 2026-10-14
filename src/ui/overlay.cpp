#include "ui/overlay.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

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

// Direct2D on one device, for one thread, with what outlined text needs.
struct TextKit {
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> round;
    ComPtr<ID2D1Effect> shadow;  // under the outlined text
    std::vector<Run> runs;       // of the text being drawn
    // The outlined text's format, for one size.
    float textSize = 0;
    ComPtr<IDWriteTextFormat> text;
};

struct {
    ComPtr<ID3D11Device> device;
    TextKit kit;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1LinearGradientBrush> shade;  // under the controls
    ComPtr<ID2D1LinearGradientBrush> fade;   // over the bottom of a track panel column that scrolls
    std::vector<Target> targets;
    // The text formats of the bars and controls, for one DPI scale.
    float scale = 0;
    ComPtr<IDWriteTextFormat> menu, time, title, icons, number, header, row, check;
    ComPtr<IDWriteInlineObject> ellipsis, rowEllipsis;
    ComPtr<IDWriteTypography> tabular;  // digits all one width, null until first used
} g;

// The shared DirectWrite factory, which any thread can use.
IDWriteFactory* DWrite() {
    static const ComPtr<IDWriteFactory> factory = [] {
        ComPtr<IDWriteFactory> f;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(f.GetAddressOf()));
        return f;
    }();
    return factory.Get();
}

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
    // context is the TextKit to collect into.
    STDMETHODIMP DrawGlyphRun(void* context, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                              const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        TextKit& kit = *static_cast<TextKit*>(context);
        Run r{nullptr, {x, y}};
        ComPtr<ID2D1GeometrySink> sink;
        HRESULT hr = kit.factory->CreatePathGeometry(&r.outline);
        if (SUCCEEDED(hr)) hr = r.outline->Open(&sink);
        if (SUCCEEDED(hr)) {
            hr = run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances,
                                                   run->glyphOffsets, run->glyphCount, run->isSideways,
                                                   run->bidiLevel % 2, sink.Get());
        }
        if (SUCCEEDED(hr)) hr = sink->Close();
        if (SUCCEEDED(hr)) kit.runs.push_back(std::move(r));
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

// Opens Direct2D on the device for the kit's thread, or for any thread if multithreaded. Returns false, with the
// kit's context null, if it can't.
bool InitKit(TextKit& kit, ID3D11Device* device, bool multithreaded = false) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> d2d;
    HRESULT hr = D2D1CreateFactory(multithreaded ? D2D1_FACTORY_TYPE_MULTI_THREADED : D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   kit.factory.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = kit.factory->CreateDevice(dxgi.Get(), &d2d);
    if (SUCCEEDED(hr)) hr = d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &kit.context);
    if (SUCCEEDED(hr) && !DWrite()) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = kit.context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &kit.brush);
    if (SUCCEEDED(hr)) {
        hr = kit.factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                            nullptr, 0, &kit.round);
    }
    if (SUCCEEDED(hr)) hr = kit.context->CreateEffect(CLSID_D2D1Shadow, &kit.shadow);
    if (FAILED(hr)) kit.context.Reset();
    return SUCCEEDED(hr);
}

bool Init() {
    if (g.kit.context) return true;
    g.dwrite = DWrite();
    HRESULT hr = g.device && InitKit(g.kit, g.device.Get()) ? S_OK : E_FAIL;
    const D2D1_GRADIENT_STOP stops[] = {
        {0, D2D1::ColorF(0, 0, 0, 0)}, {0.5f, D2D1::ColorF(0, 0, 0, 0.5f)}, {1, D2D1::ColorF(0, 0, 0, 0.85f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    if (SUCCEEDED(hr)) hr = g.kit.context->CreateGradientStopCollection(stops, UINT32(std::size(stops)), &collection);
    if (SUCCEEDED(hr)) {
        hr = g.kit.context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, 1}),
                                                  collection.Get(), &g.shade);
    }
    const D2D1_GRADIENT_STOP fade[] = {{0, D2D1::ColorF(kPanelColor, 0)}, {1, D2D1::ColorF(kPanelColor, 1)}};
    collection.Reset();
    if (SUCCEEDED(hr)) hr = g.kit.context->CreateGradientStopCollection(fade, UINT32(std::size(fade)), &collection);
    if (SUCCEEDED(hr)) {
        hr = g.kit.context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, 1}),
                                                  collection.Get(), &g.fade);
    }
    if (FAILED(hr)) g.kit.context.Reset();
    return SUCCEEDED(hr);
}

bool IsFontInstalled(const wchar_t* family) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index;
    BOOL found = FALSE;
    if (SUCCEEDED(DWrite()->GetSystemFontCollection(&fonts))) fonts->FindFamilyName(family, &index, &found);
    return found;
}

// Netflix's font, for the subtitles and the track panel, if installed, else the closest one Windows has.
const wchar_t* SubtitleFont() {
    static const wchar_t* const font = IsFontInstalled(L"Netflix Sans") ? L"Netflix Sans" : L"Segoe UI";
    return font;
}

ComPtr<IDWriteTextLayout> CreateLayout(TextKit& kit, std::wstring_view text, const std::vector<StyleSpan>& spans,
                                       float fontSize, float maxWidth, bool center) {
    if (!kit.text || kit.textSize != fontSize) {
        kit.text.Reset();
        if (FAILED(DWrite()->CreateTextFormat(SubtitleFont(), nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"",
                                              &kit.text))) {
            return nullptr;
        }
        kit.textSize = fontSize;
    }
    ComPtr<IDWriteTextLayout> layout;
    kit.text->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    const HRESULT hr =
        DWrite()->CreateTextLayout(text.data(), UINT32(text.size()), kit.text.Get(), maxWidth, 0, &layout);
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
    if (FAILED(g.kit.factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
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
    dc->DrawGeometry(path.Get(), g.kit.brush.Get(), stroke, g.kit.round.Get());
    dc->FillGeometry(path.Get(), g.kit.brush.Get());
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
    const bool drawn = SUCCEEDED(g.kit.context->CreateBitmapFromDxgiSurface(target.Get(), &props, &bitmap));
    if (drawn) {
        ID2D1DeviceContext* dc = g.kit.context.Get();
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

// The time label's glyphs, kGlyphs drawn once per scale into the surface of a visual of their own, a cell apart.
// The label shows each character as a slice of it, so a new time only moves visuals and draws nothing.
constexpr wchar_t kGlyphs[] = L"0123456789:";
constexpr int kGlyphCount = int(std::size(kGlyphs)) - 1;
struct {
    float scale = 0;
    ComPtr<IDCompositionVisual> owner;  // never shown
    IDCompositionSurface* surface = nullptr;
    int cell = 0, pad = 0;
    int advance[kGlyphCount]{};  // in whole pixels, so every glyph lands on the pixel grid
    UINT height = 0;
    UINT pillWidth = 0;  // the label's background, as last drawn
    std::vector<ComPtr<IDCompositionVisual>> chars;
    std::vector<std::pair<int, int>> shown;  // each char's glyph (-1 for none) and x, as last set
} glyphs;

bool DrawGlyphs(IDCompositionDevice* dcomp, float scale) {
    if (glyphs.surface && glyphs.scale == scale) return true;
    glyphs.surface = nullptr;
    glyphs.pillWidth = 0;
    if (!glyphs.owner && FAILED(dcomp->CreateVisual(&glyphs.owner))) return false;
    // Digits of one width keep the label one size while the time under the mouse changes.
    if (!g.tabular && SUCCEEDED(g.dwrite->CreateTypography(&g.tabular)) &&
        FAILED(g.tabular->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1}))) {
        g.tabular.Reset();
    }
    const UINT h = UINT(std::round(26 * scale));
    ComPtr<IDWriteTextLayout> layouts[kGlyphCount];
    int widest = 0;
    for (int i = 0; i < kGlyphCount; ++i) {
        DWRITE_TEXT_METRICS m;
        if (FAILED(g.dwrite->CreateTextLayout(&kGlyphs[i], 1, g.time.Get(), 1e6f, float(h), &layouts[i]))) {
            return false;
        }
        if (g.tabular) layouts[i]->SetTypography(g.tabular.Get(), {0, 1});
        if (FAILED(layouts[i]->GetMetrics(&m))) return false;
        glyphs.advance[i] = int(std::lround(m.widthIncludingTrailingWhitespace));
        widest = std::max(widest, glyphs.advance[i]);
    }
    const int pad = int(std::ceil(2 * scale)), cell = widest + 2 * pad;  // room for strokes past the advance
    const bool drawn = Paint(dcomp, glyphs.owner.Get(), UINT(cell * kGlyphCount), h,
                             [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F&) {
                                 dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                                 dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                                 g.kit.brush->SetColor(D2D1::ColorF(0xffffff));
                                 for (int i = 0; i < kGlyphCount; ++i) {
                                     dc->DrawTextLayout(D2D1::Point2F(float(i * cell + pad), 0), layouts[i].Get(),
                                                        g.kit.brush.Get());
                                 }
                             });
    if (!drawn) return false;
    glyphs.surface = SurfaceFor(dcomp, glyphs.owner.Get(), UINT(cell * kGlyphCount), h);
    glyphs.cell = cell;
    glyphs.pad = pad;
    glyphs.height = h;
    glyphs.scale = scale;
    for (const auto& c : glyphs.chars) c->SetContent(glyphs.surface);
    glyphs.shown.assign(glyphs.chars.size(), {-1, INT_MIN});  // the slices moved: set them all again
    return true;
}

}  // namespace

void SetOverlayDevice(ID3D11Device* device) { g.device = device; }

namespace {

// Lays out and outlines text in white with a soft black outline, then has paint(w, h, draw) draw it into a w x h
// target with draw(context, transform to the target's pixels). Returns the size, or 0 x 0 if there is nothing to
// draw or paint fails.
template <typename PaintFn>
SIZE OutlineText(TextKit& kit, std::wstring_view text, const std::vector<StyleSpan>& spans, float fontSize,
                 float maxWidth, bool center, PaintFn paint) {
    const ComPtr<IDWriteTextLayout> layout =
        text.empty() ? nullptr : CreateLayout(kit, text, spans, fontSize, maxWidth, center);
    DWRITE_TEXT_METRICS m;
    DWRITE_OVERHANG_METRICS o;
    if (!layout || FAILED(layout->GetMetrics(&m)) || FAILED(layout->GetOverhangMetrics(&o))) return {};

    // The ink box (overhangs are relative to the maxWidth x 0 layout box) and the logical box, plus the shadow,
    // which reaches 3 standard deviations of its blur past its offset glyphs.
    const float stroke = std::max(1.0f, fontSize * 0.03f), blur = std::max(1.5f, fontSize * 0.06f);
    const float drop = std::round(fontSize * 0.03f);
    const float pad = std::ceil(blur * 3 + drop) + 1;
    const float left = std::min(m.left, -o.left), top = std::min(m.top, -o.top);
    const float right = std::max(m.left + m.width, maxWidth + o.right), bottom = std::max(m.top + m.height, o.bottom);
    const UINT w = UINT(std::ceil(right - left + 2 * pad)), h = UINT(std::ceil(bottom - top + 2 * pad));

    kit.runs.clear();
    if (FAILED(layout->Draw(&kit, &collector, pad - left, pad - top))) return {};
    // Each pass over all runs: the glyphs filled (width 0) or stroked.
    const auto drawRuns = [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base, float width) {
        for (const Run& r : kit.runs) {
            dc->SetTransform(D2D1::Matrix3x2F::Translation(r.origin.x, r.origin.y) * base);
            if (width > 0) {
                dc->DrawGeometry(r.outline.Get(), kit.brush.Get(), width, kit.round.Get());
            } else {
                dc->FillGeometry(r.outline.Get(), kit.brush.Get());
            }
        }
    };
    // The glyphs' shapes, for the shadow effect to blur.
    ComPtr<ID2D1CommandList> shapes;
    ID2D1DeviceContext* context = kit.context.Get();
    if (SUCCEEDED(context->CreateCommandList(&shapes))) {
        context->SetTarget(shapes.Get());
        context->BeginDraw();
        kit.brush->SetColor(D2D1::ColorF(0, 0, 0, 1));
        drawRuns(context, D2D1::Matrix3x2F::Identity(), stroke);
        drawRuns(context, D2D1::Matrix3x2F::Identity(), 0);
        if (FAILED(context->EndDraw()) || FAILED(shapes->Close())) shapes.Reset();
        context->SetTarget(nullptr);
    }
    const bool drawn = paint(w, h, [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // A soft drop shadow, a faint thin outline that keeps the edges on bright video, then the white fill.
        if (shapes) {
            kit.shadow->SetInput(0, shapes.Get());
            kit.shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur);
            kit.shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.9f));
            dc->SetTransform(D2D1::Matrix3x2F::Translation(drop * 0.5f, drop) * base);
            dc->DrawImage(kit.shadow.Get());
            kit.shadow->SetInput(0, nullptr);
        }
        kit.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
        drawRuns(dc, base, stroke);
        kit.brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
        drawRuns(dc, base, 0);
    });
    kit.runs.clear();
    return drawn ? SIZE{LONG(w), LONG(h)} : SIZE{};
}

}  // namespace

SIZE DrawOutlinedText(IDCompositionDevice* dcomp, IDCompositionVisual* visual, std::wstring_view text,
                      const std::vector<StyleSpan>& spans, float fontSize, float maxWidth, bool center) {
    const SIZE s = text.empty() || !Init() ? SIZE{}
                                           : OutlineText(g.kit, text, spans, fontSize, maxWidth, center,
                                                         [&](UINT w, UINT h, const auto& draw) {
                                                             return Paint(dcomp, visual, w, h, draw);
                                                         });
    if (!s.cx) visual->SetContent(nullptr);
    return s;
}


struct FramePainter::Kit : TextKit {
    ComPtr<ID2D1Bitmap1> images[2];
    SIZE sizes[2]{};
    ComPtr<ID2D1Bitmap1> target;  // drawing into texture, kept for the next frame
    ID3D11Texture2D* texture = nullptr;
};

FramePainter::FramePainter() = default;
FramePainter::~FramePainter() = default;

bool FramePainter::Open(ID3D11Device* device) {
    kit_ = std::make_unique<Kit>();
    // Multithreaded, as the window thread lets go of the target.
    if (!InitKit(*kit_, device, true)) kit_.reset();
    return kit_ != nullptr;
}

SIZE FramePainter::Prepare(int slot, std::wstring_view text, const std::vector<StyleSpan>& spans, float fontSize,
                           float maxWidth) {
    if (!kit_) return {};
    Kit& kit = *kit_;
    kit.images[slot].Reset();
    kit.sizes[slot] = OutlineText(kit, text, spans, fontSize, maxWidth, true, [&](UINT w, UINT h, const auto& draw) {
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> image;
        if (FAILED(kit.context->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &image))) return false;
        kit.context->SetTarget(image.Get());
        kit.context->BeginDraw();
        draw(kit.context.Get(), D2D1::Matrix3x2F::Identity());
        const bool drawn = SUCCEEDED(kit.context->EndDraw());
        kit.context->SetTarget(nullptr);
        if (drawn) kit.images[slot] = std::move(image);
        return drawn;
    });
    return kit.sizes[slot];
}

bool FramePainter::Draw(ID3D11Texture2D* target, const POINT at[2]) {
    if (!kit_ || (!kit_->images[0] && !kit_->images[1])) return true;
    Kit& kit = *kit_;
    if (target != kit.texture) {
        DropTarget();
        ComPtr<IDXGISurface> surface;
        const D2D1_BITMAP_PROPERTIES1 props =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(target->QueryInterface(IID_PPV_ARGS(&surface))) ||
            FAILED(kit.context->CreateBitmapFromDxgiSurface(surface.Get(), &props, &kit.target))) {
            return false;
        }
        kit.texture = target;
    }
    kit.context->SetTarget(kit.target.Get());
    kit.context->BeginDraw();
    kit.context->SetTransform(D2D1::Matrix3x2F::Identity());
    for (int i = 0; i < 2; ++i) {
        if (!kit.images[i]) continue;
        const D2D1_SIZE_F s = kit.images[i]->GetSize();
        const float x = float(at[i].x), y = float(at[i].y);
        kit.context->DrawBitmap(kit.images[i].Get(), D2D1::RectF(x, y, x + s.width, y + s.height), 1.0f,
                                D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    const bool drawn = SUCCEEDED(kit.context->EndDraw());
    kit.context->SetTarget(nullptr);
    return drawn;
}

void FramePainter::DropTarget() {
    if (!kit_) return;
    kit_->target.Reset();
    kit_->texture = nullptr;
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
                g.kit.brush->SetColor(D2D1::ColorF(0x3a3a3a));
                dc->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(r.left, r.top + inset, r.right, r.bottom - inset), radius, radius),
                    g.kit.brush.Get());
            }
            g.kit.brush->SetColor(D2D1::ColorF(item.dim ? 0x707070 : 0xffffff));
            dc->DrawText(item.text.data(), UINT32(item.text.size()), g.menu.Get(), r, g.kit.brush.Get(),
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
        const float y = (seek.top + seek.bottom) / 2, half = (c.seekHot ? 4 : 2.5f) * scale;
        const float x = seek.left + float(c.progress) * (seek.right - seek.left), knob = (c.seekHot ? 8 : 6) * scale;
        g.kit.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        dc->FillRectangle(D2D1::RectF(x, y - half, seek.right, y + half), g.kit.brush.Get());
        g.kit.brush->SetColor(D2D1::ColorF(0xe50914));
        dc->FillRectangle(D2D1::RectF(seek.left, y - half, x, y + half), g.kit.brush.Get());
        dc->FillEllipse(D2D1::Ellipse({x, y}, knob, knob), g.kit.brush.Get());

        // The volume slider, the same way in white.
        if (c.volumeTrack.right > c.volumeTrack.left) {
            const D2D1_RECT_F v = RectF(c.volumeTrack);
            const float vy = (v.top + v.bottom) / 2, vhalf = 1.5f * scale;
            const float vx = v.left + float(c.volumeLevel) * (v.right - v.left);
            g.kit.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
            dc->FillRectangle(D2D1::RectF(vx, vy - vhalf, v.right, vy + vhalf), g.kit.brush.Get());
            g.kit.brush->SetColor(D2D1::ColorF(0xffffff));
            dc->FillRectangle(D2D1::RectF(v.left, vy - vhalf, vx, vy + vhalf), g.kit.brush.Get());
            dc->FillEllipse(D2D1::Ellipse({vx, vy}, 6 * scale, 6 * scale), g.kit.brush.Get());
        }

        g.kit.brush->SetColor(D2D1::ColorF(0xffffff));
        dc->DrawText(c.time.data(), UINT32(c.time.size()), g.time.Get(), RectF(c.timeRect), g.kit.brush.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc->DrawText(c.title.data(), UINT32(c.title.size()), g.title.Get(), RectF(c.titleRect), g.kit.brush.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        for (const BarItem& item : c.buttons) {
            const D2D1_RECT_F r = RectF(item.rect);
            const D2D1_POINT_2F center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            const float zoom = item.hot ? 1.2f : 1;
            dc->SetTransform(D2D1::Matrix3x2F::Scale(item.mirror ? -zoom : zoom, zoom, center) * base);
            g.kit.brush->SetColor(D2D1::ColorF(item.dim ? 0x808080 : 0xffffff));
            if (item.replay) DrawReplayArrow(dc, center, 11 * scale, 2 * scale, item.replay);
            dc->DrawText(item.text.data(), UINT32(item.text.size()), item.replay ? g.number.Get() : g.icons.Get(),
                         r, g.kit.brush.Get());
        }
    });
}

void DrawFlash(IDCompositionDevice* dcomp, IDCompositionVisual* visual, UINT size, std::wstring_view glyph,
               bool slash) {
    const float s = float(size);
    const ComPtr<IDWriteTextFormat> icon =
        size && Init() ? CreateFormat(IconFont(), s * 0.42f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER)
                       : nullptr;
    if (!icon) {
        visual->SetContent(nullptr);
        return;
    }
    Paint(dcomp, visual, size, size, [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F&) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g.kit.brush->SetColor(D2D1::ColorF(0, 0, 0, 0.5f));
        dc->FillEllipse(D2D1::Ellipse({s / 2, s / 2}, s / 2, s / 2), g.kit.brush.Get());
        g.kit.brush->SetColor(D2D1::ColorF(0xffffff));
        dc->DrawText(glyph.data(), UINT32(glyph.size()), icon.Get(), D2D1::RectF(0, 0, s, s), g.kit.brush.Get());
        if (slash) {
            const float a = s * 0.3f, b = s - a;
            dc->DrawLine({a, a}, {b, b}, g.kit.brush.Get(), s * 0.035f, g.kit.round.Get());
        }
    });
}

SIZE DrawTimeLabel(IDCompositionDevice* dcomp, IDCompositionVisual* visual, float scale, std::wstring_view text) {
    if (!Init() || !SetScale(scale) || !DrawGlyphs(dcomp, scale)) {
        visual->SetContent(nullptr);
        return {};
    }
    const auto glyph = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : 10; };
    int total = 0;
    for (const wchar_t c : text) total += glyphs.advance[glyph(c)];
    const UINT w = UINT(std::ceil(total + 20 * scale)), h = glyphs.height;
    if (w != glyphs.pillWidth) {
        const bool drawn = Paint(dcomp, visual, w, h, [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F&) {
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            const float r = h / 2.0f;
            g.kit.brush->SetColor(D2D1::ColorF(kPanelColor, 0.9f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, float(w), float(h)), r, r), g.kit.brush.Get());
        });
        glyphs.pillWidth = drawn ? w : 0;
        if (!drawn) return {};
    }
    // Each character shows its glyph's slice of the glyph surface, side by side. Only the characters that changed
    // are set again; a sweep mostly changes the last digit or two.
    int x = int(w - total) / 2;
    size_t k = 0;
    for (; k < text.size(); ++k) {
        if (k == glyphs.chars.size()) {
            ComPtr<IDCompositionVisual> v;
            if (FAILED(dcomp->CreateVisual(&v)) || FAILED(visual->AddVisual(v.Get(), TRUE, nullptr))) break;
            v->SetContent(glyphs.surface);
            glyphs.chars.push_back(std::move(v));
            glyphs.shown.emplace_back(-1, INT_MIN);
        }
        const int i = glyph(text[k]);
        if (glyphs.shown[k] != std::pair{i, x}) {
            glyphs.shown[k] = {i, x};
            const float left = float(i * glyphs.cell + glyphs.pad);
            glyphs.chars[k]->SetOffsetX(x - left);
            glyphs.chars[k]->SetClip(D2D1::RectF(left, 0, left + glyphs.advance[i], float(h)));
        }
        x += glyphs.advance[i];
    }
    for (; k < glyphs.chars.size(); ++k) {
        if (glyphs.shown[k].first < 0) continue;
        glyphs.shown[k] = {-1, INT_MIN};
        glyphs.chars[k]->SetClip(D2D1::RectF(0, 0, 0, 0));
    }
    return {LONG(w), LONG(h)};
}

int DrawPreviewShadow(IDCompositionDevice* dcomp, IDCompositionVisual* visual, float scale, SIZE frame, float radius) {
    // The rect's shape, for the shadow effect to blur.
    ComPtr<ID2D1CommandList> shape;
    bool recorded = frame.cx > 0 && frame.cy > 0 && Init() && SUCCEEDED(g.kit.context->CreateCommandList(&shape));
    if (recorded) {
        ID2D1DeviceContext* context = g.kit.context.Get();
        context->SetTarget(shape.Get());
        context->BeginDraw();
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        g.kit.brush->SetColor(D2D1::ColorF(0, 0, 0, 1));
        context->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0, 0, float(frame.cx), float(frame.cy)), radius, radius), g.kit.brush.Get());
        recorded = SUCCEEDED(context->EndDraw()) && SUCCEEDED(shape->Close());
        context->SetTarget(nullptr);
    }
    if (!recorded) {
        visual->SetContent(nullptr);
        return -1;
    }
    // Faint and soft, a little lower than the frame, like Netflix's.
    const float blur = 8 * scale, drop = 2 * scale;
    const int pad = int(std::ceil(3 * blur + drop));
    const bool drawn = Paint(dcomp, visual, UINT(frame.cx + 2 * pad), UINT(frame.cy + 2 * pad),
                             [&](ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& base) {
                                 dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                                 g.kit.shadow->SetInput(0, shape.Get());
                                 g.kit.shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur);
                                 g.kit.shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.5f));
                                 dc->SetTransform(D2D1::Matrix3x2F::Translation(float(pad), pad + drop) * base);
                                 dc->DrawImage(g.kit.shadow.Get());
                                 g.kit.shadow->SetInput(0, nullptr);
                             });
    return drawn ? pad : -1;
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
        g.kit.brush->SetColor(D2D1::ColorF(kPanelColor));
        // Rounded like the menus, whose corners Windows rounds by 8 pixels.
        const float radius = 8 * scale;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, float(width), float(height)), radius, radius),
                                 g.kit.brush.Get());
        for (const TrackColumn& c : columns) {
            const D2D1_RECT_F rows = RectF(c.rows);
            D2D1_RECT_F header = RectF(c.header);
            header.left += check;
            g.kit.brush->SetColor(D2D1::ColorF(0xffffff));
            dc->DrawText(c.title.data(), UINT32(c.title.size()), g.header.Get(), header, g.kit.brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
            dc->PushAxisAlignedClip(rows, D2D1_ANTIALIAS_MODE_ALIASED);
            for (const BarItem& item : c.items) {
                const D2D1_RECT_F r = RectF(item.rect);
                g.kit.brush->SetColor(D2D1::ColorF(item.checked || item.hot ? 0xffffff : 0xb3b3b3));
                if (item.checked) {
                    dc->DrawText(L"\xE73E", 1, g.check.Get(), D2D1::RectF(r.left, r.top, r.left + check, r.bottom),
                                 g.kit.brush.Get());
                }
                dc->DrawText(item.text.data(), UINT32(item.text.size()), g.row.Get(),
                             D2D1::RectF(r.left + check, r.top, r.right - 8 * scale, r.bottom), g.kit.brush.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (c.more) {  // the rows below fade into the box
                g.fade->SetStartPoint({0, rows.bottom - fade});
                g.fade->SetEndPoint({0, rows.bottom});
                dc->FillRectangle(D2D1::RectF(rows.left, rows.bottom - fade, rows.right, rows.bottom), g.fade.Get());
            }
            dc->PopAxisAlignedClip();
            if (c.thumb.bottom > c.thumb.top) {
                g.kit.brush->SetColor(D2D1::ColorF(0x414141));
                dc->FillRectangle(RectF(c.thumb), g.kit.brush.Get());
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
