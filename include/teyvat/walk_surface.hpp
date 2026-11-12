#pragma once
#include "teyvat/route.hpp"
#include <array>

namespace teyvat {
struct SurfacePoint { double x = 0, y = 0, z = 0; };
struct CollisionFace {
    std::array<SurfacePoint, 3> corners;
    bool supports_walking = true;
};
struct WalkBakeSettings {
    double min_x = 0, max_x = 0, min_z = 0, max_z = 0;
    double spacing = 1, radius = .4, height = 1.9, step = .35, max_slope_degrees = 40;
    double metres_per_second = 4;
    std::string scene, floor;
};
struct WalkSurface {
    std::vector<RouteNode> nodes;
    std::vector<RouteEdge> passages;
};
// Bake a bounded area from supplied collision triangles. All edges require
// continuous support and a clear swept body volume. Overlapping X/Z surfaces
// remain separate at their actual elevations.
[[nodiscard]] WalkSurface bake_walk_surface(std::span<const CollisionFace> faces,
    const WalkBakeSettings& settings, std::stop_token stop = {});
} // namespace teyvat
