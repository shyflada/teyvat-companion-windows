#include "teyvat/localization.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace teyvat {
bool minimap_present(const QImage&, const LocalizationConfig&);
namespace {
cv::Mat pixels(const QImage& image) {
    return cv::Mat(image.height(), image.width(), CV_8UC4, const_cast<uchar*>(image.constBits()), image.bytesPerLine());
}
constexpr int bins = 72;
struct RingPixel { int x, y, bin; };
struct Sample { double background = 0, observed = 0, residual = 0; int bin = 0; };

const std::vector<RingPixel>& camera_ring(QSize size) {
    thread_local QSize previous;
    thread_local std::vector<RingPixel> ring;
    if (size == previous) { return ring; }
    ring.clear();
    const double cx = (size.width() - 1) * .5, cy = (size.height() - 1) * .5;
    const double scale = size.width() * .5;
    for (int y = 0; y < size.height(); ++y) { for (int x = 0; x < size.width(); ++x) {
        const double dx = x - cx, dy = cy - y, radius = std::hypot(dx, dy) / scale;
        if (radius < .30 || radius > .73) { continue; }
        const auto theta = std::atan2(dx, dy);
        const int bin = static_cast<int>((theta + std::numbers::pi) * bins / (2 * std::numbers::pi)) % bins;
        ring.push_back({x, y, bin});
    } }
    previous = size;
    return ring;
}
}
std::optional<QImage> learn_hud_presence(const QImage& image, const LocalizationConfig& config) {
    if (!image.rect().contains(config.roi.united(config.presence_roi)) || config.presence_roi.width() < 16 || config.presence_roi.height() < 16) { return {}; }
    const auto eye = image.copy(config.presence_roi).convertToFormat(QImage::Format_ARGB32); int white = 0, dark = 0;
    for (int y = 0; y < eye.height(); ++y) { const auto* row = reinterpret_cast<const QRgb*>(eye.constScanLine(y));
        for (int x = 0; x < eye.width(); ++x) { const auto c = row[x];
            if (qRed(c) > 215 && qGreen(c) > 215 && qBlue(c) > 215) { ++white; }
            if (qGray(c) < 70) { ++dark; }
        }
    }
    const auto area = eye.width() * eye.height(); if (white < area * .12 || white > area * .65 || dark < area * .02) { return {}; }
    const auto center = config.roi.center(); const int r = qRound(config.roi.width() * .09);
    const auto icon = image.copy(QRect(center - QPoint(r, r), QSize(r * 2 + 1, r * 2 + 1))).convertToFormat(QImage::Format_ARGB32); int cyan = 0;
    for (int y = 0; y < icon.height(); ++y) { const auto* row = reinterpret_cast<const QRgb*>(icon.constScanLine(y));
        for (int x = 0; x < icon.width(); ++x) { const auto c = row[x]; if (qRed(c) < 100 && qGreen(c) > 170 && qBlue(c) > 190) { ++cyan; } }
    }
    if (cyan < config.roi.width() * config.roi.height() * .0004) { return {}; } return eye;
}
std::optional<CameraHeading> estimate_camera_heading(const QImage& image,
    const LocalizationConfig& config, const LocalizationResult& pose) {
    if (!pose.position || pose.reason != LocalizationReason::Matched || config.method != LocalizationMethod::Orb
        || !image.rect().contains(config.roi) || !std::isfinite(pose.scale) || pose.scale < .5 || pose.scale > 4
        || !std::isfinite(pose.rotation) || config.reference.isNull() || !minimap_present(image, config)) { return {}; }
    const auto input = image.copy(config.roi).convertToFormat(QImage::Format_ARGB32);
    const double cx = (input.width() - 1) * .5, cy = (input.height() - 1) * .5;
    const double angle = pose.rotation * std::numbers::pi / 180, a = std::cos(angle) * pose.scale, b = std::sin(angle) * pose.scale;
    const auto center = QPointF((pose.position->x - config.origin.x()) / config.units_per_pixel.width(),
        (pose.position->y - config.origin.y()) / config.units_per_pixel.height());
    cv::Mat transform = (cv::Mat_<double>(2, 3) << a, -b, center.x() - a * cx + b * cy, b, a, center.y() - b * cx - a * cy);
    thread_local QImage reference;
    thread_local qint64 reference_key = 0;
    if (reference_key != config.reference.cacheKey()) {
        reference = config.reference.convertToFormat(QImage::Format_ARGB32);
        reference_key = config.reference.cacheKey();
    }
    cv::Mat background, current;
    cv::warpAffine(pixels(reference), background, transform, {input.width(), input.height()}, cv::WARP_INVERSE_MAP | cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    cv::cvtColor(background, background, cv::COLOR_BGRA2GRAY); cv::cvtColor(pixels(input), current, cv::COLOR_BGRA2GRAY);
    cv::GaussianBlur(background, background, {7, 7}, 1.4); cv::GaussianBlur(current, current, {7, 7}, 1.4);
    const auto& ring = camera_ring(input.size());
    std::vector<Sample> samples; samples.reserve(ring.size());
    const auto* colors = input.constBits(); const auto stride = input.bytesPerLine();
    for (const auto& point : ring) {
        const auto c = reinterpret_cast<const QRgb*>(colors + point.y * stride)[point.x];
        if ((qRed(c) > 220 && qGreen(c) > 220 && qBlue(c) > 220)
            || (qRed(c) < 100 && qGreen(c) > 180 && qBlue(c) > 190) || (qRed(c) > 190 && qGreen(c) > 135 && qBlue(c) < 90)) { continue; }
        const double ref = background.at<uchar>(point.y, point.x), now = current.at<uchar>(point.y, point.x);
        if (ref >= 35 && ref <= 220) { samples.push_back({ref, now, 0, point.bin}); }
    }
    if (samples.size() < 1000) { return {}; }
    double gain = 1, offset = 0;
    // The cone affects a minority of pixels. Trim high residuals when fitting
    // the ordinary minimap's brightness, so bright terrain is not a camera cue.
    for (int pass = 0; pass < 3; ++pass) {
        for (auto& p : samples) { p.residual = p.observed - gain * p.background - offset; }
        std::sort(samples.begin(), samples.end(), [](const auto& l, const auto& r) { return l.residual < r.residual; });
        const auto first = pass == 0 ? 0 : samples.size() / 10, last = pass == 0 ? samples.size() : samples.size() * 8 / 10;
        double x = 0, y = 0, xx = 0, xy = 0; const double n = static_cast<double>(last - first);
        for (auto i = first; i < last; ++i) { const auto& p = samples[i]; x += p.background; y += p.observed; xx += p.background * p.background; xy += p.background * p.observed; }
        const double denominator = n * xx - x * x; if (denominator <= n * n * 16) { return {}; }
        gain = (n * xy - x * y) / denominator; offset = (y - gain * x) / n;
    }
    if (gain < .4 || gain > 1.8 || offset < -90 || offset > 90) { return {}; }
    std::array<double, bins> sums{}, counts{}, weights{};
    for (const auto& p : samples) {
        const double base = gain * p.background + offset;
        sums[p.bin] += std::clamp((p.observed - base - 4) * 160 / std::max(40., 255 - base), 0., 45.); ++counts[p.bin];
    }
    int covered = 0; for (int i = 0; i < bins; ++i) { if (counts[i] >= 10) { weights[i] = sums[i] / counts[i]; ++covered; } }
    if (covered < 60) { return {}; }
    auto sorted = weights; std::sort(sorted.begin(), sorted.end()); const double baseline = sorted[bins / 2];
    double sx = 0, sy = 0, total = 0, peak = 0;
    for (int i = 0; i < bins; ++i) {
        weights[i] = std::max(0., weights[i] - baseline); peak = std::max(peak, weights[i]);
        const double theta = (i + .5) * 2 * std::numbers::pi / bins - std::numbers::pi;
        sx += std::sin(theta) * weights[i]; sy += std::cos(theta) * weights[i]; total += weights[i];
    }
    if (total < 30 || peak < 5) { return {}; }
    const double quality = std::hypot(sx, sy) / total;
    const auto width = std::count_if(weights.begin(), weights.end(), [peak](double w) { return w > peak * .5; });
    if (quality < .6 || width < 4 || width > 24) { return {}; }
    return CameraHeading{std::atan2(sx, sy) * 180 / std::numbers::pi, quality};
}
} // namespace teyvat
