#include "teyvat/localization.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace teyvat {
LocalizationResult feature_localization(const QImage&, const LocalizationConfig&, std::stop_token);
bool minimap_present(const QImage&, const LocalizationConfig&);
namespace {
void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) { throw std::runtime_error("Analysis cancelled. No progress was changed."); }
}
struct Sample { int x, y; double a, b, c, d, value; };
struct Pattern { std::vector<Sample> samples; double square = 0; };
struct Peak { int x, y; double score, angle, scale; };

Pattern pattern(const QImage& crop, int stride, double scale, double angle) {
    Pattern result;
    const double cx = (crop.width() - 1) / 2.0, cy = (crop.height() - 1) / 2.0;
    const double radius = std::min(cx, cy), radians = angle * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians) * scale, sine = std::sin(radians) * scale;
    double sum = 0;
    for (int y = 2; y < crop.height() - 2; y += stride) {
        const auto* row = crop.constScanLine(y);
        for (int x = 2; x < crop.width() - 2; x += stride) {
            const double dx = x - cx, dy = y - cy, distance = std::hypot(dx, dy);
            if (distance < radius * 0.28 || distance > radius * 0.90) { continue; }
            const double rx = cosine * dx - sine * dy, ry = sine * dx + cosine * dy;
            const int ix = static_cast<int>(std::floor(rx)), iy = static_cast<int>(std::floor(ry));
            const double fx = rx - ix, fy = ry - iy, value = row[x];
            result.samples.push_back({ix, iy, (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy, value}); sum += value;
        }
    }
    if (result.samples.empty()) { return result; }
    const double mean = sum / result.samples.size();
    for (auto& sample : result.samples) { sample.value -= mean; result.square += sample.value * sample.value; }
    return result;
}

double correlation(const QImage& reference, const Pattern& p, int x, int y) {
    double sum = 0, square = 0, product = 0;
    for (const auto& s : p.samples) {
        const auto* top = reference.constScanLine(y + s.y) + x + s.x;
        const auto* bottom = reference.constScanLine(y + s.y + 1) + x + s.x;
        const double value = s.a * top[0] + s.b * top[1] + s.c * bottom[0] + s.d * bottom[1];
        sum += value; square += value * value; product += value * s.value;
    }
    const double variance = square - sum * sum / p.samples.size();
    if (variance < 100 * p.samples.size()) { return -1; }
    return std::clamp(product / std::sqrt(p.square * variance), -1.0, 1.0);
}

void retain_peak(std::vector<Peak>& peaks, const Peak& candidate) {
    for (auto& peak : peaks) {
        if (std::hypot(peak.x - candidate.x, peak.y - candidate.y) < 8) {
            if (candidate.score > peak.score) { peak = candidate; }
            std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
            return;
        }
    }
    peaks.push_back(candidate);
    std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (peaks.size() > 8) { peaks.pop_back(); }
}
} // namespace

void validate_localization(const LocalizationConfig& config) {
    const auto& r = config.reference;
    const bool features = config.method == LocalizationMethod::Orb;
    const int reference_limit = features ? 1024 : 384, roi_limit = features ? 512 : 128;
    if ((config.method != LocalizationMethod::Orb && config.method != LocalizationMethod::Correlation)
        || (!features && config.allow_rotation)
        || r.isNull() || r.width() < 96 || r.height() < 96 || r.width() > reference_limit || r.height() > reference_limit
        || config.roi.width() < 48 || config.roi.width() > roi_limit || config.roi.width() != config.roi.height()
        || config.roi.x() < 0 || config.roi.y() < 0 || config.roi.x() > (features ? 7679 : 1919) || config.roi.y() > (features ? 4319 : 1079)
        || !std::isfinite(config.origin.x()) || !std::isfinite(config.origin.y()) || config.origin.x() < 0 || config.origin.y() < 0
        || !std::isfinite(config.units_per_pixel.width()) || !std::isfinite(config.units_per_pixel.height())
        || config.units_per_pixel.width() < 0.001 || config.units_per_pixel.height() < 0.001
        || config.units_per_pixel.width() > 1000 || config.units_per_pixel.height() > 1000
        || config.scales.isEmpty() || config.rotations.isEmpty() || config.scales.size() * config.rotations.size() > 16) {
        throw std::runtime_error("Invalid minimap calibration. Correlation: reference 96..384, ROI 48..128. ORB: reference 96..1024, ROI 48..512. At most 16 scale/rotation pairs.");
    }
    for (double scale : config.scales) {
        if (!std::isfinite(scale) || scale < 0.5 || scale > 4) { throw std::runtime_error("Minimap scales must be between 0.5 and 4 reference pixels per minimap pixel."); }
    }
    for (double angle : config.rotations) {
        if (!std::isfinite(angle) || angle < -180 || angle > 360) { throw std::runtime_error("Minimap rotations must be between -180 and 360 degrees."); }
    }
    if (!config.presence_template.isNull() && (config.presence_template.size() != config.presence_roi.size()
        || config.presence_roi.x() < 0 || config.presence_roi.y() < 0 || config.presence_roi.width() < 16 || config.presence_roi.height() < 16
        || config.presence_roi.width() > 128 || config.presence_roi.height() > 128
        || !std::isfinite(config.presence_minimum) || config.presence_minimum < .7 || config.presence_minimum > 1)) {
        throw std::runtime_error("Invalid fixed-HUD presence calibration.");
    }
    if (config.origin.x() + (r.width() - 1) * config.units_per_pixel.width() > 1000000
        || config.origin.y() + (r.height() - 1) * config.units_per_pixel.height() > 1000000) {
        throw std::runtime_error("Calibrated reference coordinates exceed supported map bounds.");
    }
}

