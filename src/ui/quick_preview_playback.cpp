#include "quick_preview_window.h"
#include "playback_timeline.h"
#include "typography.h"
#include "../common/localization.h"
#include <d2d1helper.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kVideoTimer = 8;
constexpr UINT_PTR kScrubTimer = 9;
constexpr ULONGLONG kSeekIntervalMs = 80;
constexpr float kPlaybackHeight = 76.0f;
// Rollback switch: false restores the text-button bar and full-bleed video.
constexpr bool kVideoChrome = true;
constexpr float kChromeBarHeight = 56.0f;
// Rollback switch: false leaves an undecodable video track as a black frame.
constexpr bool kCodecHint = true;
constexpr float kRates[] = {1.0f, 1.25f, 1.5f, 2.0f, 0.5f};
const wchar_t* Label(const wchar_t* chinese, const wchar_t* english) {
    return l10n::Pick(chinese, english);
}
bool Contains(const D2D1_RECT_F& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}
std::wstring Clock(int64_t ticks) {
    const int64_t ms = (std::max)(int64_t{0}, ticks / 10000);
    wchar_t value[64];
    swprintf_s(value, L"%02lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return value;
}
// m:ss, or h:mm:ss when the media is an hour or longer.
std::wstring ShortClock(int64_t ticks, bool hours) {
    const int64_t total = (std::max)(int64_t{0}, ticks / 10000000);
    wchar_t value[32];
    if (hours) swprintf_s(value, L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    else swprintf_s(value, L"%lld:%02lld", total / 60, total % 60);
    return value;
}
std::wstring RateLabel(float rate) {
    wchar_t value[16];
    swprintf_s(value, L"%g\x00D7", static_cast<double>(rate));
    return value;
}
std::wstring ResolutionLabel(uint32_t width, uint32_t height) {
    const uint32_t lines = (std::min)(width, height);
    if (lines >= 2160) return L"4K";
    if (lines >= 1440) return L"1440p";
    if (lines >= 1080) return L"1080p";
    if (lines >= 720) return L"720p";
    if (!width || !height) return {};
    return std::to_wstring(width) + L"\x00D7" + std::to_wstring(height);
}
// Icons are outlines in a 16-unit box centred in `box`, so they do not depend
// on the installed icon font.
struct IconPen {
    ID2D1DeviceContext* dc;
    ID2D1Factory* factory;
    D2D1_RECT_F box;
    float unit;
    D2D1_POINT_2F P(float x, float y) const {
        const float cx = (box.left + box.right) * 0.5f, cy = (box.top + box.bottom) * 0.5f;
        return D2D1::Point2F(cx + (x - 8.0f) * unit, cy + (y - 8.0f) * unit);
    }
    void Poly(std::initializer_list<D2D1_POINT_2F> points, ID2D1Brush* brush) const {
        if (!factory || points.size() < 3) return;
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(factory->CreatePathGeometry(&path)) || !path.get()) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(&sink)) || !sink.get()) return;
        auto it = points.begin();
        sink->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
        for (++it; it != points.end(); ++it) sink->AddLine(*it);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (SUCCEEDED(sink->Close())) dc->FillGeometry(path.get(), brush);
    }
    void Bar(float l, float t, float r, float b, ID2D1Brush* brush) const {
        const auto a = P(l, t), c = P(r, b);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a.x, a.y, c.x, c.y), unit, unit), brush);
    }
    void Arc(float x0, float y0, float x1, float y1, float radius, ID2D1Brush* brush) const {
        if (!factory) return;
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(factory->CreatePathGeometry(&path)) || !path.get()) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(&sink)) || !sink.get()) return;
        sink->BeginFigure(P(x0, y0), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(P(x1, y1), D2D1::SizeF(radius * unit, radius * unit), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (SUCCEEDED(sink->Close())) dc->DrawGeometry(path.get(), brush, 1.35f * unit);
    }
    void Line(float x0, float y0, float x1, float y1, ID2D1Brush* brush) const {
        dc->DrawLine(P(x0, y0), P(x1, y1), brush, 1.35f * unit);
    }
};
}

