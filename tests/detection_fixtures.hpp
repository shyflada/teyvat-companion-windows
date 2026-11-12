#pragma once
#include "teyvat/map_data.hpp"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <stdexcept>

namespace detection_fixtures {
inline teyvat::Dataset dataset() {
    teyvat::MapLayer layer{{QStringLiteral("tc-offline-demo"), QStringLiteral("test-grid"), QStringLiteral("surface")},
        QStringLiteral("Original offline automation example"), QStringLiteral("synthetic-2026-v1"), 500, 300,
        QStringLiteral("Original synthetic QA artwork and coordinates; not game data"), QStringLiteral("MIT")};
    return {{layer}, {{layer.key, QStringLiteral("demo-chest-a"), QStringLiteral("common"), QStringLiteral("Example chest A"), {}, 100, 120},
        {layer.key, QStringLiteral("demo-chest-b"), QStringLiteral("precious"), QStringLiteral("Example chest B"), {}, 350, 150}}};
}
inline void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) { throw std::runtime_error("Cannot write test fixture"); }
}
inline QImage artwork(int state) {
    QImage image(64, 64, QImage::Format_ARGB32); image.fill(QColor("#142430"));
    QPainter p(&image); p.setPen(QPen(QColor("#e5bc55"), 3));
    if (state == 0) {
        p.setBrush(QColor("#a26332")); p.drawRect(5, 30, 54, 29);
        p.setBrush(QColor("#c58246")); p.drawRoundedRect(5, 10, 54, 26, 5, 5);
        p.drawLine(16, 11, 16, 58); p.drawLine(48, 11, 48, 58);
        p.setBrush(QColor("#fff1a5")); p.drawRect(28, 28, 8, 12);
    } else if (state == 1) {
        p.setBrush(QColor("#c58246")); p.drawRect(5, 2, 54, 11);
        p.setBrush(QColor("#0a101c")); p.drawRect(5, 23, 54, 20);
        p.setBrush(QColor("#a26332")); p.drawRect(5, 42, 54, 17);
        p.drawLine(16, 43, 16, 58); p.drawLine(48, 43, 48, 58);
        p.setBrush(QColor("#fff1a5")); p.drawRect(28, 43, 8, 8);
    } else if (state == 2) {
        p.fillRect(3, 3, 58, 58, QColor("#385b9c"));
        p.setBrush(QColor("#efce69")); p.drawEllipse(17, 16, 30, 30);
        p.drawLine(13, 54, 51, 54);
    }
    p.end();
    if (state == 4) { image = artwork(1); QPainter q(&image); q.fillRect(0, 0, 40, 64, Qt::gray); }
    if (state == 5) { image = artwork(1).transformed(QTransform().rotate(180)); }
    if (state == 6) { image = artwork(1).scaled(32, 32).scaled(64, 64); }
    return image;
}
inline QImage noisy(QImage image, int seed, int amplitude) {
    quint32 random = static_cast<quint32>(seed + 1);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            random = random * 1664525U + 1013904223U;
            const int noise = static_cast<int>((random >> 16) % static_cast<quint32>(2 * amplitude + 1)) - amplitude;
            row[x] = qRgb(qBound(0, qRed(row[x]) + noise, 255), qBound(0, qGreen(row[x]) + noise, 255), qBound(0, qBlue(row[x]) + noise, 255));
        }
    }
    return image;
}
inline QJsonObject make_bundle(const QString& dir, const QVector<int>& states = {0, 0, 1, 1, 1}, int seed = 0, int noise = 0, bool two_targets = false) {
    if (!QDir().mkpath(dir)) { throw std::runtime_error("Cannot create fixture directory"); }
    for (int state = 0; state < 2; ++state) {
        if (!artwork(state).save(QDir(dir).filePath(state == 0 ? QStringLiteral("closed.png") : QStringLiteral("opened.png")), "PNG")) { throw std::runtime_error("Cannot write reference PNG"); }
    }
    QJsonArray frames;
    for (int i = 0; i < states.size(); ++i) {
        QImage frame(128, 96, QImage::Format_ARGB32); frame.fill(QColor("#101c28"));
        { QPainter p(&frame); p.drawImage(32, 16, noisy(artwork(states[i]), seed + i, noise)); }
        const auto name = QStringLiteral("frame-%1.png").arg(i, 3, 10, QLatin1Char('0'));
        if (!frame.save(QDir(dir).filePath(name), "PNG")) { throw std::runtime_error("Cannot write frame PNG"); }
        const bool second = two_targets && i >= 5;
        frames.append(QJsonObject{{QStringLiteral("image"), name}, {QStringLiteral("time_ms"), i * 100},
            {QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), second ? 350 : 100}, {QStringLiteral("y"), second ? 150 : 120},
                {QStringLiteral("uncertainty"), 1}, {QStringLiteral("confidence"), 0.99}}}});
    }
    const auto layer = dataset().maps.first();
    return {{QStringLiteral("format"), QStringLiteral("teyvat-offline-recording")}, {QStringLiteral("version"), 1},
        {QStringLiteral("provenance"), QStringLiteral("Original synthetic fixture; no game capture or assets")}, {QStringLiteral("license"), QStringLiteral("MIT")},
        {QStringLiteral("map"), QJsonObject{{QStringLiteral("source"), layer.key.source}, {QStringLiteral("region"), layer.key.region},
            {QStringLiteral("layer"), layer.key.layer}, {QStringLiteral("frame"), layer.frame}}},
        {QStringLiteral("match_radius"), 12}, {QStringLiteral("roi"), QJsonObject{{QStringLiteral("x"), 32}, {QStringLiteral("y"), 16}, {QStringLiteral("width"), 64}, {QStringLiteral("height"), 64}}},
        {QStringLiteral("closed_template"), QStringLiteral("closed.png")}, {QStringLiteral("opened_template"), QStringLiteral("opened.png")},
        {QStringLiteral("frames"), frames}};
}
inline QString save_bundle(const QString& dir, const QJsonObject& root) {
    const auto path = QDir(dir).filePath(QStringLiteral("recording.json"));
    write(path, QJsonDocument(root).toJson()); return path;
}
inline QString bundle(const QString& dir) { return save_bundle(dir, make_bundle(dir)); }
} // namespace detection_fixtures
