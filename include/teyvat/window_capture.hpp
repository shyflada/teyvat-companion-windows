#pragma once
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <optional>

namespace teyvat {
struct WindowSource { quintptr handle = 0; quint32 process = 0; QString title; QSize client_size; };
enum class CaptureReason { Ready, Inactive, Changed, Occluded, Unavailable };
struct CapturedRegions { CaptureReason reason = CaptureReason::Unavailable; QVector<QImage> images; };
[[nodiscard]] std::optional<WindowSource> inspect_window(quintptr handle);
[[nodiscard]] std::optional<WindowSource> find_game_window();
[[nodiscard]] bool source_active(const WindowSource& source);
// Visible client pixels only. No game process access, input, hooks or protected-surface fallback.
[[nodiscard]] CapturedRegions capture_regions(const WindowSource& source, QSize calibrated_size,
    const QVector<QRect>& regions, quintptr own_overlay = 0);
} // namespace teyvat