bool QuickPreviewWindow::HasPlayback() const noexcept {
    return frame_count_ > 1 || video_.active();
}
float QuickPreviewWindow::PlaybackHeight() const noexcept {
    return HasPlayback() ? (kVideoChrome ? kChromeBarHeight : kPlaybackHeight) * scale_ : 0.0f;
}
D2D1_RECT_F QuickPreviewWindow::PlaybackRect() const {
    const float height = static_cast<float>(compositor_.Height());
    return D2D1::RectF(0, height - PlaybackHeight(),
        static_cast<float>(compositor_.Width()), height);
}
QuickPreviewWindow::PlaybackGeometry QuickPreviewWindow::PlaybackLayout() const {
    PlaybackGeometry g;
    const auto bar = PlaybackRect();
    const float s = scale_;
    const float cy = (bar.top + bar.bottom) * 0.5f;
    const bool video = video_.active();
    g.button_count = video ? 2 : 3;
    g.play_index = video ? 0 : 1;
    float x = bar.left + 14.0f * s;
    for (int i = 0; i < g.button_count; ++i) {
        const float size = (i == g.play_index ? 40.0f : 36.0f) * s;
        g.buttons[i] = D2D1::RectF(x, cy - size * 0.5f, x + size, cy + size * 0.5f);
        x += size + 6.0f * s;
    }
    float right = bar.right - 14.0f * s;
    const auto slot = [&](float width) {
        const D2D1_RECT_F rect = D2D1::RectF(right - width, cy - 18.0f * s, right, cy + 18.0f * s);
        right -= width + 4.0f * s;
        return rect;
    };
    g.chip = slot(70.0f * s);
    if (video) {
        g.speed = slot(42.0f * s);
        g.volume = slot(36.0f * s);
    }
    const bool hours = video && video_.Snapshot().duration >= int64_t{36000000000};
    const float time_width = (hours ? 56.0f : 44.0f) * s;
    g.time_right = D2D1::RectF(right - time_width, cy - 12.0f * s, right, cy + 12.0f * s);
    right -= time_width + 4.0f * s;
    x += 4.0f * s;
    g.time_left = D2D1::RectF(x, cy - 12.0f * s, x + time_width, cy + 12.0f * s);
    x += time_width + 10.0f * s;
    g.track = D2D1::RectF(x, cy - 12.0f * s, (std::max)(x + s, right - 6.0f * s), cy + 12.0f * s);
    return g;
}
D2D1_RECT_F QuickPreviewWindow::VideoFrameRect(const VideoPreview::State& state) const {
    const auto content = ContentRect();
    if (!kVideoChrome) return content;
    const float s = scale_;
    // Side/top margins frame the picture; the bottom one leaves room for the
    // seek bar's time bubble, which cannot draw over the video window.
    const D2D1_RECT_F box = D2D1::RectF(content.left + 16.0f * s, content.top + 12.0f * s,
                                        content.right - 16.0f * s, content.bottom - 28.0f * s);
    const float bw = box.right - box.left, bh = box.bottom - box.top;
    if (bw < 48.0f * s || bh < 48.0f * s) return content;
    if (!state.width || !state.height) return box;
    const float k = (std::min)(bw / static_cast<float>(state.width), bh / static_cast<float>(state.height));
    const float w = static_cast<float>(state.width) * k, h = static_cast<float>(state.height) * k;
    const float cx = (box.left + box.right) * 0.5f, cy = (box.top + box.bottom) * 0.5f;
    return D2D1::RectF(std::round(cx - w * 0.5f), std::round(cy - h * 0.5f),
                       std::round(cx + w * 0.5f), std::round(cy + h * 0.5f));
}
void QuickPreviewWindow::LayoutVideo(ID2D1DeviceContext* dc, const VideoPreview::State& state,
                                     bool show) {
    const D2D1_RECT_F frame = VideoFrameRect(state);
    const RECT bounds{std::lround(frame.left), std::lround(frame.top), std::lround(frame.right),
                      std::lround(frame.bottom)};
    const int radius = kVideoChrome ? static_cast<int>(std::lround(10.0f * scale_)) : 0;
    if (kVideoChrome && show && dc) {
        ComPtr<ID2D1SolidColorBrush> shadow;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, dark_ ? 0.07f : 0.035f), &shadow);
        if (shadow.get()) {
            for (int i = 6; i >= 1; --i) {
                const float grow = static_cast<float>(i) * 2.0f * scale_;
                const D2D1_RECT_F r = D2D1::RectF(frame.left - grow, frame.top - grow + 3.0f * scale_,
                                                  frame.right + grow, frame.bottom + grow + 3.0f * scale_);
                const float rr = static_cast<float>(radius) + grow;
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, rr, rr), shadow.get());
            }
        }
    }
    video_.Layout(bounds, show, radius);
}
bool QuickPreviewWindow::PlaybackHover(POINT point) {
    float hover_x = -1.0f;
    int button = -1;
    if (kVideoChrome && HasPlayback() && Contains(PlaybackRect(), point)) {
        const auto g = PlaybackLayout();
        for (int i = 0; i < g.button_count; ++i)
            if (Contains(g.buttons[i], point)) button = i;
        if (Contains(g.volume, point)) button = 3;
        if (Contains(g.speed, point)) button = 4;
        if (Contains(g.track, point)) hover_x = static_cast<float>(point.x);
    }
    const bool changed = button != playback_hover_button_ ||
        (hover_x < 0) != (playback_hover_x_ < 0) || std::abs(hover_x - playback_hover_x_) >= 1.0f;
    playback_hover_x_ = hover_x;
    playback_hover_button_ = button;
    return changed;
}
bool QuickPreviewWindow::PlaybackWheel(POINT point, float steps) {
    if (!kVideoChrome || !video_.active() || !Contains(PlaybackLayout().volume, point)) return false;
    playback_volume_ = std::clamp(playback_volume_ + steps * 0.05f, 0.0f, 1.0f);
    if (steps > 0 && playback_muted_) {
        playback_muted_ = false;
        video_.SetMuted(false);
    }
    video_.SetVolume(playback_volume_);
    playback_note_ = Label(L"\x97F3\x91CF ", L"Volume ") +
        std::to_wstring(std::lround(playback_volume_ * 100.0f)) + L"%";
    playback_note_until_ = GetTickCount64() + 1200;
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}
D2D1_RECT_F QuickPreviewWindow::PlaybackButtonRect(int button) const {
    if (kVideoChrome) {
        const auto g = PlaybackLayout();
        return g.buttons[std::clamp(button, 0, g.button_count - 1)];
    }
    const auto bar = PlaybackRect();
    const float x = (12.0f + 70.0f * static_cast<float>(button)) * scale_;
    return D2D1::RectF(x, bar.top + 6.0f * scale_, x + 66.0f * scale_,
        bar.top + 38.0f * scale_);
}
D2D1_RECT_F QuickPreviewWindow::PlaybackTrackRect() const {
    if (kVideoChrome) return PlaybackLayout().track;
    const auto bar = PlaybackRect();
    const float left = (video_.active() ? 164.0f : 234.0f) * scale_;
    return D2D1::RectF(left, bar.top + 6.0f * scale_,
        (std::max)(left + scale_, bar.right - 18.0f * scale_), bar.top + 38.0f * scale_);
}
void QuickPreviewWindow::BeginVideo() {
    if (safe_mode_ || OfflinePlaceholder() || (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        !VideoPreview::Supports(item_.path)) return;
    handler_.Reset();
    video_.Open(hwnd_, item_.path);
    playback_rate_ = 1.0f;
    playback_note_until_ = 0;
    playback_hover_x_ = -1.0f;
    playback_hover_button_ = -1;
    video_.SetVolume(playback_volume_);
    video_.SetMuted(playback_muted_);
    if (VideoPreview::IsAudio(item_.path)) waveform_.Start(item_.path);
    SetTimer(hwnd_, kVideoTimer, 50, nullptr);
}
void QuickPreviewWindow::CancelPlaybackScrub() {
    const bool was_dragging = playback_drag_;
    playback_drag_ = false;
    playback_resume_ = false;
    playback_scrub_pending_ = false;
    playback_seek_dirty_ = false;
    playback_scrub_fraction_ = 0;
    playback_last_seek_ms_ = 0;
    if (hwnd_) {
        KillTimer(hwnd_, kScrubTimer);
        if (was_dragging && GetCapture() == hwnd_) ReleaseCapture();
    }
}
void QuickPreviewWindow::ResetPlayback() {
    CancelPlaybackScrub();
    if (hwnd_) {
        KillTimer(hwnd_, kVideoTimer);
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    video_.Reset();
}
void QuickPreviewWindow::TogglePlayback() {
    if (playback_drag_) {
        EndPlaybackDrag(false);
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    if (video_.active()) {
        const auto state = video_.Snapshot();
        if (state.ready && SUCCEEDED(state.error)) video_.Play(!state.playing);
    } else if (frame_count_ > 1) {
        animation_active_ = !animation_active_;
        KillTimer(hwnd_, kAnimationTimer);
        if (animation_active_) {
            completed_loops_ = 0;
            if (!playback_scrub_pending_ && !waiting_for_frame_ && frame_index_ + 1 >= frame_count_) {
                requested_frame_ = 0;
                waiting_for_frame_ = true;
            }
            if (!playback_scrub_pending_ && !waiting_for_frame_)
                SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        } else if (waiting_for_frame_ && !playback_scrub_pending_) {
            // Pause means retain the frame actually on screen, not an outstanding auto tick.
            waiting_for_frame_ = false;
            requested_frame_ = frame_index_;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::StepPlayback(int direction) {
    const uint32_t base_frame = playback_scrub_pending_
        ? playback::FrameAt(playback_scrub_fraction_, frame_count_)
        : waiting_for_frame_ ? requested_frame_ : frame_index_;
    CancelPlaybackScrub();
    if (video_.active()) {
        if (direction > 0) video_.Step(); // MFPlay's exact frame step is forward-only.
    } else if (frame_count_ > 1) {
        animation_active_ = false;
        KillTimer(hwnd_, kAnimationTimer);
        requested_frame_ = playback::StepFrame(
            base_frame, direction, frame_count_);
        waiting_for_frame_ = requested_frame_ != frame_index_;
        completed_loops_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::SeekPlayback(float x) {
    const auto track = PlaybackTrackRect();
    // Pointer position is continuous, independent of both frame quantization and
    // asynchronous decoding. Moving the thumb never waits for a thumbnail.
    playback_scrub_fraction_ = playback::Fraction(x, track.left, track.right);
    playback_scrub_pending_ = true;
    playback_seek_dirty_ = true;
    SubmitPlaybackSeek(!playback_drag_);
    SetTimer(hwnd_, kScrubTimer, 16, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::SubmitPlaybackSeek(bool immediate) {
    if (!playback_seek_dirty_) return;
    const auto now = GetTickCount64();
    if (!immediate && playback_last_seek_ms_ && now - playback_last_seek_ms_ < kSeekIntervalMs)
        return;
    if (video_.active()) {
        const auto state = video_.Snapshot();
        if (!state.ready || !state.can_seek || state.duration <= 0 || FAILED(state.error)) {
            playback_seek_dirty_ = false;
            return;
        }
        if (!immediate && state.busy) return;
        video_.Seek(playback_scrub_fraction_);
    } else if (frame_count_ > 1) {
        // Do not replace an in-flight frame with every mouse move. Keep only the
        // newest desired fraction, then submit it when this decode has finished.
        if (waiting_for_frame_) return;
        requested_frame_ = playback::FrameAt(playback_scrub_fraction_, frame_count_);
        waiting_for_frame_ = requested_frame_ != frame_index_;
        completed_loops_ = 0;
    }
    playback_last_seek_ms_ = now;
    playback_seek_dirty_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::TickPlaybackSeek() {
    SubmitPlaybackSeek(!playback_drag_);
    const bool settled = video_.active() ? !playback_seek_dirty_
        : !playback_seek_dirty_ && !waiting_for_frame_;
    if (!playback_drag_ && settled) {
        playback_scrub_pending_ = false;
        KillTimer(hwnd_, kScrubTimer);
        if (!video_.active() && animation_active_ && frame_count_ > 1)
            SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
double QuickPreviewWindow::PlaybackFraction(const VideoPreview::State& state) const {
    if (playback_drag_ || playback_scrub_pending_) return playback_scrub_fraction_;
    if (video_.active() && state.duration > 0)
        return std::clamp(static_cast<double>(state.position) / state.duration, 0.0, 1.0);
    return frame_count_ > 1 ? static_cast<double>(frame_index_) / (frame_count_ - 1) : 0;
}
bool QuickPreviewWindow::PlaybackMouseDown(POINT point) {
    if (!HasPlayback() || !Contains(PlaybackRect(), point)) return false;
    SetFocus(hwnd_);
    const bool video = video_.active();
    const auto state = video_.Snapshot();
    if (kVideoChrome && video) {
        const auto g = PlaybackLayout();
        if (Contains(g.volume, point)) {
            playback_muted_ = !playback_muted_;
            video_.SetMuted(playback_muted_);
            playback_note_ = playback_muted_ ? std::wstring(Label(L"\x5DF2\x9759\x97F3", L"Muted"))
                : Label(L"\x97F3\x91CF ", L"Volume ") +
                  std::to_wstring(std::lround(playback_volume_ * 100.0f)) + L"%";
            playback_note_until_ = GetTickCount64() + 1200;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (Contains(g.speed, point)) {
            size_t next = 0;
            for (size_t i = 0; i < std::size(kRates); ++i)
                if (std::abs(kRates[i] - playback_rate_) < 0.01f) next = (i + 1) % std::size(kRates);
            playback_rate_ = kRates[next];
            video_.SetRate(playback_rate_);
            playback_note_ = Label(L"\x500D\x901F ", L"Speed ") + RateLabel(playback_rate_);
            playback_note_until_ = GetTickCount64() + 1200;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
    }
    if (video && (!state.ready || FAILED(state.error))) return true;
    const int play = video ? 0 : 1;
    const int next = video ? 1 : 2;
    if (Contains(PlaybackButtonRect(play), point)) TogglePlayback();
    else if (Contains(PlaybackButtonRect(next), point)) StepPlayback(1);
    else if (!video && Contains(PlaybackButtonRect(0), point)) StepPlayback(-1);
    else if (Contains(PlaybackTrackRect(), point) && (!video || state.can_seek)) {
        const bool was_playing = video ? state.playing : animation_active_;
        CancelPlaybackScrub();
        playback_resume_ = was_playing;
        playback_drag_ = true;
        if (video) video_.Play(false);
        else {
            animation_active_ = false;
            KillTimer(hwnd_, kAnimationTimer);
        }
        SetCapture(hwnd_);
        SeekPlayback(static_cast<float>(point.x));
    }
    return true;
}
void QuickPreviewWindow::EndPlaybackDrag(bool resume) {
    if (!playback_drag_) return;
    playback_drag_ = false;
    const bool play = resume && playback_resume_;
    playback_resume_ = false;
    SubmitPlaybackSeek(true); // release bypasses throttling, but not a GIF decode in flight
    if (video_.active()) video_.Play(play);
    else animation_active_ = play;
    TickPlaybackSeek();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::DrawPlayback(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush) {
    if (!HasPlayback() || !dc || !brush) return;
    if (kVideoChrome) {
        DrawPlaybackChrome(dc, brush);
        return;
    }
    const auto bar = PlaybackRect();
    const bool video = video_.active();
    const auto state = video_.Snapshot();
    const bool enabled = !video || (state.ready && SUCCEEDED(state.error));
    const bool playing = video ? state.playing : animation_active_;
    const auto original = brush->GetColor();
    auto color = original;
    color.a = 0.12f;
    brush->SetColor(color);
    dc->DrawLine(D2D1::Point2F(bar.left, bar.top), D2D1::Point2F(bar.right, bar.top), brush);
    auto* format = compositor_.SmallFormat();
    if (!format) { brush->SetColor(original); return; }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int i = 0; i < (video ? 2 : 3); ++i) {
        const bool is_play = i == (video ? 0 : 1);
        const wchar_t* text = is_play ? (playing ? Label(L"暂停", L"Pause") : Label(L"播放", L"Play"))
            : i == 0 ? Label(L"上一帧", L"Prev frame") : Label(L"下一帧", L"Next frame");
        const bool available = enabled && (is_play || !video || !state.ended);
        const auto rect = PlaybackButtonRect(i);
        color = original; color.a = available ? 0.09f : 0.03f;
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4 * scale_, 4 * scale_), brush);
        color = original; color.a = available ? 1.0f : 0.4f;
        brush->SetColor(color);
        dc->DrawTextW(text, static_cast<UINT32>(wcslen(text)), format, rect, brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    const auto track = PlaybackTrackRect();
    const float y = (track.top + track.bottom) * 0.5f;
    const double fraction = PlaybackFraction(state);
    color = original; color.a = enabled && (!video || state.can_seek) ? 0.3f : 0.1f;
    brush->SetColor(color);
    dc->DrawLine(D2D1::Point2F(track.left, y), D2D1::Point2F(track.right, y), brush, 3 * scale_);
    const float thumb = track.left + static_cast<float>(fraction) * (track.right - track.left);
    brush->SetColor(original);
    dc->DrawLine(D2D1::Point2F(track.left, y), D2D1::Point2F(thumb, y), brush, 3 * scale_);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb, y), 5 * scale_, 5 * scale_), brush);
    const std::wstring info = PlaybackInfo(state);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    dc->DrawTextW(info.data(), static_cast<UINT32>(info.size()), format,
        D2D1::RectF(12 * scale_, bar.top + 40 * scale_, bar.right - 12 * scale_, bar.bottom - 8 * scale_),
        brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    brush->SetColor(original);
}
std::wstring QuickPreviewWindow::PlaybackInfo(const VideoPreview::State& state) const {
    const bool video = video_.active();
    const bool enabled = !video || (state.ready && SUCCEEDED(state.error));
    const bool playing = video ? state.playing : animation_active_;
    std::wstring info;
    if (video) {
        info = Clock(state.position) + L" / " + Clock(state.duration);
        if (state.width && state.height)
            info += L" · " + std::to_wstring(state.width) + L"×" + std::to_wstring(state.height);
        if (state.stepped_frames)
            info += Label(L" · 逐帧 +", L" · Stepped +") + std::to_wstring(state.stepped_frames);
        if (FAILED(state.error)) info += Label(L" · 无法播放，请用默认应用打开", L" · Cannot play; open externally");
        else if (FAILED(state.control_error)) info += Label(L" · 此媒体不支持该操作", L" · Control unavailable for this media");
        else if (!state.ready) info += Label(L" · 正在加载", L" · Loading");
        else if (playback_drag_) info += Label(L" · 拖动定位", L" · Scrubbing");
        else if (state.busy) info += Label(L" · 正在定位", L" · Seeking/stepping");
        else if (state.ended) info += Label(L" · 已结束", L" · Ended");
    } else {
        info = Label(L"帧 ", L"Frame ") + std::to_wstring(frame_index_ + 1) + L" / " +
            std::to_wstring(frame_count_) + L" · " + std::to_wstring(frame_delay_ms_) + L" ms";
        // Waiting is normal between every pair of GIF frames. Never insert a
        // transient "Seeking" label here: it shifts/flashes the entire suffix.
        if (playback_drag_) info += Label(L" · 拖动定位", L" · Scrubbing");
    }
    if (enabled) info += playing ? Label(L" · 播放中", L" · Playing") : Label(L" · 已暂停", L" · Paused");
    return info;
}
void QuickPreviewWindow::DrawPlaybackChrome(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush) {
    auto* format = compositor_.SmallFormat();
    if (!format) return;
    const auto g = PlaybackLayout();
    const auto bar = PlaybackRect();
    const float s = scale_;
    const bool video = video_.active();
    const bool audio = video && VideoPreview::IsAudio(item_.path);
    const auto state = video_.Snapshot();
    const bool enabled = !video || (state.ready && SUCCEEDED(state.error));
    const bool playing = video ? state.playing : animation_active_;
    const bool seekable = enabled && (!video || state.can_seek);
    const auto original = brush->GetColor();
    const auto tint = [&](float alpha) {
        auto color = original;
        color.a = original.a * alpha;
        brush->SetColor(color);
        return brush;
    };
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ComPtr<ID2D1SolidColorBrush> ink, white, shade;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x111111) : D2D1::ColorF(0xFFFFFF), &ink);
    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), &white);
    dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.35f), &shade);
    const auto text_alignment = format->GetTextAlignment();
    const auto paragraph_alignment = format->GetParagraphAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const auto text = [&](const std::wstring& value, const D2D1_RECT_F& rect, ID2D1Brush* b) {
        dc->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format, rect, b,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const auto hover_fill = [&](const D2D1_RECT_F& rect, int id) {
        if (playback_hover_button_ != id) return;
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f * s, 6.0f * s), tint(dark_ ? 0.08f : 0.06f));
    };

    // Transport buttons: a filled round play/pause plus frame steps.
    for (int i = 0; i < g.button_count; ++i) {
        const auto rect = g.buttons[i];
        if (i == g.play_index) {
            const float r = (rect.right - rect.left) * 0.5f;
            const auto centre = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
            dc->FillEllipse(D2D1::Ellipse(centre, r, r),
                            tint(enabled ? (playback_hover_button_ == i ? 0.86f : 1.0f) : 0.35f));
            if (!ink.get()) continue;
            const IconPen pen{dc, factory.get(), rect, 14.0f * s / 16.0f};
            if (playing) {
                pen.Bar(3.4f, 2.2f, 6.9f, 13.8f, ink.get());
                pen.Bar(9.1f, 2.2f, 12.6f, 13.8f, ink.get());
            } else {
                pen.Poly({pen.P(4.6f, 2.0f), pen.P(4.6f, 14.0f), pen.P(14.0f, 8.0f)}, ink.get());
            }
            continue;
        }
        hover_fill(rect, i);
        const bool forward = i > g.play_index;
        const bool available = enabled && (!video || (forward && !state.ended && !CodecCardVisible(state)));
        const IconPen pen{dc, factory.get(), rect, s};
        ID2D1Brush* b = tint(available ? 0.95f : 0.35f);
        if (forward) {
            pen.Poly({pen.P(3.0f, 2.5f), pen.P(3.0f, 13.5f), pen.P(10.5f, 8.0f)}, b);
            pen.Bar(11.5f, 2.5f, 13.5f, 13.5f, b);
        } else {
            pen.Poly({pen.P(13.0f, 2.5f), pen.P(13.0f, 13.5f), pen.P(5.5f, 8.0f)}, b);
            pen.Bar(2.5f, 2.5f, 4.5f, 13.5f, b);
        }
    }

    // Times either side of the track.
    const double fraction = PlaybackFraction(state);
    const bool hours = video && state.duration >= int64_t{36000000000};
    std::wstring left, right;
    if (video) {
        const int64_t shown = (playback_drag_ || playback_scrub_pending_)
            ? static_cast<int64_t>(fraction * static_cast<double>(state.duration)) : state.position;
        left = ShortClock(shown, hours);
        right = ShortClock(state.duration, hours);
    } else {
        const uint32_t frame = (playback_drag_ || playback_scrub_pending_)
            ? playback::FrameAt(playback_scrub_fraction_, frame_count_) : frame_index_;
        left = std::to_wstring(frame + 1);
        right = std::to_wstring(frame_count_);
    }
    text(left, g.time_left, tint(enabled ? 0.85f : 0.4f));
    text(right, g.time_right, tint(enabled ? 0.85f : 0.4f));

    // Seek track: thicker under the pointer or while dragging.
    const auto track = g.track;
    const float y = (track.top + track.bottom) * 0.5f;
    const bool active = seekable && (playback_hover_x_ >= 0.0f || playback_drag_);
    const float thick = (active ? 6.0f : 4.0f) * s;
    const auto rail = D2D1::RectF(track.left, y - thick * 0.5f, track.right, y + thick * 0.5f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rail, thick * 0.5f, thick * 0.5f),
                             tint(seekable ? (dark_ ? 0.18f : 0.14f) : 0.07f));
    const float thumb = track.left + static_cast<float>(fraction) * (track.right - track.left);
    if (thumb > track.left + 0.5f) {
        const auto filled = D2D1::RectF(track.left, rail.top, thumb, rail.bottom);
        dc->FillRoundedRectangle(D2D1::RoundedRect(filled, thick * 0.5f, thick * 0.5f),
                                 tint(enabled ? 1.0f : 0.35f));
    }
    if (active && !playback_drag_ && playback_hover_x_ >= 0.0f) {
        const float gx = std::clamp(playback_hover_x_, track.left, track.right);
        dc->DrawLine(D2D1::Point2F(gx, y - 6.0f * s), D2D1::Point2F(gx, y + 6.0f * s),
                     tint(0.6f), 2.0f * s);
    }
    if (enabled && white.get() && shade.get()) {
        const float r = (playback_drag_ ? 8.0f : 7.0f) * s;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb, y + 1.0f * s), r + 1.0f * s, r + 1.0f * s), shade.get());
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb, y), r, r), white.get());
    }

    // Volume and speed (video / audio only).
    if (video) {
        hover_fill(g.volume, 3);
        const IconPen pen{dc, factory.get(), g.volume, s};
        ID2D1Brush* b = tint(0.95f);
        pen.Poly({pen.P(1.5f, 6.0f), pen.P(4.2f, 6.0f), pen.P(7.8f, 2.8f), pen.P(7.8f, 13.2f),
                  pen.P(4.2f, 10.0f), pen.P(1.5f, 10.0f)}, b);
        if (playback_muted_ || playback_volume_ <= 0.0f) {
            pen.Line(10.2f, 5.8f, 14.4f, 10.2f, b);
            pen.Line(14.4f, 5.8f, 10.2f, 10.2f, b);
        } else {
            pen.Arc(10.3f, 5.3f, 10.3f, 10.7f, 3.9f, b);
            if (playback_volume_ > 0.5f) pen.Arc(12.1f, 3.4f, 12.1f, 12.6f, 6.4f, b);
        }
        hover_fill(g.speed, 4);
        text(RateLabel(playback_rate_), g.speed, tint(std::abs(playback_rate_ - 1.0f) < 0.01f ? 0.9f : 1.0f));
    }

    // Chip: a transient note, otherwise state / resolution / exact time.
    std::wstring chip;
    if (GetTickCount64() < playback_note_until_) chip = playback_note_;
    else if (video && CodecCardVisible(state)) chip = state.codec;
    else if (video && FAILED(state.error)) chip = Label(L"\x65E0\x6CD5\x64AD\x653E", L"Can't play");
    else if (video && FAILED(state.control_error)) chip = Label(L"\x64CD\x4F5C\x4E0D\x53EF\x7528", L"Unavailable");
    else if (video && !state.ready) chip = Label(L"\x52A0\x8F7D\x4E2D", L"Loading");
    else if (video && !audio && (playback_drag_ || (!state.playing && state.stepped_frames)))
        chip = Clock(playback_drag_ ? static_cast<int64_t>(fraction * static_cast<double>(state.duration))
                                    : state.position);
    else if (video && !audio) chip = ResolutionLabel(state.width, state.height);
    else if (!video) chip = std::to_wstring(frame_delay_ms_) + L" ms";
    if (!chip.empty()) {
        const float h = 22.0f * s;
        const float cy = (g.chip.top + g.chip.bottom) * 0.5f;
        const auto pill = D2D1::RectF(g.chip.left, cy - h * 0.5f, g.chip.right, cy + h * 0.5f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(pill, h * 0.5f, h * 0.5f), tint(dark_ ? 0.08f : 0.06f));
        text(chip, pill, tint(0.9f));
    }

    // Time bubble above the pointer on the track.
    if (active && !playback_drag_ && playback_hover_x_ >= 0.0f && compositor_.DwriteFactory()) {
        const double at = playback::Fraction(playback_hover_x_, track.left, track.right);
        const std::wstring tip = video
            ? ShortClock(static_cast<int64_t>(at * static_cast<double>(state.duration)), hours)
            : std::to_wstring(playback::FrameAt(at, frame_count_) + 1);
        const float tw = typography::MeasureAdvance(compositor_.DwriteFactory(), format, tip) + 16.0f * s;
        const float th = 22.0f * s;
        const float cx = std::clamp(playback_hover_x_, bar.left + tw * 0.5f + 4.0f * s,
                                    bar.right - tw * 0.5f - 4.0f * s);
        const auto bubble = D2D1::RectF(cx - tw * 0.5f, bar.top - th - 4.0f * s,
                                        cx + tw * 0.5f, bar.top - 4.0f * s);
        ComPtr<ID2D1SolidColorBrush> fill;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x2B2B2B, 0.96f), &fill);
        if (fill.get() && white.get()) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(bubble, 5.0f * s, 5.0f * s), fill.get());
            text(tip, bubble, white.get());
        }
    }
    format->SetTextAlignment(text_alignment);
    format->SetParagraphAlignment(paragraph_alignment);
    brush->SetColor(original);
}
bool QuickPreviewWindow::CodecCardVisible(const VideoPreview::State& state) const {
    return kCodecHint && state.missing_decoder && !VideoPreview::IsAudio(item_.path);
}
bool QuickPreviewWindow::OverCodecButton(POINT point) const {
    return Contains(codec_store_rect_, point) || Contains(codec_open_rect_, point);
}
bool QuickPreviewWindow::CodecCardClick(POINT point) {
    if (Contains(codec_store_rect_, point) && !codec_store_id_.empty()) {
        const std::wstring url = L"ms-windows-store://pdp/?ProductId=" + codec_store_id_;
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (Contains(codec_open_rect_, point)) {
        PostAction(QuickPreviewAction::Open);
        return true;
    }
    return false;
}
void QuickPreviewWindow::DrawCodecCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                       const VideoPreview::State& state,
                                       ID2D1SolidColorBrush* brush) {
    const bool zh = l10n::IsChinese();
    CodecCardText t;
    t.title = l10n::Pick(L"这台电脑无法显示此视频的画面", L"Can't show this video on this PC");
    t.lead = l10n::Pick(L"视频使用 ", L"It's encoded as ");
    t.name = state.codec_name;
    t.tail = l10n::Pick(L" 编码，系统中没有对应的解码器。\n声音仍可正常播放。", L" and no decoder for it is installed.\nThe sound still plays.");
    t.store_id = state.store_id;
    if (state.store_id)
        t.get = zh ? l10n::Cn(L"获取 ") + state.codec + l10n::Cn(L" 视频扩展") : L"Get " + state.codec + L" Video Extension";
    t.hint = l10n::Pick(L"安装后重新打开预览即可看到画面", L"Reopen the preview after installing");
    DrawCodecCardText(dc, content, t, brush);
}

