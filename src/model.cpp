#include "teyvat/model.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace teyvat {
namespace {

constexpr std::array<std::pair<CollectionStatus, std::string_view>, 5> statuses{{
    {CollectionStatus::NotCollected, "not_collected"},
    {CollectionStatus::PossiblyCollected, "possibly_collected"},
    {CollectionStatus::Collected, "collected"},
    {CollectionStatus::ManuallyConfirmed, "manually_confirmed"},
    {CollectionStatus::Unknown, "unknown"}
}};

bool blank(std::string_view value) noexcept {
    return value.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos;
}

} // namespace

std::string_view status_name(CollectionStatus status) {
    for (const auto& [value, name] : statuses) {
        if (value == status) {
            return name;
        }
    }
    throw std::invalid_argument("Invalid collection status");
}

std::optional<CollectionStatus> parse_status(std::string_view name) noexcept {
    for (const auto& [value, stored_name] : statuses) {
        if (name == stored_name) {
            return value;
        }
    }
    return std::nullopt;
}

bool is_confirmed(CollectionStatus status) noexcept {
    return status == CollectionStatus::Collected || status == CollectionStatus::ManuallyConfirmed;
}

std::optional<std::string_view> validation_error(const MapPoint& point) noexcept {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return "Coordinates must be finite";
    }
    if (blank(point.coordinate_space)) {
        return "Coordinate space is required";
    }
    if (blank(point.region)) {
        return "Region is required";
    }
    if (blank(point.layer)) {
        return "Map layer is required";
    }
    return std::nullopt;
}

std::optional<std::string_view> validation_error(const Marker& marker) noexcept {
    if (blank(marker.key.source)) {
        return "Marker source is required";
    }
    if (blank(marker.key.id)) {
        return "Stable marker ID is required";
    }
    if (blank(marker.category)) {
        return "Marker category is required";
    }
    return validation_error(marker.position);
}

std::optional<double> approximate_map_distance(const MapPoint& from, const MapPoint& to) noexcept {
    if (validation_error(from) || validation_error(to)
        || from.coordinate_space != to.coordinate_space
        || from.region != to.region || from.layer != to.layer) {
        return std::nullopt;
    }
    const double distance = std::hypot(to.x - from.x, to.y - from.y);
    if (!std::isfinite(distance)) {
        return std::nullopt;
    }
    return distance;
}

} // namespace teyvat
