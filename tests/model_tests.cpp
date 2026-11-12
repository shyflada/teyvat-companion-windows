#include "teyvat/model.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

teyvat::MapPoint point(double x = 0.0, double y = 0.0) {
    return {x, y, "test:cartesian-v1", "test-region", "surface"};
}

void statuses() {
    using teyvat::CollectionStatus;
    constexpr std::array cases{
        std::pair{CollectionStatus::NotCollected, std::string_view{"not_collected"}},
        std::pair{CollectionStatus::PossiblyCollected, std::string_view{"possibly_collected"}},
        std::pair{CollectionStatus::Collected, std::string_view{"collected"}},
        std::pair{CollectionStatus::ManuallyConfirmed, std::string_view{"manually_confirmed"}},
        std::pair{CollectionStatus::Unknown, std::string_view{"unknown"}}
    };
    for (const auto& [status, name] : cases) {
        check(teyvat::status_name(status) == name, "Status storage name changed");
        check(teyvat::parse_status(name) == status, "Status cannot be restored");
        const bool confirmed = name == "collected" || name == "manually_confirmed";
        check(teyvat::is_confirmed(status) == confirmed, "Uncertain evidence counted as collected");
    }
    for (const std::string_view invalid : {"", "Collected", " collected", "collected ", "invalid"}) {
        check(!teyvat::parse_status(invalid), "Invalid storage status accepted");
    }
    bool rejected = false;
    try {
        (void)teyvat::status_name(static_cast<CollectionStatus>(99));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "Invalid enum silently converted to a valid status");
}

void markers() {
    const teyvat::Marker valid{{"test-source", "stable-42"}, "chest", point()};
    check(!teyvat::validation_error(valid), "Valid marker rejected");
    check(valid.key == teyvat::MarkerKey{"test-source", "stable-42"}, "Stable identity changed");
    check(valid.key != teyvat::MarkerKey{"other-source", "stable-42"}, "Provider namespaces collide");
    check(valid.key != teyvat::MarkerKey{"test-source", "stable-43"}, "Different IDs collide");
    for (const std::string_view missing : {"", " \t\r\n"}) {
        auto marker = valid;
        marker.key.source = missing;
        check(teyvat::validation_error(marker).has_value(), "Missing source accepted");
        marker = valid;
        marker.key.id = missing;
        check(teyvat::validation_error(marker).has_value(), "Missing ID accepted");
        marker = valid;
        marker.category = missing;
        check(teyvat::validation_error(marker).has_value(), "Missing category accepted");
    }
    auto marker = valid;
    marker.position.x = std::numeric_limits<double>::quiet_NaN();
    check(teyvat::validation_error(marker).has_value(), "Invalid marker coordinates accepted");
}

void coordinates() {
    const auto origin = point();
    const auto target = point(3.0, 4.0);
    const auto distance = teyvat::approximate_map_distance(origin, target);
    check(distance && std::abs(*distance - 5.0) < 1e-12, "Incorrect map separation");
    check(teyvat::approximate_map_distance(origin, origin) == 0.0, "Origin has nonzero distance");
    check(teyvat::approximate_map_distance(target, origin) == distance, "Distance is asymmetric");
    check(teyvat::approximate_map_distance(point(-3.0, -4.0), origin) == distance,
          "Valid negative coordinates rejected");

    auto other = target;
    other.coordinate_space = "test:different-scale";
    check(!teyvat::approximate_map_distance(origin, other), "Incompatible scales compared");
    other = target;
    other.region = "other-region";
    check(!teyvat::approximate_map_distance(origin, other), "Different regions compared");
    other = target;
    other.layer = "underground";
    check(!teyvat::approximate_map_distance(origin, other), "Underground treated as surface");

    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()}) {
        other = target;
        other.x = invalid;
        check(teyvat::validation_error(other).has_value(), "Nonfinite X accepted");
        check(!teyvat::approximate_map_distance(origin, other), "Invalid destination has distance");
        check(!teyvat::approximate_map_distance(other, origin), "Invalid origin has distance");
        other = target;
        other.y = invalid;
        check(teyvat::validation_error(other).has_value(), "Nonfinite Y accepted");
    }
    for (const std::string_view missing : {"", " \t\n"}) {
        other = target;
        other.coordinate_space = missing;
        check(teyvat::validation_error(other).has_value(), "Missing coordinate space accepted");
        check(!teyvat::approximate_map_distance(other, other), "Empty coordinate spaces compared");
        other = target;
        other.region = missing;
        check(teyvat::validation_error(other).has_value(), "Missing region accepted");
        other = target;
        other.layer = missing;
        check(teyvat::validation_error(other).has_value(), "Missing map layer accepted");
    }
    const double largest = std::numeric_limits<double>::max();
    check(!teyvat::approximate_map_distance(point(largest, 0), point(-largest, 0)),
          "Overflow presented as a usable distance");
    const auto large_distance = teyvat::approximate_map_distance(origin, point(1e200, 1e200));
    check(large_distance && std::isfinite(*large_distance), "Avoidable intermediate overflow");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Expected one suite: statuses, markers or coordinates\n";
        return 2;
    }
    const std::string_view suite = argv[1];
    try {
        if (suite == "statuses") {
            statuses();
        } else if (suite == "markers") {
            markers();
        } else if (suite == "coordinates") {
            coordinates();
        } else {
            std::cerr << "Unknown suite\n";
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << suite << ": " << error.what() << '\n';
        return 1;
    }
    std::cout << suite << ": passed\n";
    return 0;
}