// HEIF / HEIC / AVIF without the Store extension that decodes them (mockup ⑥).
void QuickPreviewWindow::DrawImageCodecCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                            const std::wstring& codec, ID2D1SolidColorBrush* brush) {
    CodecCardText t;
    t.picture = true;
    t.title = l10n::Pick(L"这台电脑无法显示此图片", L"Can't show this picture on this PC");
    t.lead = l10n::Pick(L"图片使用 ", L"It uses ");
    t.tail = l10n::Pick(L" 格式，系统中没有对应的解码器。", L" and no decoder for it is installed.");
    if (codec == L"av1") {
        t.name = L"AVIF";
        t.store_id = L"9MVZQVXJBQ9V";
        t.get = l10n::Pick(L"获取 AV1 视频扩展", L"Get AV1 Video Extension");
        t.hint = l10n::Pick(L"安装后重新打开预览即可看到图片", L"Reopen the preview after installing");
    } else if (codec == L"hevc") {
        t.name = L"HEIF / HEIC";
        t.tail = l10n::Pick(L" 格式，照片使用 HEVC 编码，还需要 HEVC 视频扩展。", L"; its photo is HEVC-encoded and needs the HEVC Video Extension.");
        t.store_id = L"9NMZLZ57R3T7";
        t.get = l10n::Pick(L"获取 HEVC 视频扩展", L"Get HEVC Video Extension");
        t.hint = l10n::Pick(L"安装后重新打开预览即可看到图片", L"Reopen the preview after installing");
    } else {
        t.name = L"HEIF / HEIC";
        t.store_id = L"9PMMSR1CGPWG";
        t.get = l10n::Pick(L"获取 HEIF 图像扩展", L"Get HEIF Image Extensions");
        t.hint = l10n::Pick(L"iPhone 照片常用这个格式；部分 HEIC 还需要 HEVC 视频扩展", L"Common for iPhone photos; some HEIC also need the HEVC Video Extension");
    }
    DrawCodecCardText(dc, content, t, brush);
}

