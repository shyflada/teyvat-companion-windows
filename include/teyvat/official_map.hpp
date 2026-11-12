#pragma once
#include "teyvat/map_data.hpp"
#include <QPointF>
#include <QSize>
#include <QUrl>
#include <QRect>
#include <QHash>
#include <QSet>
#include <memory>
#include <optional>
#include <stop_token>
#include "teyvat/localization.hpp"

namespace teyvat {
struct TrackingSetup;
// Called after a failed local match. A resumed HUD gets one immediate verified
// global attempt; ordinary misses remain throttled and ambiguous planes abstain.
[[nodiscard]] bool global_recovery_due(bool anchored, int misses, qint64 since_attempt,
    bool resumed, bool ambiguous_plane);
// HoYoLAB's raster origin may move between revisions. Stored coordinates do not.
inline constexpr double official_coordinate_offset = 500000;
struct OfficialWaypoint {
    QString id, name, layer;
    QPointF position;
    int area = 0;
};
struct OfficialFloor {
    QString layer, title;
    QUrl overlay;
    QRectF bounds;
    std::optional<QPointF> entrance;
};
struct OfficialMap {
    int id = 2;
    QString name, tile_version, revision, directory;
    QSize raster_size;
    QPointF raster_origin;
    Dataset dataset;
    QHash<QString, int> marker_areas;
    QVector<OfficialWaypoint> waypoints;
    QVector<OfficialFloor> floors;
    [[nodiscard]] LayerKey surface() const;
    [[nodiscard]] QPointF world(QPointF raster, int zoom = 0) const;
    [[nodiscard]] QPointF raster(QPointF world, int zoom = 0) const;
};

// These anonymous GETs use the public map page's first-party resources. They
// never read a browser session, send captured pixels, or update account progress.
[[nodiscard]] OfficialMap parse_official_map(int map_id, const QByteArray& info,
    const QByteArray& labels, const QByteArray& points, const QByteArray& groups);
[[nodiscard]] OfficialMap refresh_official_map(const QString& cache_root, int map_id,
    std::stop_token stop = {});
[[nodiscard]] OfficialMap load_official_map(const QString& cache_root, int map_id);
void parse_official_waypoints(OfficialMap& map, const QByteArray& bytes);
// Conditional horizontal shortcuts only; unlock state and terrain cost are unknown.
[[nodiscard]] QVector<OfficialWaypoint> official_teleport_options(const OfficialMap& map,
    const QString& layer, QPointF player, QPointF target);
[[nodiscard]] QSet<QString> official_region_markers(const OfficialMap& map, int area);
[[nodiscard]] QRectF official_region_bounds(const OfficialMap& map, int area);
[[nodiscard]] TrackingSetup make_official_tracking_setup(const OfficialMap& map, const TrackingSetup& hud, const QString& layer = {});
[[nodiscard]] std::optional<LocalizationConfig> official_floor_reference(const OfficialMap& map, const QString& layer,
    QPointF position, const LocalizationConfig& hud, std::stop_token stop = {}, bool allow_download = true);
[[nodiscard]] QImage decode_official_tile(const QByteArray& webp);
[[nodiscard]] QImage official_tile(const OfficialMap& map, int x, int y, int zoom,
    std::stop_token stop = {});
[[nodiscard]] QImage official_region(const OfficialMap& map, QRect raster_pixels,
    int zoom = 0, std::stop_token stop = {});

// Build once per tile revision, acquire location from the game's world map,
// then match only a 1024-pixel local viewport during ordinary gameplay.
struct OfficialAcquisition {
    LocalizationResult estimate;
    LocalizationConfig reference;
    QString layer = QStringLiteral("surface");
};
struct OfficialPlaneSearch {
    std::optional<OfficialAcquisition> match;
    bool ambiguous = false;
};
// Cheap check before initializing the global feature index for a map frame.
[[nodiscard]] bool official_world_map_marker_visible(const QImage& image);
[[nodiscard]] OfficialPlaneSearch match_official_planes(const OfficialMap& map, const QImage& image,
    const LocalizationConfig& hud, QPointF hint, const QString& current_layer, bool world_map = false,
    std::stop_token stop = {}, bool allow_download = true);
[[nodiscard]] std::optional<OfficialAcquisition> verify_official_waypoint(const OfficialMap& map,
    const QImage& image, const LocalizationConfig& hud, const OfficialWaypoint& waypoint, std::stop_token stop = {});
class OfficialWorldLocator final {
public:
    explicit OfficialWorldLocator(OfficialMap map, std::stop_token stop = {});
    ~OfficialWorldLocator();
    [[nodiscard]] LocalizationResult locate(const QImage& world_map, std::stop_token stop = {}) const;
    // Coarse global features nominate areas only. A position is returned only
    // after an independent full-resolution local match, with ambiguity checks.
    [[nodiscard]] std::optional<OfficialAcquisition> locate_minimap(const QImage& image,
        const LocalizationConfig& hud, std::stop_token stop = {}, const QRectF& preferred_area = {}) const;
private:
    struct Data;
    std::unique_ptr<Data> data_;
};
[[nodiscard]] LocalizationConfig official_local_reference(const OfficialMap& map,
    QPointF position, const LocalizationConfig& hud, std::stop_token stop = {});
} // namespace teyvat
