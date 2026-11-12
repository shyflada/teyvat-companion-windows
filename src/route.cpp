#include "teyvat/route.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace teyvat {
namespace {
bool finite(double v) { return std::isfinite(v) && std::abs(v) <= 1e6; }
bool named(const std::string& s) { return !s.empty() && s.size() <= 128 && s.find('\0') == std::string::npos; }
void band(const HeightBand& h) {
    if (!finite(h.minimum) || !finite(h.maximum) || h.minimum > h.maximum || !named(h.frame)) {
        throw std::invalid_argument("Invalid bounded height interval or coordinate frame.");
    }
}
double length(const RouteNode& a, const RouteNode& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }
}
VerticalRelation compare_height(const std::optional<HeightBand>& player, const std::optional<HeightBand>& target, double tolerance) {
    if (!std::isfinite(tolerance) || tolerance < 0 || tolerance > 100) { throw std::invalid_argument("Invalid height comparison tolerance."); }
    if (player) { band(*player); } if (target) { band(*target); }
    if (!player || !target || player->frame != target->frame) { return VerticalRelation::Unknown; }
    const double minimum = target->minimum - player->maximum, maximum = target->maximum - player->minimum;
    if (minimum > tolerance) { return VerticalRelation::Above; }
    if (maximum < -tolerance) { return VerticalRelation::Below; }
    if (minimum >= -tolerance && maximum <= tolerance) { return VerticalRelation::Level; }
    return VerticalRelation::Unknown;
}
RoutePlanner::RoutePlanner(std::vector<RouteNode> nodes, std::vector<RouteEdge> edges, std::vector<RouteTeleport> teleports)
    : nodes_(std::move(nodes)), edges_(std::move(edges)), teleports_(std::move(teleports)) {
    if (nodes_.empty() || nodes_.size() > 500000 || edges_.size() > 2000000 || teleports_.size() > 10000) {
        throw std::invalid_argument("Navigation graph exceeds supported bounds.");
    }
    outgoing_.resize(nodes_.size());
    for (const auto& n : nodes_) {
        if (!finite(n.x) || !finite(n.y) || !finite(n.z) || !named(n.scene) || !named(n.floor)) {
            throw std::invalid_argument("Every route node needs verified finite XYZ, scene and floor.");
        }
    }
    for (std::size_t i = 0; i < edges_.size(); ++i) {
        const auto& e = edges_[i];
        if (e.from >= nodes_.size() || e.to >= nodes_.size() || e.from == e.to
            || e.mode < Traversal::Walk || e.mode >= Traversal::Teleport
            || e.state < PassageState::Unknown || e.state > PassageState::Closed
            || (e.seconds && (!std::isfinite(*e.seconds) || *e.seconds <= 0 || *e.seconds > 86400))) {
            throw std::invalid_argument("Invalid directed passage or travel-time estimate.");
        }
        const auto& a = nodes_[e.from]; const auto& b = nodes_[e.to];
        if ((a.scene != b.scene && e.mode != Traversal::Portal)
            || (a.floor != b.floor && (e.mode == Traversal::Walk || e.mode == Traversal::Swim))) {
            throw std::invalid_argument("A floor/scene change requires an explicit verified transition.");
        }
        outgoing_[e.from].push_back(static_cast<std::uint32_t>(i));
    }
    std::unordered_set<std::string> ids;
    for (const auto& t : teleports_) {
        if (!named(t.id) || !ids.insert(t.id).second || t.destination >= nodes_.size()
            || !std::isfinite(t.seconds) || t.seconds <= 0 || t.seconds > 600) {
            throw std::invalid_argument("Invalid or duplicate teleport destination.");
        }
    }
}
std::optional<Route> RoutePlanner::fastest(std::uint32_t from, std::uint32_t to, const RouteOptions& options, std::stop_token stop) const {
    if (from >= nodes_.size() || to >= nodes_.size() || options.unlocked_teleports.size() > 10000
        || options.teleport_allowed_scenes.size() > 10000) { throw std::invalid_argument("Invalid route endpoints or availability state."); }
    const std::unordered_set<std::string> unlocked(options.unlocked_teleports.begin(), options.unlocked_teleports.end());
    const std::unordered_set<std::string> teleport_scenes(options.teleport_allowed_scenes.begin(), options.teleport_allowed_scenes.end());
    for (const auto& id : unlocked) { if (!named(id)) { throw std::invalid_argument("Invalid unlocked teleport ID."); } }
    for (const auto& scene : teleport_scenes) { if (!named(scene)) { throw std::invalid_argument("Invalid teleport scene."); } }
    const double infinity = std::numeric_limits<double>::infinity();
    std::vector<double> costs(nodes_.size(), infinity); std::vector<std::optional<RouteStep>> previous(nodes_.size());
    using Entry = std::pair<double, std::uint32_t>;
    // A geometric A* heuristic overestimates routes containing cheap teleports.
    // Dijkstra remains correct for explicit stairs, portals and teleport costs.
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    costs[from] = 0; queue.emplace(0, from); bool teleports_seeded = false;
    const auto relax = [&](const RouteStep& step, double cost) {
        const double next = cost + step.seconds;
        if (next < costs[step.to]) { costs[step.to] = next; previous[step.to] = step; queue.emplace(next, step.to); }
    };
    while (!queue.empty()) {
        if (stop.stop_requested()) { return {}; }
        const auto [cost, node] = queue.top(); queue.pop();
        if (cost != costs[node]) { continue; }
        if (node == to) {
            Route result; result.estimated_seconds = cost;
            for (auto at = to; at != from;) {
                const auto& p = previous[at]; if (!p) { throw std::logic_error("Route predecessor is missing."); }
                result.steps.push_back(*p); result.locomotion_metres += p->locomotion_metres; at = p->from;
            }
            std::reverse(result.steps.begin(), result.steps.end()); return result;
        }
        // The first eligible settled node is the cheapest departure. With fixed
        // positive per-destination costs, reseeding from later nodes cannot help.
        if (!teleports_seeded && teleport_scenes.contains(nodes_[node].scene)) {
            teleports_seeded = true;
            for (const auto& t : teleports_) { if (unlocked.contains(t.id) && t.destination != node) {
                relax({node, t.destination, Traversal::Teleport, t.seconds, 0}, cost);
            } }
        }
        for (const auto index : outgoing_[node]) {
            const auto& e = edges_[index];
            if (e.state != PassageState::Open || !e.seconds) { continue; }
            relax({node, e.to, e.mode, *e.seconds, e.mode == Traversal::Portal ? 0 : length(nodes_[node], nodes_[e.to])}, cost);
        }
    }
    return {};
}
} // namespace teyvat