LocalizationResult localize_minimap(const QImage& image, const LocalizationConfig& config, std::stop_token stop) {
    validate_localization(config); cancelled(stop);
    if (!image.rect().contains(config.roi)) { throw std::runtime_error("Minimap ROI exceeds the recording frame."); }
    if (!minimap_present(image, config)) { LocalizationResult result; result.reason = LocalizationReason::MinimapAbsent; return result; }
    if (config.method == LocalizationMethod::Orb) { return feature_localization(image, config, stop); }
    const auto crop = image.copy(config.roi).convertToFormat(QImage::Format_Grayscale8);
    const auto reference = config.reference.convertToFormat(QImage::Format_Grayscale8);
    const auto texture = pattern(crop, 3, 1, 0);
    LocalizationResult result;
    if (texture.samples.size() < 24 || texture.square < texture.samples.size() * 100) { result.reason = LocalizationReason::LowTexture; return result; }
    std::vector<Peak> refined;
    bool fits = false;
    for (double scale : config.scales) {
        const int edge = static_cast<int>(std::ceil(config.roi.width() * 0.5 * scale)) + 2;
        if (edge * 2 >= reference.width() || edge * 2 >= reference.height()) { continue; }
        fits = true;
        for (double angle : config.rotations) {
            cancelled(stop);
            const auto coarse = pattern(crop, 8, scale, angle), fine = pattern(crop, 3, scale, angle);
            if (coarse.samples.size() < 12 || coarse.square < coarse.samples.size() * 100) { continue; }
            std::vector<Peak> seeds;
            // ponytail: bounded tile search (384px, 16 poses); use feature matching
            // only after representative recordings show this calibrated search insufficient.
            for (int y = edge; y < reference.height() - edge; y += 4) {
                cancelled(stop);
                for (int x = edge; x < reference.width() - edge; x += 4) {
                    const double score = correlation(reference, coarse, x, y);
                    if (score >= 0.60) { retain_peak(seeds, {x, y, score, angle, scale}); }
                }
            }
            for (const auto& seed : seeds) {
                for (int y = std::max(edge, seed.y - 4); y <= std::min(reference.height() - edge - 1, seed.y + 4); ++y) {
                    for (int x = std::max(edge, seed.x - 4); x <= std::min(reference.width() - edge - 1, seed.x + 4); ++x) {
                        refined.push_back({x, y, correlation(reference, fine, x, y), angle, scale});
                    }
                }
            }
        }
    }
    if (!fits) { result.reason = LocalizationReason::OutsideReference; return result; }
    if (refined.empty()) { return result; }
    std::sort(refined.begin(), refined.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    const auto& best = refined.front();
    result.similarity = best.score; result.rotation = best.angle; result.scale = best.scale;
    for (const auto& next : refined) {
        if (std::hypot(next.x - best.x, next.y - best.y) > 6) { result.runner_up = std::max(0.0, next.score); break; }
    }
    if (best.score < 0.97) { return result; }
    if (best.score - result.runner_up < 0.06) { result.reason = LocalizationReason::Ambiguous; return result; }
    result.reason = LocalizationReason::Matched;
    result.position = RecordedPosition{config.origin.x() + best.x * config.units_per_pixel.width(),
        config.origin.y() + best.y * config.units_per_pixel.height(),
        2 * std::max(config.units_per_pixel.width(), config.units_per_pixel.height()), best.score};
    return result;
}
} // namespace teyvat
