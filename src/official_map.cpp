#include "teyvat/official_map.hpp"
#include "teyvat/detection.hpp"
#include <QCryptographicHash>
#include <QBuffer>
#include <QImageReader>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#else
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTimer>
#endif
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace teyvat {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
void cancelled(std::stop_token stop) { if (stop.stop_requested()) { fail("Official map operation cancelled."); } }
void check_id(int id) { if (id != 2 && id != 7 && id != 9 && id != 36) { fail("Unsupported official map ID."); } }
QJsonObject response(const QByteArray& bytes) {
    if (bytes.isEmpty() || bytes.size() > max_transfer_bytes) { fail("Missing or oversized official map response."); }
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) { fail("Invalid official map JSON."); }
    const auto root = doc.object();
    if (root.value("retcode") != QJsonValue(0) || !root.value("data").isObject()) { fail("Official map service returned an error."); }
    return root.value("data").toObject();
}
double number(QJsonValue value) {
    if (!value.isDouble() || !std::isfinite(value.toDouble())) { fail("Invalid official map coordinate."); }
    return value.toDouble();
}
int integer(QJsonValue value, int minimum = 0, int maximum = 1000000000) {
    const auto n = number(value);
    if (n < minimum || n > maximum || std::trunc(n) != n) { fail("Invalid official map integer."); }
    return static_cast<int>(n);
}
QPointF pair(QJsonValue value) {
    if (!value.isArray() || value.toArray().size() != 2) { fail("Invalid official raster geometry."); }
    const auto a = value.toArray(); return {number(a[0]), number(a[1])};
}
QString version(QJsonValue value) {
    const auto text = value.toString();
    if (!QRegularExpression(QStringLiteral("^[a-f0-9]{32}$")).match(text).hasMatch()) { fail("Invalid official tile revision."); }
    return text;
}
QString map_directory(const QString& root, int id) {
    check_id(id); return QDir(root).absoluteFilePath(QStringLiteral("map-%1").arg(id));
}
QByteArray read(const QString& path, qsizetype limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit || file.size() <= 0) { fail("Official map cache is missing or invalid."); }
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { fail("Cannot create the local official map cache."); }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { fail("Cannot save the local official map cache."); }
}
bool overlay_url(const QUrl& url);
#ifdef Q_OS_ANDROID
QByteArray get(const QUrl& url, qsizetype limit, std::stop_token stop) {
#else
QByteArray get(QNetworkAccessManager& manager, const QUrl& url, qsizetype limit, std::stop_token stop) {
#endif
    cancelled(stop);
    const bool api = url.host() == QStringLiteral("sg-public-api.hoyolab.com")
        || url.host() == QStringLiteral("sg-public-api-static.hoyolab.com");
    const bool tile = url.host() == QStringLiteral("act-webstatic.hoyoverse.com") || url.host() == QStringLiteral("act-upload.hoyoverse.com");
    if (url.scheme() != QStringLiteral("https") || (!api && !tile) || !url.userInfo().isEmpty()
        || url.port(-1) != -1 || url.hasFragment()
        || (api && !url.path().startsWith(QStringLiteral("/common/map_user/ys_obc/")))
        || (tile && !url.path().startsWith(QStringLiteral("/map_manage/map/")) && !overlay_url(url))) { fail("Untrusted map resource URL."); }
#ifdef Q_OS_ANDROID
    const auto address = QJniObject::fromString(url.toString(QUrl::FullyEncoded));
    const auto response = QJniObject::callStaticObjectMethod("app/teyvatcompanion/probe/OfficialMapHttp", "download",
        "(Ljava/lang/String;I)[B", address.object<jstring>(), static_cast<jint>(limit));
    cancelled(stop);
    if (!response.isValid()) { fail("Could not download a public HoYoLAB map resource."); }
    QJniEnvironment env;
    const auto array = response.object<jbyteArray>();
    const auto count = env->GetArrayLength(array);
    if (count < 1 || count > limit) { fail("Official map download exceeds the size limit or is empty."); }
    QByteArray bytes(count, Qt::Uninitialized);
    env->GetByteArrayRegion(array, 0, count, reinterpret_cast<jbyte*>(bytes.data()));
    return bytes;
#else
#ifdef Q_OS_WIN
    if (QSslSocket::activeBackend() != QStringLiteral("schannel") && !QSslSocket::setActiveBackend(QStringLiteral("schannel"))) { fail("Windows TLS backend is unavailable."); }
#endif
    QNetworkRequest request(url); request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setRawHeader("User-Agent", "TeyvatCompanion/0.1 (public map cache)");
    auto* reply = manager.get(request); QEventLoop loop; QTimer poll; poll.setInterval(100);
    QByteArray bytes; bool oversized = false;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (reply->bytesAvailable() > limit - bytes.size()) { oversized = true; reply->abort(); }
        else { bytes += reply->readAll(); }
    });
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (stop.stop_requested()) { reply->abort(); } });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    poll.start(); loop.exec(); cancelled(stop);
    if (oversized || reply->bytesAvailable() > limit - bytes.size()) { fail("Official map download exceeds the size limit."); }
    bytes += reply->readAll();
    if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        throw std::runtime_error((QStringLiteral("Official map request failed: ") + reply->errorString()).toStdString());
    }
    return bytes;
