#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace teyvat {

enum class CollectionStatus {
    NotCollected,
    PossiblyCollected,
    Collected,
    ManuallyConfirmed,
    Unknown
};

// Stable storage names, independent of the language used by the UI.
[[nodiscard]] std::string_view status_name(CollectionStatus status);
[[nodiscard]] std::optional<CollectionStatus> parse_status(std::string_view name) noexcept;
[[nodiscard]] bool is_confirmed(CollectionStatus status) noexcept;

struct MarkerKey {
    std::string source;
    std::string id;

    bool operator==(const MarkerKey&) const = default;
};

struct MapPoint {
    double x{};
    double y{};
    std::string coordinate_space;
    std::string region;
    std::string layer;
};

struct Marker {
    MarkerKey key;
    std::string category;
    MapPoint position;
};

// A missing error means that the model's structural invariants hold.
// Data provenance, map bounds and licences must also be checked by importers.
[[nodiscard]] std::optional<std::string_view> validation_error(const MapPoint& point) noexcept;
[[nodiscard]] std::optional<std::string_view> validation_error(const Marker& marker) noexcept;

// Straight-line separation in provider-defined map units, never a walkable path
// or a distance in metres. Incompatible or invalid points have no distance.
[[nodiscard]] std::optional<double> approximate_map_distance(
    const MapPoint& from, const MapPoint& to) noexcept;

} // namespace teyvat
