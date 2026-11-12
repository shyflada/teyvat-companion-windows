#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace teyvat {
enum class VerticalRelation { Unknown, Above, Below, Level };
struct HeightBand {
    double minimum = 0, maximum = 0;
    std::string frame;
};
[[nodiscard]] VerticalRelation compare_height(const std::optional<HeightBand>& player,
    const std::optional<HeightBand>& target, double level_tolerance_metres = 2);

enum class PassageState { Unknown, Open, Closed };
enum class Traversal { Walk, Climb, Swim, Glide, Stairs, Portal, Teleport };
struct RouteNode {
    double x = 0, y = 0, z = 0; // Metres in the verified scene's XYZ frame.
    std::string scene, floor;
};
struct RouteEdge {
    std::uint32_t from = 0, to = 0;
    Traversal mode = Traversal::Walk;
    PassageState state = PassageState::Unknown;
    std::optional<double> seconds; // Unknown costs cannot establish a fastest route.
};
struct RouteTeleport {
    std::string id;
    std::uint32_t destination = 0;
    double seconds = 0; // Includes the measured menu/loading estimate.
};
struct RouteOptions {
    std::span<const std::string> unlocked_teleports;
    std::span<const std::string> teleport_allowed_scenes;
};
struct RouteStep {
    std::uint32_t from = 0, to = 0;
    Traversal mode = Traversal::Walk;
    double seconds = 0, locomotion_metres = 0;
};
struct Route {
    std::vector<RouteStep> steps;
    double estimated_seconds = 0, locomotion_metres = 0;
};
// Edges must come from verified passages and traversal costs. This planner never
// creates a connection from proximity, a terrain height or a shared X/Z alone.
class RoutePlanner final {
public:
    RoutePlanner(std::vector<RouteNode> nodes, std::vector<RouteEdge> edges,
        std::vector<RouteTeleport> teleports = {});
    [[nodiscard]] std::optional<Route> fastest(std::uint32_t from, std::uint32_t to,
        const RouteOptions& options = {}, std::stop_token stop = {}) const;
    [[nodiscard]] const std::vector<RouteNode>& nodes() const { return nodes_; }
private:
    std::vector<RouteNode> nodes_;
    std::vector<RouteEdge> edges_;
    std::vector<RouteTeleport> teleports_;
    std::vector<std::vector<std::uint32_t>> outgoing_;
};
} // namespace teyvat
