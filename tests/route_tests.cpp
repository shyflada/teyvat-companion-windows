#include "teyvat/route.hpp"
#include "teyvat/walk_surface.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace teyvat;
void check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
template<class F> void rejects(F f) {
    bool rejected = false; try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Invalid navigation data accepted");
}
void heights() {
    const HeightBand p{100, 101, "world-y-v1"};
    check(compare_height(p, HeightBand{110, 111, "world-y-v1"}) == VerticalRelation::Above, "Higher target lost");
    check(compare_height(p, HeightBand{90, 91, "world-y-v1"}) == VerticalRelation::Below, "Lower target lost");
    check(compare_height(p, HeightBand{100, 101, "world-y-v1"}) == VerticalRelation::Level, "Same level lost");
    check(compare_height(p, {}) == VerticalRelation::Unknown, "Missing target height invented");
    check(compare_height({}, p) == VerticalRelation::Unknown, "Missing player height invented");
    check(compare_height(p, HeightBand{110, 111, "another-scene-y"}) == VerticalRelation::Unknown, "Unrelated height frames mixed");
    check(compare_height(HeightBand{0, 200, "world-y-v1"}, HeightBand{0, 200, "world-y-v1"}) == VerticalRelation::Unknown, "Overlapping uncertain heights called the same level");
    rejects([&] { (void)compare_height(p, HeightBand{2, 1, "world-y-v1"}); });
    rejects([&] { (void)compare_height(p, HeightBand{0, std::numeric_limits<double>::quiet_NaN(), "world-y-v1"}); });
}
void detours_and_costs() {
    // The direct connection crosses a blocked rock. The geometric short climb
    // takes longer than the safe walking detour. Unknown passages stay excluded.
    std::vector<RouteNode> nodes{{0, 0, 0, "world", "surface"}, {0, 0, 5, "world", "surface"},
        {10, 0, 5, "world", "surface"}, {10, 0, 0, "world", "surface"}, {5, 3, 0, "world", "surface"}};
    std::vector<RouteEdge> edges{{0, 3, Traversal::Walk, PassageState::Closed, 1},
        {0, 1, Traversal::Walk, PassageState::Open, 2}, {1, 2, Traversal::Walk, PassageState::Open, 2},
        {2, 3, Traversal::Walk, PassageState::Open, 2}, {0, 4, Traversal::Climb, PassageState::Open, 8},
        {4, 3, Traversal::Glide, PassageState::Open, 1}, {0, 3, Traversal::Walk, PassageState::Unknown, .1}};
    RoutePlanner planner(nodes, edges); const auto result = planner.fastest(0, 3);
    check(result && result->steps.size() == 3 && result->steps[0].to == 1, "Blocked path or slow short climb selected");
    check(result->estimated_seconds == 6 && result->locomotion_metres == 20, "ETA/path distance is a straight-line substitute");
    check(!planner.fastest(3, 0), "One-way passage silently reversed");
    const auto at_target = planner.fastest(0, 0); check(at_target && at_target->steps.empty() && at_target->estimated_seconds == 0, "Already-at-target route incorrect");
    edges[1].seconds.reset(); RoutePlanner unknown_cost(nodes, edges); const auto alternative = unknown_cost.fastest(0, 3);
    check(alternative && alternative->estimated_seconds == 9, "Unknown traversal time invented");
    std::stop_source cancel; cancel.request_stop(); check(!planner.fastest(0, 3, {}, cancel.get_token()), "Cancelled search returned guidance");
}
void floors_and_teleports() {
    // A sealed cave has the same X/Z as the player. It needs an actual passage.
    std::vector<RouteNode> nodes{{0, 0, 0, "world", "surface"}, {0, -20, 0, "world", "cave"},
        {1000, -20, 0, "world", "cave"}, {1001, -20, 0, "world", "cave"}, {0, 0, 0, "dungeon", "entrance"}};
    std::vector<RouteEdge> edges{{0, 1, Traversal::Stairs, PassageState::Closed, 5},
        {2, 3, Traversal::Walk, PassageState::Open, 1}, {4, 0, Traversal::Portal, PassageState::Open, 4}};
    RoutePlanner planner(nodes, edges, {{"known-waypoint", 2, 2}});
    check(!planner.fastest(0, 1), "Same-X/Z cave acquired without a passage");
    check(!planner.fastest(0, 3), "Unlock or teleport permission guessed");
    const std::array<std::string, 1> unlocked{"known-waypoint"}, world{"world"};
    const auto result = planner.fastest(0, 3, {unlocked, world});
    check(result && result->steps.size() == 2 && result->steps[0].mode == Traversal::Teleport, "Useful verified teleport ignored");
    check(result->estimated_seconds == 3 && result->locomotion_metres == 1, "Teleport world separation counted as walking");
    const auto portal = planner.fastest(4, 3, {unlocked, world});
    check(portal && portal->steps[0].mode == Traversal::Portal && portal->estimated_seconds == 7, "Teleport used before leaving a restricted scene");
    rejects([&] { RoutePlanner invalid(nodes, {{0, 1, Traversal::Walk, PassageState::Open, 2}}); });
    rejects([&] { RoutePlanner invalid(nodes, {{4, 0, Traversal::Walk, PassageState::Open, 2}}); });
    rejects([&] { (void)planner.fastest(100, 0); });
    rejects([&] { RoutePlanner invalid(nodes, {{0, 2, Traversal::Walk, PassageState::Open, -1}}); });
    rejects([&] { RoutePlanner invalid(nodes, {}, {{"duplicate", 2, 1}, {"duplicate", 3, 1}}); });
    nodes[0].y = std::numeric_limits<double>::infinity(); rejects([&] { RoutePlanner invalid(nodes, {}); });
}