#endif
}
#ifndef Q_OS_ANDROID
QByteArray get(const QUrl& url, qsizetype limit, std::stop_token stop) {
    QNetworkAccessManager manager;
    return get(manager, url, limit, stop);
}
#endif
QUrl endpoint(const char* path, int id, const QString& extra = {}) {
    return QUrl(QStringLiteral("https://sg-public-api-static.hoyolab.com/common/map_user/ys_obc/")
        + QLatin1String(path) + QStringLiteral("?map_id=%1&app_sn=ys_obc&lang=en-us").arg(id) + extra);
}
QJsonObject navigation_metadata(const OfficialMap& map) {
    QJsonObject areas; for (auto i = map.marker_areas.cbegin(); i != map.marker_areas.cend(); ++i) { areas[i.key()] = i.value(); }
    QJsonArray floors, waypoints;
    for (const auto& f : map.floors) {
        QJsonObject item{{"layer", f.layer}, {"title", f.title}, {"url", f.overlay.toString()},
            {"bounds", QJsonArray{f.bounds.x(), f.bounds.y(), f.bounds.width(), f.bounds.height()}}};
        if (f.entrance) { item["entrance"] = QJsonArray{f.entrance->x(), f.entrance->y()}; } floors.append(item);
    }
    for (const auto& w : map.waypoints) { waypoints.append(QJsonObject{{"id", w.id}, {"name", w.name}, {"layer", w.layer},
        {"area", w.area}, {"position", QJsonArray{w.position.x(), w.position.y()}}}); }
    return {{"areas", areas}, {"floors", floors}, {"waypoints", waypoints}};
}
bool overlay_url(const QUrl& url) {
    return url.scheme() == QStringLiteral("https") && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()
        && (url.host() == QStringLiteral("act-upload.hoyoverse.com") || url.host() == QStringLiteral("act-webstatic.hoyoverse.com"))
        && QRegularExpression(QStringLiteral("^/map_manage/[A-Za-z0-9_/-]+\\.png$")).match(url.path()).hasMatch();
}
QByteArray snapshot(const OfficialMap& map) {
    const auto metadata = navigation_metadata(map);
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(metadata).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    return QJsonDocument(QJsonObject{{"format", "teyvat-official-cache"}, {"version", 1}, {"map_id", map.id},
        {"name", map.name}, {"tile_version", map.tile_version}, {"revision", map.revision},
        {"raster_size", QJsonArray{map.raster_size.width(), map.raster_size.height()}},
        {"raster_origin", QJsonArray{map.raster_origin.x(), map.raster_origin.y()}},
        {"dataset", QJsonDocument::fromJson(encode_json(map.dataset)).object()}, {"navigation", metadata}, {"navigation_hash", hash}}).toJson(QJsonDocument::Compact);
}
void validate_geometry(const OfficialMap& map) {
    if (map.raster_size.width() < 256 || map.raster_size.height() < 256 || map.raster_size.width() > 65536
        || map.raster_size.height() > 65536 || map.raster_size.width() % 256 || map.raster_size.height() % 256
        || !QRectF(QPointF(0, 0), QSizeF(map.raster_size)).contains(map.raster_origin)) { fail("Unsupported official raster geometry."); }
}
} // namespace

