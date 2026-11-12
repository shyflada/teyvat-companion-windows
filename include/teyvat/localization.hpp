#pragma once

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QSizeF>
#include <QVector>
#include <optional>
#include <stop_token>

namespace teyvat {

struct RecordedPosition {
    double x = 0, y = 0, uncertainty = 0, confidence = 0;
};

enum class LocalizationMethod { Correlation, Orb };

struct LocalizationConfig {
    QImage reference;
    QRect roi;
    QPointF origin;
    QSizeF units_per_pixel{1, 1};
    QVector<double> scales{1};
    QVector<double> rotations{0};
    LocalizationMethod method = LocalizationMethod::Correlation;
    QImage presence_template;
    QRect presence_roi;
    double presence_minimum = .75;
    bool allow_rotation = false;
};

enum class LocalizationReason { Matched, LowTexture, WeakMatch, Ambiguous, OutsideReference,
    MinimapAbsent, InsufficientFeatures, InconsistentGeometry };
struct LocalizationResult {
    std::optional<RecordedPosition> position;
    LocalizationReason reason = LocalizationReason::WeakMatch;
    double similarity = 0, runner_up = 0, rotation = 0, scale = 1;
    int features = 0, matches = 0, inliers = 0;
    double reprojection_rms = 0;
    bool limited_support = false;
};
struct LocalizationSample {
    qint64 time_ms = 0;
    LocalizationResult estimate;
};
struct CameraHeading {
    double degrees = 0; // Clockwise from the top of the minimap, independent of the avatar.
    double quality = 0;
};

void validate_localization(const LocalizationConfig& config);
[[nodiscard]] bool minimap_present(const QImage& image, const LocalizationConfig& config);
// Calibrated correlation or ORB/similarity geometry. Quality is not a probability.
[[nodiscard]] LocalizationResult localize_minimap(const QImage& image,
    const LocalizationConfig& config, std::stop_token stop = {});
// Compare the camera's bright field-of-view sector with the matched map terrain.
// Missing/ambiguous evidence produces no heading, never the avatar's facing.
[[nodiscard]] std::optional<CameraHeading> estimate_camera_heading(const QImage& image,
    const LocalizationConfig& config, const LocalizationResult& pose);
// Learn only the small fixed Eye/HUD crop after verifying the centered cyan
// player symbol and the high-contrast Eye icon. The image stays in memory.
[[nodiscard]] std::optional<QImage> learn_hud_presence(const QImage& image, const LocalizationConfig& config);

} // namespace teyvat
