#pragma once
#include "teyvat/model.hpp"
#include <span>
#include <vector>

namespace teyvat {
struct XY { double x = 0, y = 0; };
struct ControlPoint { XY source, destination; };
struct AffineCalibration {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
    double fit_rms = 0, check_max_error = 0;
    [[nodiscard]] XY map(XY point) const;
};
// Independent check points are mandatory; a three-point fit alone proves no accuracy.
[[nodiscard]] AffineCalibration calibrate_coordinates(std::span<const ControlPoint> fit,
    std::span<const ControlPoint> checks, double maximum_error);

struct NavigationPose {
    MapPoint position;
    double confidence = 0, uncertainty = 0;
    // Rotation from minimap pixels to reference pixels, clockwise in image coordinates.
    std::optional<double> minimap_rotation;
    double units_per_pixel_x = 1, units_per_pixel_y = 1;
};
struct NavigationChest {
    Marker marker;
    CollectionStatus status = CollectionStatus::NotCollected;
    bool retired = false;
};
struct Guidance {
    MarkerKey target;
    double map_units = 0;
    std::optional<double> minimap_bearing;
};
class TargetSelector final {
public:
    [[nodiscard]] std::optional<Guidance> update(const std::optional<NavigationPose>& pose,
        std::span<const NavigationChest> chests);
    void skip();
    void restore(std::optional<MarkerKey> target, std::vector<MarkerKey> skipped);
    void reset();
    [[nodiscard]] const std::optional<MarkerKey>& target() const { return target_; }
    [[nodiscard]] const std::vector<MarkerKey>& skipped() const { return skipped_; }
private:
    std::optional<MarkerKey> target_;
    std::vector<MarkerKey> skipped_;
};
} // namespace teyvat