LayerKey OfficialMap::surface() const { return {QStringLiteral("hoyolab-map-%1").arg(id), QStringLiteral("world"), QStringLiteral("surface")}; }
QPointF OfficialMap::world(QPointF p, int zoom) const {
    return p * std::exp2(-zoom) - raster_origin + QPointF(official_coordinate_offset, official_coordinate_offset);
}
QPointF OfficialMap::raster(QPointF p, int zoom) const {
    return (p - QPointF(official_coordinate_offset, official_coordinate_offset) + raster_origin) * std::exp2(zoom);
}
OfficialMap parse_official_map(int id, const QByteArray& info_bytes, const QByteArray& label_bytes,
    const QByteArray& point_bytes, const QByteArray& group_bytes) {
    check_id(id); OfficialMap map; map.id = id;
    const auto info = response(info_bytes).value("info").toObject();
    if (integer(info.value("id"), 1) != id || !info.value("detail_v2").isObject()) { fail("Official map identity mismatch."); }
    map.name = info.value("name").toString(); const auto detail = info.value("detail_v2").toObject();
    map.tile_version = version(detail.value("map_version")); map.raster_origin = pair(detail.value("origin"));
    const auto size = pair(detail.value("total_size"));
    if (size.x() != std::trunc(size.x()) || size.y() != std::trunc(size.y())) { fail("Invalid official raster size."); }
    map.raster_size = QSize(integer(size.x(), 256, 65536), integer(size.y(), 256, 65536)); validate_geometry(map);
    if (integer(detail.value("min_zoom"), -8, 0) > -2 || integer(detail.value("max_zoom"), 0, 8) < 0) { fail("Unsupported official map zoom range."); }
    const QHash<int, QString> categories{{17, "common"}, {44, "exquisite"}, {45, "precious"}, {46, "luxurious"}, {269, "remarkable"}};
    QHash<int, int> expected, actual; QHash<int, QString> names;
    const auto tree = response(label_bytes).value("tree");
    if (!tree.isArray()) { fail("Missing official chest labels."); }
    for (const auto& root : tree.toArray()) { const auto parent = root.toObject();
        if (integer(parent.value("id"), 1) != 13) { continue; }
        for (const auto& v : parent.value("children").toArray()) { const auto label = v.toObject(); const int key = integer(label.value("id"), 1);
            if (!categories.contains(key) || expected.contains(key) || integer(label.value("parent_id"), 1) != 13) { fail("Official chest categories changed; the old cache is preserved."); }
            expected[key] = integer(label.value("point_count"), 0, max_layer_markers); names[key] = label.value("name").toString().simplified();
        }
    }
    if (expected.isEmpty()) { fail("Official chest labels are missing."); }
    QSet<QString> floor_keys; QHash<QString, QString> floor_names;
    const auto groups = response(group_bytes).value("list");
    if (!groups.isArray() || groups.toArray().size() > 1024) { fail("Invalid official floor catalogue."); }
    for (const auto& v : groups.toArray()) { const auto group = v.toObject();
        const int gid = integer(group.value("id"), 1);
        if (integer(group.value("map_id"), 1) != id) { fail("Official floor map mismatch."); }
        for (const auto& fv : group.value("floors").toArray()) { const auto floor = fv.toObject(); const int fid = integer(floor.value("id"));
            const auto key = QStringLiteral("group-%1-floor-%2").arg(gid).arg(fid);
            if (floor_keys.contains(key)) { fail("Duplicate official floor identity."); }
            floor_keys.insert(key); floor_names[key] = floor.value("floor_name").toString().left(100);
            const auto overlay = floor.value("overlay").toObject(); const QUrl url(overlay.value("url").toString());
            if (!overlay.isEmpty() && overlay_url(url)) {
                const double x = number(overlay.value("l_x")), y = number(overlay.value("l_y"));
                const double right = number(overlay.value("r_x")), bottom = number(overlay.value("r_y"));
                const QRectF bounds(x + official_coordinate_offset, y + official_coordinate_offset, right - x, bottom - y);
                if (!bounds.isValid() || !QRectF(0, 0, 1000000, 1000000).contains(bounds) || bounds.width() > 16384 || bounds.height() > 16384) { fail("Invalid official floor extent."); }
                OfficialFloor entry{key, floor_names[key], url, bounds, std::nullopt};
                if (group.value("underground_entrance_x_pos").isDouble() && group.value("underground_entrance_y_pos").isDouble()) {
                    const auto ex = number(group.value("underground_entrance_x_pos")), ey = number(group.value("underground_entrance_y_pos"));
                    if (ex != 0 && ey != 0 && QRectF(0, 0, 1000000, 1000000).contains({ex + official_coordinate_offset, ey + official_coordinate_offset})) { entry.entrance = QPointF(ex + official_coordinate_offset, ey + official_coordinate_offset); }
                }
                map.floors.append(entry);
            }
        }
    }
    QSet<QString> layers;
    const auto add_layer = [&](const QString& layer, const QString& title) {
        if (layers.contains(layer)) { return; } layers.insert(layer); auto key = map.surface(); key.layer = layer;
        map.dataset.maps.append({key, title, QStringLiteral("hoyolab-map-%1-xy-down-v1").arg(id), 1000000, 1000000,
            QStringLiteral("https://act.hoyolab.com/ys/app/interactive-map/index.html"),
            QStringLiteral("HoYoverse map content; personal local cache; redistribution permission not asserted"), QStringLiteral("pending")});
    };
    add_layer(QStringLiteral("surface"), map.name + QStringLiteral(" · surface"));
    if (map.floors.size() > 2048) { fail("Oversized official floor catalogue."); }
    for (const auto& floor : map.floors) { add_layer(floor.layer, floor.title); }
    const auto points = response(point_bytes).value("point_list");
    if (!points.isArray() || points.toArray().size() > max_layer_markers) { fail("Invalid official chest list."); }
    for (const auto& v : points.toArray()) { const auto p = v.toObject(); const int label = integer(p.value("label_id"), 1);
        if (!expected.contains(label)) { fail("Unexpected marker category in the official chest response."); } ++actual[label];
        const auto raw = QPointF(number(p.value("x_pos")), number(p.value("y_pos")));
        if (!QRectF(QPointF(0, 0), QSizeF(map.raster_size)).contains(raw + map.raster_origin)) { fail("Official chest lies outside its raster."); }
        QString layer = QStringLiteral("surface");
        if (!p.value("point_group").isNull()) {
            if (!p.value("point_group").isObject()) { fail("Invalid official chest floor."); }
            const auto group = p.value("point_group").toObject(); const int fid = integer(group.value("floor_id"));
            layer = QStringLiteral("group-%1-floor-%2").arg(integer(group.value("group_id"), 1)).arg(fid);
            if (!floor_keys.contains(layer)) { fail("Official chest has an unknown floor."); }
            add_layer(layer, floor_names[layer].isEmpty() ? layer : floor_names[layer]);
        } else if (integer(p.value("z_level"), -100, 100) != 0 || integer(p.value("icon_sign"), 0, 100) != 0) {
            layer = QStringLiteral("unresolved"); add_layer(layer, QStringLiteral("Floor unresolved · navigation disabled"));
        }
        auto key = map.surface(); key.layer = layer;
        map.dataset.markers.append({key, QString::number(integer(p.value("id"), 1)), categories[label], names[label], {},
            raw.x() + official_coordinate_offset, raw.y() + official_coordinate_offset,
            CollectionStatus::NotCollected, integer(p.value("display_state"), 0, 10) != 1, QStringLiteral("import")});
        if (p.value("area_id").isDouble()) { map.marker_areas[map.dataset.markers.last().id] = integer(p.value("area_id"), 0, 100000); }
    }
    for (auto i = expected.cbegin(); i != expected.cend(); ++i) { if (actual.value(i.key()) != i.value()) { fail("Official chest download is incomplete; the old cache is preserved."); } }
    if (map.dataset.markers.isEmpty()) { fail("Empty official chest snapshot rejected."); }
    // Do not retain author/account fields from the public response.
    map.revision = QString::fromLatin1(QCryptographicHash::hash(encode_json(map.dataset), QCryptographicHash::Sha256).toHex());
    for (auto& layer : map.dataset.maps) { layer.version = QStringLiteral("official-") + map.revision.left(24); }
    validate_dataset(map.dataset); return map;
}
OfficialMap refresh_official_map(const QString& root, int id, std::stop_token stop) {
    check_id(id);
#ifndef Q_OS_ANDROID
    // Reuse one anonymous connection pool for the complete checked snapshot.
    QNetworkAccessManager manager;
#endif
    const auto fetch = [&](const QUrl& url) {
#ifdef Q_OS_ANDROID
        return get(url, max_transfer_bytes, stop);
#else
        return get(manager, url, max_transfer_bytes, stop);
#endif
    };
    const auto info = fetch(endpoint("v3/map/info", id));
    const auto labels = fetch(endpoint("v2/map/label/tree", id));
    const auto points = fetch(endpoint("v3/map/point/list", id, QStringLiteral("&label_ids=13")));
    const auto groups = fetch(endpoint("v2/map/point_group", id));
    auto map = parse_official_map(id, info, labels, points, groups);
    try { parse_official_waypoints(map, fetch(endpoint("v3/map/point/list", id, QStringLiteral("&label_ids=1")))); }
    catch (const std::exception&) { if (stop.stop_requested()) { throw; } try { map.waypoints = load_official_map(root, id).waypoints; } catch (const std::exception&) {} }
    const auto check = response(fetch(endpoint("v3/map/info", id))).value("info").toObject().value("detail_v2").toObject();
    if (version(check.value("map_version")) != map.tile_version || pair(check.value("origin")) != map.raster_origin
        || pair(check.value("total_size")) != QPointF(map.raster_size.width(), map.raster_size.height())) { fail("Official map changed during download. Retry the update."); }
    map.directory = map_directory(root, id); cancelled(stop);
    write(QDir(map.directory).filePath(QStringLiteral("snapshot.json")), snapshot(map)); return map;
}
OfficialMap load_official_map(const QString& root, int id) {
    check_id(id); OfficialMap map; map.id = id; map.directory = map_directory(root, id);
    const auto doc = QJsonDocument::fromJson(read(QDir(map.directory).filePath(QStringLiteral("snapshot.json")), max_transfer_bytes)); const auto o = doc.object();
    if (o.value("format") != QJsonValue("teyvat-official-cache") || o.value("version") != QJsonValue(1) || integer(o.value("map_id"), 1) != id) { fail("Invalid official cache format."); }
    map.name = o.value("name").toString(); map.tile_version = version(o.value("tile_version")); map.revision = o.value("revision").toString();
    const auto size = pair(o.value("raster_size")); map.raster_size = QSize(integer(size.x(), 256, 65536), integer(size.y(), 256, 65536));
    map.raster_origin = pair(o.value("raster_origin")); validate_geometry(map);
    map.dataset = decode_json(QJsonDocument(o.value("dataset").toObject()).toJson(QJsonDocument::Compact));
    if (map.dataset.maps.isEmpty() || map.dataset.markers.isEmpty() || !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(map.revision).hasMatch()) { fail("Incomplete official cache."); }
    for (const auto& layer : map.dataset.maps) {
        if (layer.key.source != map.surface().source || layer.key.region != map.surface().region
            || layer.frame != QStringLiteral("hoyolab-map-%1-xy-down-v1").arg(id) || layer.width != 1000000 || layer.height != 1000000) { fail("Official cache coordinate frame mismatch."); }
    }
    auto checked = map.dataset;
    for (auto& layer : checked.maps) { layer.version = QStringLiteral("pending"); }
    if (QString::fromLatin1(QCryptographicHash::hash(encode_json(checked), QCryptographicHash::Sha256).toHex()) != map.revision) { fail("Official cache checksum mismatch."); }
    if (o.contains("navigation")) {
        const auto metadata = o.value("navigation").toObject();
        if (QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(metadata).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()) != o.value("navigation_hash").toString()) { fail("Official navigation metadata checksum mismatch."); }
        const auto areas = metadata.value("areas").toObject(); if (areas.size() > max_markers) { fail("Oversized area catalogue."); }
        QSet<QString> marker_ids; for (const auto& marker : map.dataset.markers) { marker_ids.insert(marker.id); }
        for (auto i = areas.begin(); i != areas.end(); ++i) { if (!marker_ids.contains(i.key())) { fail("Unknown regional marker."); } map.marker_areas[i.key()] = integer(i.value(), 0, 100000); }
        const auto floors = metadata.value("floors").toArray(), waypoints = metadata.value("waypoints").toArray();
        if (floors.size() > 2048 || waypoints.size() > max_layer_markers) { fail("Oversized navigation catalogue."); }
        QSet<QString> floor_keys;
        for (const auto& v : floors) { const auto f = v.toObject(); const auto b = f.value("bounds").toArray();
            if (b.size() != 4) { fail("Invalid cached floor geometry."); }
            OfficialFloor entry{f.value("layer").toString(), f.value("title").toString(), QUrl(f.value("url").toString()),
                {number(b[0]), number(b[1]), number(b[2]), number(b[3])}, std::nullopt};
            if (!QRegularExpression(QStringLiteral("^group-[1-9][0-9]*-floor-[0-9]+$")).match(entry.layer).hasMatch() || floor_keys.contains(entry.layer)
                || entry.title.size() > 100 || !overlay_url(entry.overlay) || !entry.bounds.isValid() || entry.bounds.width() > 16384 || entry.bounds.height() > 16384
                || !QRectF(0, 0, 1000000, 1000000).contains(entry.bounds)) { fail("Invalid cached floor."); }
            floor_keys.insert(entry.layer);
            if (f.contains("entrance")) { entry.entrance = pair(f.value("entrance")); if (!QRectF(0, 0, 1000000, 1000000).contains(*entry.entrance)) { fail("Invalid cached entrance."); } }
            map.floors.append(entry);
        }
        QSet<QString> waypoint_ids;
        for (const auto& v : waypoints) { const auto w = v.toObject(); OfficialWaypoint entry{w.value("id").toString(), w.value("name").toString(), w.value("layer").toString(), pair(w.value("position")), integer(w.value("area"), 0, 100000)};
            if (!QRegularExpression(QStringLiteral("^[1-9][0-9]{0,8}$")).match(entry.id).hasMatch() || waypoint_ids.contains(entry.id) || entry.name.size() > 100
                || (entry.layer != QStringLiteral("surface") && !floor_keys.contains(entry.layer)) || !QRectF(0, 0, 1000000, 1000000).contains(entry.position)) { fail("Invalid cached waypoint."); }
            waypoint_ids.insert(entry.id); map.waypoints.append(entry);
        }
    }
    return map;
}
QImage decode_official_tile(const QByteArray& bytes) {
    if (bytes.size() < 12 || bytes.size() > 2 * 1024 * 1024 || bytes.first(4) != "RIFF" || bytes.mid(8, 4) != "WEBP"
        || qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + 4)) != static_cast<quint32>(bytes.size() - 8)) { fail("Invalid or truncated official WebP tile."); }
    // Qt 6.11's WebP handler peeks sizeof(WebPBitstreamFeatures) bytes even for
    // valid 34-byte lossless tiles. Pad only the decoder buffer after checking
    // the complete original RIFF length; preserve original cache bytes/hashes.
    auto padded = bytes; if (padded.size() < 64) { padded.append(QByteArray(64 - padded.size(), 0)); }
    QBuffer buffer(&padded); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer, "webp");
    if (reader.size() != QSize(256, 256)) { fail("Unsupported official tile dimensions."); }
    auto image = reader.read();
    if (image.isNull() || image.size() != QSize(256, 256)) { fail("Official tile decoding failed."); } return image;
}
void parse_official_waypoints(OfficialMap& map, const QByteArray& bytes) {
    const auto data = response(bytes); const auto points = data.value("point_list").toArray(), labels = data.value("label_list").toArray();
    if (!data.value("point_list").isArray() || points.size() > max_layer_markers || labels.size() > 1000) { fail("Invalid official waypoint catalogue."); }
    QHash<int, QString> names; for (const auto& v : labels) { const auto label = v.toObject(); names[integer(label.value("id"), 1)] = label.value("name").toString().left(100); }
    QVector<OfficialWaypoint> waypoints; QSet<QString> ids, floors;
    for (const auto& f : map.floors) { floors.insert(f.layer); }
    for (const auto& v : points) { const auto p = v.toObject(); const int label = integer(p.value("label_id"), 1);
        if ((label != 2 && label != 3 && label != 154) || integer(p.value("display_state"), 0, 10) != 1) { continue; }
        QString layer = QStringLiteral("surface");
        if (p.value("point_group").isObject()) { const auto f = p.value("point_group").toObject(); layer = QStringLiteral("group-%1-floor-%2").arg(integer(f.value("group_id"), 1)).arg(integer(f.value("floor_id"))); if (!floors.contains(layer)) { continue; } }
        else if (integer(p.value("icon_sign"), 0, 100) != 0 || integer(p.value("z_level"), -100, 100) != 0) { continue; }
        const auto id = QString::number(integer(p.value("id"), 1)); if (ids.contains(id)) { fail("Duplicate official waypoint."); } ids.insert(id);
        const QPointF position(number(p.value("x_pos")) + official_coordinate_offset, number(p.value("y_pos")) + official_coordinate_offset);
        if (!QRectF(0, 0, 1000000, 1000000).contains(position)) { fail("Invalid official waypoint position."); }
        waypoints.append({id, names.value(label, QStringLiteral("Teleport")), layer, position, p.value("area_id").isDouble() ? integer(p.value("area_id"), 0, 100000) : 0});
    }
    map.waypoints = std::move(waypoints);
}
QRectF official_region_bounds(const OfficialMap& map, int area) {
    if (area == 1001) {
        // A vicinity preset around the official Dragonspine Statue, not a claim
        // that its radius is an official administrative/terrain boundary.
        for (const auto& w : map.waypoints) { if (w.id == QStringLiteral("6537") && w.area == 1) { return {w.position - QPointF(1100, 1100), QSizeF(2200, 2200)}; } }
        return {};
    }
    QRectF bounds; bool first = true;
    for (const auto& m : map.dataset.markers) { if (map.marker_areas.value(m.id) == area) {
        const QRectF point(m.x, m.y, .01, .01); bounds = first ? point : bounds.united(point); first = false;
    } }
    return first ? QRectF{} : bounds.adjusted(-512, -512, 512, 512);
}
QSet<QString> official_region_markers(const OfficialMap& map, int area) {
    QSet<QString> result; const auto vicinity = area == 1001 ? official_region_bounds(map, area) : QRectF{};
    for (const auto& m : map.dataset.markers) {
        if (area == 0 || (area == 1001 ? map.marker_areas.value(m.id) == 1 && vicinity.contains({m.x, m.y}) : map.marker_areas.value(m.id) == area)) { result.insert(m.id); }
    }
    return result;
}
QVector<OfficialWaypoint> official_teleport_options(const OfficialMap& map, const QString& layer, QPointF player, QPointF target) {
    if (!std::isfinite(player.x()) || !std::isfinite(player.y()) || !std::isfinite(target.x()) || !std::isfinite(target.y())) { return {}; }
    const double direct = QLineF(player, target).length(); if (direct < 350) { return {}; }
    QVector<OfficialWaypoint> options;
    for (const auto& point : map.waypoints) {
        if (point.layer == layer && QLineF(point.position, target).length() < direct * .5) { options.append(point); }
    }
    std::sort(options.begin(), options.end(), [&](const auto& a, const auto& b) {
        const double da = QLineF(a.position, target).length(), db = QLineF(b.position, target).length();
        return da == db ? a.id < b.id : da < db;
    });
    if (options.size() > 3) { options.resize(3); } return options;
}
std::optional<LocalizationConfig> official_floor_reference(const OfficialMap& map, const QString& layer, QPointF position, const LocalizationConfig& hud, std::stop_token stop, bool allow_download) {
    const auto floor = std::find_if(map.floors.cbegin(), map.floors.cend(), [&](const auto& f) { return f.layer == layer; });
    if (floor == map.floors.cend() || !floor->bounds.adjusted(-128, -128, 128, 128).contains(position)) { return {}; }
    if (!overlay_url(floor->overlay)) { fail("Invalid official floor image URL."); }
    const auto path = QDir(map.directory).filePath(QStringLiteral("floors/") + QString::fromLatin1(QCryptographicHash::hash(floor->overlay.toEncoded(), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".png"));
    const auto decode = [](const QByteArray& bytes) {
        QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer, "png"); const auto size = reader.size();
        if (!size.isValid() || size.width() > 4096 || size.height() > 4096) { fail("Invalid official floor image dimensions."); }
        const auto image = reader.read(); if (image.isNull()) { fail("Cannot decode official floor image."); } return image;
    };
    QImage image;
    if (QFileInfo::exists(path)) { try { image = decode(read(path, 16 * 1024 * 1024)); } catch (const std::exception&) { cancelled(stop); } }
    if (image.isNull()) {
        if (!allow_download) { return {}; }
        const auto bytes = get(floor->overlay, 16 * 1024 * 1024, stop); image = decode(bytes); write(path, bytes);
    }
    auto config = hud; config.origin = {std::floor(position.x()) - 512, std::floor(position.y()) - 512};
    config.reference = QImage(1024, 1024, QImage::Format_ARGB32); config.reference.fill(Qt::transparent);
    { QPainter p(&config.reference); p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(floor->bounds.translated(-config.origin), image); }
    config.units_per_pixel = {1, 1}; config.scales = {.5, 4}; config.rotations = {0}; config.allow_rotation = true; config.method = LocalizationMethod::Orb;
    validate_localization(config); return config;
}
TrackingSetup make_official_tracking_setup(const OfficialMap& map, const TrackingSetup& hud, const QString& requested_layer) {
    auto setup = hud; setup.dataset = {}; setup.analysis = {}; setup.synthetic = false;
    auto selected = map.surface(); if (!requested_layer.isEmpty()) { selected.layer = requested_layer; }
    for (const auto& layer : map.dataset.maps) { if (layer.key == selected) { setup.dataset.maps.append(layer); } }
    for (const auto& marker : map.dataset.markers) { if (marker.map == selected) { setup.dataset.markers.append(marker); } }
    if (setup.dataset.maps.size() != 1) { fail("Official surface layer is missing."); }
    setup.analysis.map = selected; setup.analysis.frame = setup.dataset.maps.first().frame;
    setup.analysis.fingerprint = QByteArray::fromHex(map.revision.toLatin1()); setup.analysis.radius = 20;
    setup.analysis.provenance = setup.dataset.maps.first().provenance; setup.analysis.license = setup.dataset.maps.first().license;
    setup.analysis.reference_bounds = QRectF(map.world({0, 0}), QSizeF(map.raster_size));
    if (selected.layer != QStringLiteral("surface")) {
        const auto f = std::find_if(map.floors.cbegin(), map.floors.cend(), [&](const auto& floor) { return floor.layer == selected.layer; });
        if (f == map.floors.cend()) { fail("Official floor geometry is unavailable."); } setup.analysis.reference_bounds = f->bounds;
    }
    validate_dataset(setup.dataset); validate_localization(setup.localization); return setup;
}
QImage official_tile(const OfficialMap& map, int x, int y, int zoom, std::stop_token stop) {
    check_id(map.id); (void)version(map.tile_version); validate_geometry(map); cancelled(stop);
    if (zoom < -4 || zoom > 0 || x < 0 || y < 0 || x * 256.0 >= map.raster_size.width() * std::exp2(zoom)
        || y * 256.0 >= map.raster_size.height() * std::exp2(zoom)) { fail("Official tile is outside the map."); }
    const auto name = QStringLiteral("%1_%2_%3%4.webp").arg(x).arg(y).arg(zoom < 0 ? QLatin1Char('N') : QLatin1Char('P')).arg(std::abs(zoom));
    const auto path = QDir(map.directory).filePath(map.tile_version + QLatin1Char('/') + name);
    if (QFileInfo::exists(path)) {
        try { return decode_official_tile(read(path, 2 * 1024 * 1024)); }
        catch (const std::runtime_error&) { cancelled(stop); } // Re-fetch a damaged cached tile once.
    }
    const auto bytes = get(QUrl(QStringLiteral("https://act-webstatic.hoyoverse.com/map_manage/map/%1/%2/%3").arg(map.id).arg(map.tile_version, name)), 2 * 1024 * 1024, stop);
    const auto image = decode_official_tile(bytes); write(path, bytes); return image;
}
QImage official_region(const OfficialMap& map, QRect bounds, int zoom, std::stop_token stop) {
    if (bounds.isEmpty() || bounds.width() > 1024 || bounds.height() > 1024 || bounds.x() < 0 || bounds.y() < 0
        || bounds.right() >= map.raster_size.width() * std::exp2(zoom) || bounds.bottom() >= map.raster_size.height() * std::exp2(zoom)) { fail("Invalid local official map viewport."); }
    QImage image(bounds.size(), QImage::Format_ARGB32); image.fill(Qt::transparent); QPainter p(&image);
    for (int y = bounds.top() / 256; y <= bounds.bottom() / 256; ++y) { for (int x = bounds.left() / 256; x <= bounds.right() / 256; ++x) {
        p.drawImage(QPoint(x * 256, y * 256) - bounds.topLeft(), official_tile(map, x, y, zoom, stop));
    } } p.end(); return image;
}
} // namespace teyvat
