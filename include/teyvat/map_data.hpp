#pragma once

#include "teyvat/model.hpp"
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>

namespace teyvat {

struct LayerKey {
    QString source, region, layer;
    bool operator==(const LayerKey&) const = default;
};

struct MapLayer {
    LayerKey key;
    QString title, frame;
    double width = 1000, height = 1000;
    QString provenance = QStringLiteral("User-entered"), license = QStringLiteral("User-owned");
    QString version = QStringLiteral("v1");
    bool operator==(const MapLayer&) const = default;
};

struct MapMarker {
    LayerKey map;
    QString id, category = QStringLiteral("common"), label, notes;
    double x = 0, y = 0;
    CollectionStatus status = CollectionStatus::NotCollected;
    bool retired = false;
    QString origin = QStringLiteral("manual");
    bool operator==(const MapMarker&) const = default;
};

struct Dataset {
    QVector<MapLayer> maps;
    QVector<MapMarker> markers;
    bool operator==(const Dataset&) const = default;
};

struct ImportReview {
    QByteArray token;
    int new_maps = 0, new_markers = 0, updated_markers = 0, progress_changes = 0;
};

struct HistoryEvent {
    qint64 id = 0;
    QString marker, source, marker_id, before, after, origin, time, command;
};

inline constexpr qsizetype max_transfer_bytes = 8 * 1024 * 1024;
inline constexpr int max_markers = 20000;
inline constexpr int max_layer_markers = 5000;
inline constexpr int max_map_layers = 1024;

[[nodiscard]] QString layer_identity(const LayerKey& key);
[[nodiscard]] QString marker_identity(const MapMarker& marker);
[[nodiscard]] QString stored_status(CollectionStatus status);
[[nodiscard]] CollectionStatus read_status(const QString& status);
void validate_dataset(const Dataset& dataset);
[[nodiscard]] QByteArray encode_json(const Dataset& dataset);
[[nodiscard]] Dataset decode_json(const QByteArray& bytes);
[[nodiscard]] QByteArray encode_csv(const Dataset& dataset);
[[nodiscard]] Dataset decode_csv(const QByteArray& bytes);
[[nodiscard]] QImage decode_background(const QByteArray& png);
void atomic_export(const QString& path, const QByteArray& data);

} // namespace teyvat