void collision_surfaces() {
    std::vector<CollisionFace> faces;
    const auto ground = [&](double x0, double x1, double y) {
        const SurfacePoint a{x0, y, 0}, b{x0, y, 8}, c{x1, y, 0}, d{x1, y, 8};
        faces.push_back({{a, b, c}}); faces.push_back({{c, b, d}});
    };
    ground(0, 8, 0); ground(0, 8, 4);
    const SurfacePoint a{4, 0, 0}, b{4, 3, 0}, c{4, 0, 6}, d{4, 3, 6};
    faces.push_back({{a, b, c}, false}); faces.push_back({{c, b, d}, false});
    WalkBakeSettings settings; settings.max_x = 8; settings.max_z = 8;
    settings.scene = "verified-test-scene"; settings.floor = "local-collision-surfaces";
    const auto node = [](const WalkSurface& graph, double x, double y, double z) {
        for (std::uint32_t i = 0; i < graph.nodes.size(); ++i) {
            const auto& n = graph.nodes[i];
            if (n.x == x && n.y == y && n.z == z) { return i; }
        }
        throw std::runtime_error("Missing standing position");
    };
    const auto graph = bake_walk_surface(faces, settings);
    const RoutePlanner planner(graph.nodes, graph.passages);
    const auto from = node(graph, 2.5, 0, 3.5), to = node(graph, 6.5, 0, 3.5);
    const auto route = planner.fastest(from, to);
    check(route && route->locomotion_metres > 4, "Route crossed a solid wall");
    check(!planner.fastest(from, node(graph, 2.5, 4, 3.5)), "Overlapping floors acquired an implicit vertical connection");
    faces.clear(); ground(0, 3.7, 0); ground(3.8, 8, 0);
    const auto gap = bake_walk_surface(faces, settings);
    const RoutePlanner gap_planner(gap.nodes, gap.passages);
    check(!gap_planner.fastest(node(gap, 2.5, 0, 3.5), node(gap, 6.5, 0, 3.5)), "Walk edge crossed a gap between its endpoint samples");
}
}
int main() {
    try { heights(); detours_and_costs(); floors_and_teleports(); collision_surfaces(); std::cout << "Verified-passage detours, 3D floors, ETA, teleport constraints, height uncertainty and collision surfaces passed.\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
