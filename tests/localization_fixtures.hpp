#pragma once
#include "detection_fixtures.hpp"
#include "teyvat/localization.hpp"
#include <QPainterPath>
#include <cmath>

namespace localization_fixtures {
inline teyvat::Dataset dataset() {
    auto data = detection_fixtures::dataset();
    auto& layer = data.maps.first();
    layer.key.source = QStringLiteral("tc-localization-demo"); layer.frame = QStringLiteral("synthetic-localization-v1");
    layer.title = QStringLiteral("Minimap localization example"); layer.width = 320; layer.height = 240;
    for (auto& marker : data.markers) { marker.map = layer.key; }
    data.markers[1].x = 230; data.markers[1].y = 140;
    return data;
}
inline double lattice(int x, int y, int seed) {
    quint32 n = static_cast<quint32>(x) * 374761393U + static_cast<quint32>(y) * 668265263U + static_cast<quint32>(seed) * 1274126177U;
    n = (n ^ (n >> 13)) * 1274126177U;
    return static_cast<double>((n ^ (n >> 16)) & 65535U) / 65535.0;
}
inline double terrain(double x, double y, int seed, double step) {
    const int ix = static_cast<int>(x / step), iy = static_cast<int>(y / step);
    const double fx = x / step - ix, fy = y / step - iy;
    return (1 - fy) * ((1 - fx) * lattice(ix, iy, seed) + fx * lattice(ix + 1, iy, seed))
        + fy * ((1 - fx) * lattice(ix, iy + 1, seed) + fx * lattice(ix + 1, iy + 1, seed));
}
inline QImage atlas(int seed = 800) {
    QImage image(320, 240, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int level = qBound(30, static_cast<int>(40 + 115 * terrain(x, y, seed, 6) + 45 * terrain(x, y, seed + 1, 21)), 220);
            row[x] = qRgb(level * 3 / 4, level, level * 4 / 5);
        }
    }
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#387aab"), 7));
    QPainterPath river; river.moveTo(0, 10); river.cubicTo(160, 20, 90, 210, 320, 180); p.drawPath(river);
    p.setPen(QPen(QColor("#cfb98a"), 2)); p.drawLine(10, 210, 310, 30);
    return image;
}
inline QImage minimap(const QImage& reference, QPointF center, double angle = 0, double scale = 1, int noise = 0) {
    QImage image(65, 65, QImage::Format_ARGB32); image.fill(QColor("#142430"));
    { QPainter p(&image); p.setRenderHint(QPainter::SmoothPixmapTransform);
      p.translate(32.5, 32.5); p.rotate(-angle); p.scale(1 / scale, 1 / scale);
      p.drawImage(QPointF(-center.x() - 0.5, -center.y() - 0.5), reference); }
    image = detection_fixtures::noisy(image, static_cast<int>(center.x() * 5 + angle), noise);
    { QPainter p(&image); p.setPen(QPen(QColor("#e2efff"), 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(1, 1, 62, 62);
      p.setPen(Qt::NoPen); p.setBrush(Qt::white); p.drawPolygon(QPolygonF{QPointF(32, 26), QPointF(28, 37), QPointF(36, 37)}); }
    return image;
}
inline teyvat::LocalizationConfig config(const QImage& reference) {
    teyvat::LocalizationConfig c; c.reference = reference; c.roi = QRect(0, 0, 65, 65);
    c.scales = {1, 1.5}; c.rotations = {0, 45, 90, 180}; return c;
}
inline QString bundle(const QString& dir) {
    auto root = detection_fixtures::make_bundle(dir, {0, 0, 1, 1, 1, 0, 0, 1, 1, 1}, 42, 3, true);
    const auto data = dataset(); const auto layer = data.maps.first();
    root[QStringLiteral("version")] = 2;
    root[QStringLiteral("map")] = QJsonObject{{QStringLiteral("source"), layer.key.source}, {QStringLiteral("region"), layer.key.region}, {QStringLiteral("layer"), layer.key.layer}, {QStringLiteral("frame"), layer.frame}};
    const auto reference = atlas();
    if (!reference.save(QDir(dir).filePath(QStringLiteral("reference.png")), "PNG")) { throw std::runtime_error("Cannot write synthetic atlas"); }
    root[QStringLiteral("localization")] = QJsonObject{{QStringLiteral("reference"), QStringLiteral("reference.png")},
        {QStringLiteral("provenance"), QStringLiteral("Original synthetic terrain, not a game map")}, {QStringLiteral("license"), QStringLiteral("MIT")},
        {QStringLiteral("roi"), QJsonObject{{QStringLiteral("x"), 144}, {QStringLiteral("y"), 24}, {QStringLiteral("width"), 65}, {QStringLiteral("height"), 65}}},
        {QStringLiteral("origin"), QJsonObject{{QStringLiteral("x"), 0}, {QStringLiteral("y"), 0}}},
        {QStringLiteral("units_per_pixel"), QJsonObject{{QStringLiteral("x"), 1}, {QStringLiteral("y"), 1}}},
        {QStringLiteral("scales"), QJsonArray{1, 1.5}}, {QStringLiteral("rotations"), QJsonArray{0, 45, 90, 180}}};
    auto frames = root[QStringLiteral("frames")].toArray(); QJsonArray labels;
    for (int i = 0; i < frames.size(); ++i) {
        auto entry = frames[i].toObject(); entry.remove(QStringLiteral("position")); frames[i] = entry;
        const QPointF center = i < 5 ? QPointF(100, 120) : QPointF(230, 140);
        QImage image(240, 128, QImage::Format_ARGB32); image.fill(QColor("#101c28"));
        { QPainter p(&image); p.drawImage(32, 16, detection_fixtures::noisy(detection_fixtures::artwork(i % 5 < 2 ? 0 : 1), 42 + i, 3));
          p.drawImage(144, 24, minimap(reference, center, i < 5 ? 0 : 45, i < 5 ? 1 : 1.5, 1)); }
        if (!image.save(QDir(dir).filePath(entry[QStringLiteral("image")].toString()), "PNG")) { throw std::runtime_error("Cannot save localized frame"); }
        labels.append(QJsonObject{{QStringLiteral("time_ms"), i * 100}, {QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), center.x()}, {QStringLiteral("y"), center.y()}}}});
    }
    root[QStringLiteral("frames")] = frames;
    detection_fixtures::write(QDir(dir).filePath(QStringLiteral("map.json")), teyvat::encode_json(data));
    const QJsonObject truth{{QStringLiteral("format"), QStringLiteral("teyvat-evaluation-labels")}, {QStringLiteral("version"), 1},
        {QStringLiteral("position_tolerance"), 2}, {QStringLiteral("event_tolerance_ms"), 150},
        {QStringLiteral("positions"), labels}, {QStringLiteral("openings"), QJsonArray{
            QJsonObject{{QStringLiteral("time_ms"), 300}, {QStringLiteral("marker_id"), QStringLiteral("demo-chest-a")}},
            QJsonObject{{QStringLiteral("time_ms"), 800}, {QStringLiteral("marker_id"), QStringLiteral("demo-chest-b")}}}}};
    detection_fixtures::write(QDir(dir).filePath(QStringLiteral("labels.json")), QJsonDocument(truth).toJson());
    return detection_fixtures::save_bundle(dir, root);
}
} // namespace localization_fixtures
