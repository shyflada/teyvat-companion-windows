#include "teyvat/navigation.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace teyvat {
namespace {
void valid(XY point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > 1e6 || std::abs(point.y) > 1e6) {
        throw std::invalid_argument("Calibration coordinates must be finite and within +/-1000000.");
    }
}
bool chest_category(std::string_view category) {
    return category == "common" || category == "exquisite" || category == "precious"
        || category == "luxurious" || category == "remarkable";
}
bool trusted(const NavigationPose& pose) {
    return !validation_error(pose.position) && std::isfinite(pose.confidence) && pose.confidence >= .95 && pose.confidence <= 1
        && std::isfinite(pose.uncertainty) && pose.uncertainty >= 0 && pose.uncertainty <= 10000
        && std::isfinite(pose.units_per_pixel_x) && pose.units_per_pixel_x > 0
        && std::isfinite(pose.units_per_pixel_y) && pose.units_per_pixel_y > 0;
}
}
XY AffineCalibration::map(XY p) const { valid(p); const XY result{a * p.x + b * p.y + c, d * p.x + e * p.y + f}; valid(result); return result; }

AffineCalibration calibrate_coordinates(std::span<const ControlPoint> fit, std::span<const ControlPoint> checks, double maximum_error) {
    if (fit.size() < 3 || fit.size() > 64 || checks.empty() || checks.size() > 64 || !std::isfinite(maximum_error) || maximum_error <= 0 || maximum_error > 10000) {
        throw std::invalid_argument("Use 3..64 fit points, 1..64 independent checks and a positive bounded error tolerance.");
    }
    XY s, t;
    for (const auto& p : fit) { valid(p.source); valid(p.destination); s.x += p.source.x; s.y += p.source.y; t.x += p.destination.x; t.y += p.destination.y; }
    const double n = static_cast<double>(fit.size()); s.x /= n; s.y /= n; t.x /= n; t.y /= n;
    double xx = 0, xy = 0, yy = 0, xu = 0, yu = 0, xv = 0, yv = 0;
    for (const auto& p : fit) {
        const double x = p.source.x - s.x, y = p.source.y - s.y, u = p.destination.x - t.x, v = p.destination.y - t.y;
        xx += x * x; xy += x * y; yy += y * y; xu += x * u; yu += y * u; xv += x * v; yv += y * v;
    }
    const double det = xx * yy - xy * xy;
    if (xx <= 0 || yy <= 0 || det <= xx * yy * 1e-8) { throw std::invalid_argument("Calibration points are collinear or ill-conditioned."); }
    AffineCalibration result;
    result.a = (xu * yy - yu * xy) / det; result.b = (yu * xx - xu * xy) / det;
    result.d = (xv * yy - yv * xy) / det; result.e = (yv * xx - xv * xy) / det;
    result.c = t.x - result.a * s.x - result.b * s.y; result.f = t.y - result.d * s.x - result.e * s.y;
    const double area = std::abs(result.a * result.e - result.b * result.d);
    if (!std::isfinite(area) || area < 1e-10) { throw std::invalid_argument("Calibration collapses the destination plane."); }
    double square = 0, fit_max = 0;
    for (const auto& p : fit) { const auto q = result.map(p.source); const double error = std::hypot(q.x - p.destination.x, q.y - p.destination.y); square += error * error; fit_max = std::max(fit_max, error); }
    result.fit_rms = std::sqrt(square / n);
    for (const auto& p : checks) {
        valid(p.source); valid(p.destination);
        if (std::any_of(fit.begin(), fit.end(), [&](const auto& f) { return std::hypot(f.source.x - p.source.x, f.source.y - p.source.y) < 1e-6; })) {
            throw std::invalid_argument("Check points must be independent of fitting points.");
        }
        const auto q = result.map(p.source); result.check_max_error = std::max(result.check_max_error, std::hypot(q.x - p.destination.x, q.y - p.destination.y));
    }
    if (fit_max > maximum_error || result.check_max_error > maximum_error) { throw std::invalid_argument("Calibration error exceeds the declared tolerance."); }
    return result;
}

std::optional<Guidance> TargetSelector::update(const std::optional<NavigationPose>& pose, std::span<const NavigationChest> chests) {
    if (!pose || !trusted(*pose)) { return {}; }
    const auto eligible = [&](const NavigationChest& chest) {
        return !chest.retired && !is_confirmed(chest.status) && chest_category(chest.marker.category)
            && !validation_error(chest.marker) && approximate_map_distance(pose->position, chest.marker.position).has_value()
            && std::find(skipped_.begin(), skipped_.end(), chest.marker.key) == skipped_.end();
    };
    const NavigationChest* selected = nullptr;
    for (const auto& chest : chests) { if (target_ && chest.marker.key == *target_ && eligible(chest)) { selected = &chest; break; } }
    if (!selected) {
        target_.reset(); double nearest = 0;
        for (const auto& chest : chests) {
            if (!eligible(chest)) { continue; }
            const double distance = *approximate_map_distance(pose->position, chest.marker.position);
            const bool tie = selected && std::abs(distance - nearest) < 1e-9;
            if (!selected || distance < nearest - 1e-9 || (tie && std::pair(chest.marker.key.source, chest.marker.key.id) < std::pair(selected->marker.key.source, selected->marker.key.id))) {
                selected = &chest; nearest = distance;
            }
        }
        if (selected) { target_ = selected->marker.key; }
    }
    if (!selected) { return {}; }
    const auto& p = pose->position; const auto& t = selected->marker.position;
    Guidance result{*target_, *approximate_map_distance(p, t), {}};
    if (pose->minimap_rotation && std::isfinite(*pose->minimap_rotation) && result.map_units > pose->uncertainty) {
        const double dx = (t.x - p.x) / pose->units_per_pixel_x, dy = (t.y - p.y) / pose->units_per_pixel_y;
        result.minimap_bearing = std::remainder(std::atan2(dx, -dy) * 180 / std::numbers::pi - *pose->minimap_rotation, 360.0);
    }
    return result;
}
void TargetSelector::skip() { if (target_) { skipped_.push_back(*target_); target_.reset(); } }
void TargetSelector::restore(std::optional<MarkerKey> target, std::vector<MarkerKey> skipped) { target_ = std::move(target); skipped_ = std::move(skipped); }
void TargetSelector::reset() { target_.reset(); skipped_.clear(); }
} // namespace teyvat
