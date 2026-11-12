#include "teyvat/navigation.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
template<class F> void rejects(F f) { bool failed = false; try { f(); } catch (const std::invalid_argument&) { failed = true; } check(failed, "Unsafe calibration accepted"); }
teyvat::NavigationChest chest(const char* id, double x, double y, const char* floor = "surface") {
    return {{{"test", id}, "common", {x, y, "reference-v1", "region", floor}}, teyvat::CollectionStatus::NotCollected, false};
}
teyvat::NavigationPose pose(double x = 0, double y = 0) { return {{x, y, "reference-v1", "region", "surface"}, .99, .1, 0, 1, 1}; }
void calibration() {
    using teyvat::ControlPoint;
    const auto transform = [](teyvat::XY p) { return teyvat::XY{100 + 2 * p.x - .5 * p.y, 200 + .25 * p.x + 3 * p.y}; };
    std::array<ControlPoint, 4> points;
    int i = 0;
    for (auto p : {teyvat::XY{-30, -20}, {30, -20}, {-30, 20}, {30, 20}}) { points[static_cast<std::size_t>(i++)] = {p, transform(p)}; }
    std::array<ControlPoint, 2> checks{{{{-5, 2}, transform({-5, 2})}, {{7, -4}, transform({7, -4})}}};
    const auto calibrated = teyvat::calibrate_coordinates(points, checks, .01);
    const auto q = calibrated.map({3, -5}); const auto expected = transform({3, -5});
    check(std::hypot(q.x - expected.x, q.y - expected.y) < 1e-9, "Affine transform loses rotation/shear/translation");
    check(calibrated.fit_rms < 1e-9 && calibrated.check_max_error < 1e-9, "Control error incorrect");
    auto bad = checks; bad[0].destination.x += 10; rejects([&] { (void)teyvat::calibrate_coordinates(points, bad, 1); });
    bad = checks; bad[0] = points[0]; rejects([&] { (void)teyvat::calibrate_coordinates(points, bad, 1); });
    rejects([&] { (void)teyvat::calibrate_coordinates(points, {}, 1); });
    rejects([&] { (void)teyvat::calibrate_coordinates(std::span(points).first(2), checks, 1); });
    rejects([&] { (void)teyvat::calibrate_coordinates(points, checks, std::numeric_limits<double>::quiet_NaN()); });
    auto line = points; for (auto& p : line) { p.source.y = p.source.x; }
    rejects([&] { (void)teyvat::calibrate_coordinates(line, checks, 1); });
    auto flat = points; for (auto& p : flat) { p.destination = {0, 0}; }
    rejects([&] { (void)teyvat::calibrate_coordinates(flat, checks, 1); });
}
void targets() {
    teyvat::TargetSelector selector;
    std::array chests{chest("a", 10, 0), chest("b", 20, 0), chest("underground", 0, 0, "cave")};
    auto p = pose(); auto g = selector.update(p, chests); check(g && g->target.id == "a", "Wrong initial target or floor");
    g = selector.update(pose(19, 0), chests); check(g && g->target.id == "a", "Target jumps with proximity jitter");
    check(!selector.update({}, chests), "Lost position produces guidance"); check(selector.target()->id == "a", "Lost position destroys lock");
    p.confidence = .7; check(!selector.update(p, chests), "Weak position produces guidance");
    p = pose(); p.position.coordinate_space = "incompatible"; check(!selector.update(p, chests), "Coordinate spaces mixed");
    p = pose(); g = selector.update(p, chests); check(g && g->target.id == "a", "Valid position failed to relock");
    chests[0].status = teyvat::CollectionStatus::Collected;
    g = selector.update(p, chests); check(g && g->target.id == "b", "Collected target did not advance");
    selector.skip(); check(!selector.update(p, chests), "Skipped target immediately returns");
    teyvat::TargetSelector restored; restored.restore(selector.target(), selector.skipped()); check(!restored.update(p, chests), "Restored skip was ignored");
    restored.reset(); g = restored.update(p, chests); check(g && g->target.id == "b", "Reset did not release skip");
    chests[1].retired = true; check(!restored.update(p, chests), "Archived target remains active");
    selector.reset(); chests[0].status = teyvat::CollectionStatus::NotCollected; chests[0].marker.key.source = "other";
    chests[1] = chest("a", 10, 0); g = selector.update(p, chests);
    check(g && g->target.source == "other", "Equal-distance source-scoped tie is not deterministic");
}
void bearings() {
    teyvat::TargetSelector selector; auto p = pose(); std::array chests{chest("east", 10, 0)};
    auto g = selector.update(p, chests); check(g && g->minimap_bearing && std::abs(*g->minimap_bearing - 90) < 1e-9, "East bearing incorrect");
    p.minimap_rotation = 90; g = selector.update(p, chests); check(g && std::abs(*g->minimap_bearing) < 1e-9, "Minimap rotation confused with character heading");
    for (double angle : {17, 43, 97, 179, -179, 227, 311}) {
        p.minimap_rotation = angle; g = selector.update(p, chests);
        check(g && g->minimap_bearing && std::abs(std::remainder(*g->minimap_bearing - (90 - angle), 360.0)) < 1e-9, "Continuous minimap rotation gives wrong target direction");
    }
    p.minimap_rotation.reset(); g = selector.update(p, chests); check(g && !g->minimap_bearing, "Unknown orientation invented an arrow");
    p = pose(10, 0); g = selector.update(p, chests); check(g && !g->minimap_bearing, "Uncertainty circle produced false bearing at target");
    p = pose(); p.units_per_pixel_y = 2; chests[0] = chest("diagonal", 10, -20);
    g = selector.update(p, chests); check(g && std::abs(*g->minimap_bearing - 45) < 1e-9, "Anisotropic map scale ignored");
    p.uncertainty = -1; check(!selector.update(p, chests), "Negative uncertainty accepted");
}
}
int main() {
    try { calibration(); targets(); bearings(); std::cout << "Calibration checks, target locking, skip/advance, contexts and minimap bearings passed.\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
