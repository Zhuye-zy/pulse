#include "math_formula.h"
#include "math_layout.h"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulse::ui {
namespace {
using Microsoft::WRL::ComPtr;

using Box = math::FormulaLayout;

void Put(Box& dst, const Box& src, float x) {
    dst = src;
    dst.width += x;
    for (auto& glyph : dst.glyphs)
        glyph.x += x;
    for (auto& rule : dst.rules) {
        rule.x1 += x;
        rule.x2 += x;
    }
}
struct DrawContext {
    ID2D1DeviceContext* dc;
    ID2D1Brush* brush;
};
ComPtr<ID2D1Brush> EffectBrush(IUnknown* effect, ID2D1Brush* fallback) {
    ComPtr<ID2D1Brush> brush;
    if (effect)
        effect->QueryInterface(IID_PPV_ARGS(&brush));
    if (!brush)
        brush = fallback;
    return brush;
}

class MathInline final : public IDWriteInlineObject {
public:
    MathInline(Box box, float scale) : box_(std::move(box)), scale_(scale) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteInlineObject)) {
            *result = static_cast<IDWriteInlineObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs)
            delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE Draw(void* context, IDWriteTextRenderer*, FLOAT x, FLOAT y, BOOL sideways, BOOL,
                                   IUnknown* effect) override {
        if (!context || sideways)
            return E_NOTIMPL;
        const auto& ctx = *static_cast<DrawContext*>(context);
        auto brush = EffectBrush(effect, ctx.brush);
        D2D1_MATRIX_3X2_F previous{};
        ctx.dc->GetTransform(&previous);
        const auto transform = D2D1::Matrix3x2F::Scale(scale_, scale_) *
                               D2D1::Matrix3x2F::Translation(x, y + box_.ascent * scale_) * previous;
        ctx.dc->SetTransform(transform);
        for (const auto& g : box_.glyphs)
            ctx.dc->DrawTextLayout(D2D1::Point2F(g.x, g.y), g.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        for (const auto& r : box_.rules)
            ctx.dc->DrawLine(D2D1::Point2F(r.x1, r.y1), D2D1::Point2F(r.x2, r.y2), brush.Get(), r.width);
        ctx.dc->SetTransform(previous);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) override {
        if (!metrics)
            return E_POINTER;
        *metrics = {box_.width * scale_, (box_.ascent + box_.descent) * scale_, box_.ascent * scale_, FALSE};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* metrics) override {
        if (!metrics)
            return E_POINTER;
        *metrics = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before,
                                                 DWRITE_BREAK_CONDITION* after) override {
        if (!before || !after)
            return E_POINTER;
        *before = DWRITE_BREAK_CONDITION_NEUTRAL;
        *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    Box box_;
    float scale_;
};

class TextRenderer final : public IDWriteTextRenderer {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextRenderer) ||
            iid == __uuidof(IDWritePixelSnapping)) {
            *result = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs)
            delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        if (!disabled)
            return E_POINTER;
        *disabled = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void* context, DWRITE_MATRIX* transform) override {
        if (!context || !transform)
            return E_POINTER;
        D2D1_MATRIX_3X2_F matrix{};
        static_cast<DrawContext*>(context)->dc->GetTransform(&matrix);
        *transform = {matrix._11, matrix._12, matrix._21, matrix._22, matrix._31, matrix._32};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void* context, FLOAT* value) override {
        if (!context || !value)
            return E_POINTER;
        float x = 96, y = 96;
        static_cast<DrawContext*>(context)->dc->GetDpi(&x, &y);
        *value = x / 96;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void* context, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
                                           const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown* effect) override {
        if (!context || !run)
            return E_POINTER;
        const auto& ctx = *static_cast<DrawContext*>(context);
        auto brush = EffectBrush(effect, ctx.brush);
        ctx.dc->DrawGlyphRun(D2D1::Point2F(x, y), run, brush.Get(), mode);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void* context, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* line,
                                            IUnknown* effect) override {
        if (!context || !line)
            return E_POINTER;
        const auto& ctx = *static_cast<DrawContext*>(context);
        auto brush = EffectBrush(effect, ctx.brush);
        ctx.dc->FillRectangle(D2D1::RectF(x, y + line->offset, x + line->width, y + line->offset + line->thickness),
                              brush.Get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void* context, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* line,
                                                IUnknown* effect) override {
        if (!context || !line)
            return E_POINTER;
        const auto& ctx = *static_cast<DrawContext*>(context);
        auto brush = EffectBrush(effect, ctx.brush);
        ctx.dc->FillRectangle(D2D1::RectF(x, y + line->offset, x + line->width, y + line->offset + line->thickness),
                              brush.Get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object,
                                               BOOL sideways, BOOL rtl, IUnknown* effect) override {
        return object ? object->Draw(context, this, x, y, sideways, rtl, effect) : E_POINTER;
    }

private:
    std::atomic<ULONG> refs_{1};
};
} // namespace

bool ApplyMathInline(IDWriteFactory2* factory, IDWriteTextLayout* layout, DWRITE_TEXT_RANGE range,
                     std::wstring_view source, float font_size, bool display, float max_width) {
    if (!factory || !layout || !range.length || source.empty() || source.size() > 4096 || !std::isfinite(font_size) ||
        font_size < 4 || font_size > 256 || !std::isfinite(max_width) || max_width <= 0)
        return false;
    try {
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 family_index = 0;
        BOOL exists = FALSE;
        if (FAILED(factory->GetSystemFontCollection(&fonts)) ||
            FAILED(fonts->FindFamilyName(L"Cambria Math", &family_index, &exists)) || !exists)
            return false;
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteFont> font;
        ComPtr<IDWriteFontFace> face;
        if (FAILED(fonts->GetFontFamily(family_index, &family)) ||
            FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, &font)) ||
            FAILED(font->CreateFontFace(&face)))
            return false;
        const auto parsed = ParseMathSyntax(source);
        if (!parsed)
            return false;
        const auto metrics = math::ReadFontMathMetrics(face.Get());
        Box box;
        if (!math::BuildMathLayout(factory, face.Get(), metrics, parsed.root, font_size, display, box))
            return false;
        const float scale = box.width > 0 ? std::min(1.0f, max_width / box.width) : 1.0f;
        if (scale < 0.55f)
            return false;
        // Include half-stroke and antialiasing margins in the advertised bounds.
        const float padding = std::max(1.0f, font_size * 0.04f);
        Box padded;
        Put(padded, box, padding);
        padded.width += padding;
        padded.ascent += padding;
        padded.descent += padding;
        const float final_scale = std::min(scale, max_width / padded.width);
        ComPtr<IDWriteInlineObject> object;
        object.Attach(new MathInline(std::move(padded), final_scale));
        return SUCCEEDED(layout->SetInlineObject(object.Get(), range));
    } catch (...) {
        return false;
    }
}

void DrawMathTextLayout(ID2D1DeviceContext* dc, IDWriteTextLayout* layout, D2D1_POINT_2F origin,
                        ID2D1SolidColorBrush* brush) {
    if (!dc || !layout || !brush)
        return;
    DrawContext context{dc, brush};
    ComPtr<IDWriteTextRenderer> renderer;
    renderer.Attach(new TextRenderer());
    layout->Draw(&context, renderer.Get(), origin.x, origin.y);
}

} // namespace pulse::ui