void QuickPreviewWindow::DrawCodecCardText(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                           const CodecCardText& text, ID2D1SolidColorBrush* brush) {
    auto* factory = compositor_.DwriteFactory();
    auto* title_format = compositor_.HeaderFormat();
    auto* caption = compositor_.SmallFormat();
    if (!dc || !brush || !factory || !title_format || !caption) return;
    const float s = scale_;
    const auto original = brush->GetColor();
    const auto tint = [&](float alpha) {
        auto color = original;
        color.a = original.a * alpha;
        brush->SetColor(color);
        return brush;
    };
    const std::wstring& title = text.title;
    const std::wstring& lead = text.lead;
    const std::wstring body = text.lead + text.name + text.tail;
    const std::wstring& get = text.get;
    const std::wstring open = l10n::Pick(L"用默认应用打开", L"Open in default app");
    const std::wstring& hint = text.hint;

    const float width = (std::min)(440.0f * s, content.right - content.left - 32.0f * s);
    const float height = (text.store_id ? 250.0f : 226.0f) * s;
    if (width < 160.0f * s || content.bottom - content.top < height) return;
    const float cx = (content.left + content.right) * 0.5f;
    const float top = std::round((content.top + content.bottom - height) * 0.5f);
    const auto card = D2D1::RectF(cx - width * 0.5f, top, cx + width * 0.5f, top + height);
    ComPtr<ID2D1SolidColorBrush> fill, line;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.05f) : D2D1::ColorF(0xFFFFFF, 1.0f), &fill);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0x000000, 0.07f), &line);
    const auto rounded = D2D1::RoundedRect(card, 12.0f * s, 12.0f * s);
    if (fill.get()) dc->FillRoundedRectangle(rounded, fill.get());
    if (line.get()) dc->DrawRoundedRectangle(rounded, line.get(), 1.0f);

    // Film strip (video) or picture (image) with a slash (48-unit box).
    float y = top + 24.0f * s;
    if (text.picture) {
        const float u = 44.0f * s / 48.0f;
        const float ox = cx - 24.0f * u, oy = y;
        const auto P = [&](float x, float yy) { return D2D1::Point2F(ox + x * u, oy + yy * u); };
        ID2D1Brush* ink = tint(0.85f);
        const float w = 2.2f * u;
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(P(6, 9).x, P(6, 9).y, P(42, 39).x, P(42, 39).y),
                                                   4.0f * u, 4.0f * u), ink, w);
        dc->DrawEllipse(D2D1::Ellipse(P(16, 18), 3.2f * u, 3.2f * u), ink, w);
        dc->DrawLine(P(8, 34), P(19, 24), ink, w);
        dc->DrawLine(P(19, 24), P(26, 30), ink, w);
        dc->DrawLine(P(26, 30), P(32, 23), ink, w);
        dc->DrawLine(P(32, 23), P(40, 32), ink, w);
        dc->DrawLine(P(8, 42), P(40, 6), ink, 2.6f * u);
        y += 44.0f * s + 10.0f * s;
    } else {
        const float u = 44.0f * s / 48.0f;
        const float ox = cx - 24.0f * u, oy = y;
        const auto P = [&](float x, float yy) { return D2D1::Point2F(ox + x * u, oy + yy * u); };
        ID2D1Brush* ink = tint(0.85f);
        const float w = 2.2f * u;
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(P(6, 11).x, P(6, 11).y, P(42, 37).x, P(42, 37).y),
                                                   4.0f * u, 4.0f * u), ink, w);
        dc->DrawLine(P(6, 18), P(42, 18), ink, w);
        for (float x : {13.0f, 22.0f, 31.0f, 40.0f}) dc->DrawLine(P(x, 11), P(x - 3.0f, 18), ink, w);
        dc->DrawLine(P(8, 42), P(40, 6), ink, 2.6f * u);
        y += 44.0f * s + 10.0f * s;
    }
    const auto ta = title_format->GetTextAlignment();
    const auto tp = title_format->GetParagraphAlignment();
    const auto sa = caption->GetTextAlignment();
    const auto sp = caption->GetParagraphAlignment();
    title_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    title_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    caption->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    caption->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const float inner_l = card.left + 20.0f * s, inner_r = card.right - 20.0f * s;
    dc->DrawTextW(title.data(), static_cast<UINT32>(title.size()), title_format,
                  D2D1::RectF(inner_l, y, inner_r, y + 24.0f * s), tint(1.0f), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    y += 28.0f * s;
    ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(body.data(), static_cast<UINT32>(body.size()), caption,
                              inner_r - inner_l, 44.0f * s, &layout);
    if (layout.get()) {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_RANGE{static_cast<UINT32>(lead.size()), static_cast<UINT32>(text.name.size())});
        dc->DrawTextLayout(D2D1::Point2F(inner_l, y), layout.get(), tint(0.72f));
    }
    y += 44.0f * s + 16.0f * s;

    // Buttons: accent "Get extension" (when the Store has one) + "Open".
    const float bh = 32.0f * s;
    const float get_w = get.empty() ? 0.0f : typography::MeasureAdvance(factory, caption, get) + 32.0f * s;
    const float open_w = typography::MeasureAdvance(factory, caption, open) + 32.0f * s;
    const float gap = get.empty() ? 0.0f : 10.0f * s;
    float bx = cx - (get_w + gap + open_w) * 0.5f;
    codec_store_id_ = text.store_id ? text.store_id : L"";
    if (!get.empty()) {
        codec_store_rect_ = D2D1::RectF(bx, y, bx + get_w, y + bh);
        ComPtr<ID2D1SolidColorBrush> accent, on_accent;
        dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x60CDFF) : D2D1::ColorF(0x005FB8), &accent);
        dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x062030) : D2D1::ColorF(0xFFFFFF), &on_accent);
        if (accent.get() && on_accent.get()) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(codec_store_rect_, 5.0f * s, 5.0f * s), accent.get());
            dc->DrawTextW(get.data(), static_cast<UINT32>(get.size()), caption, codec_store_rect_,
                          on_accent.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        bx += get_w + gap;
    }
    codec_open_rect_ = D2D1::RectF(bx, y, bx + open_w, y + bh);
    dc->FillRoundedRectangle(D2D1::RoundedRect(codec_open_rect_, 5.0f * s, 5.0f * s),
                             tint(dark_ ? 0.08f : 0.04f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(codec_open_rect_, 5.0f * s, 5.0f * s),
                             tint(dark_ ? 0.10f : 0.12f), 1.0f);
    dc->DrawTextW(open.data(), static_cast<UINT32>(open.size()), caption, codec_open_rect_, tint(1.0f),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
    y += bh + 10.0f * s;
    if (text.store_id && !hint.empty())
        dc->DrawTextW(hint.data(), static_cast<UINT32>(hint.size()), caption,
                      D2D1::RectF(inner_l, y, inner_r, y + 20.0f * s), tint(0.5f), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    title_format->SetTextAlignment(ta);
    title_format->SetParagraphAlignment(tp);
    caption->SetTextAlignment(sa);
    caption->SetParagraphAlignment(sp);
    brush->SetColor(original);
}
} // namespace pulse::ui