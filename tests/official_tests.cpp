#include "teyvat/official_map.hpp"
#include "teyvat/store.hpp"
#include "localization_fixtures.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLineF>
#include <QTemporaryDir>
#include <QRandomGenerator>
#include <QTest>
#include <cmath>
#include <numbers>

namespace {
QByteArray response(QJsonObject data) { return QJsonDocument(QJsonObject{{"retcode", 0}, {"data", data}}).toJson(QJsonDocument::Compact); }
struct Fixture {
    QJsonObject info{{"id", 2}, {"name", "Synthetic official schema"}, {"detail_v2", QJsonObject{
        {"origin", QJsonArray{512, 512}}, {"total_size", QJsonArray{1024, 1024}}, {"min_zoom", -4}, {"max_zoom", 0},
        {"map_version", QString(32, QLatin1Char('a'))}}}};
    QJsonArray labels{QJsonObject{{"id", 13}, {"children", QJsonArray{
        QJsonObject{{"id", 17}, {"parent_id", 13}, {"name", "Common Chest"}, {"point_count", 4}}}}}};
    QJsonArray points;
    QJsonArray groups{QJsonObject{{"id", 48}, {"map_id", 2}, {"floors", QJsonArray{QJsonObject{{"id", 0}, {"floor_name", "First basement"}}}}},
        QJsonObject{{"id", 49}, {"map_id", 2}, {"floors", QJsonArray{QJsonObject{{"id", 0}, {"floor_name", "Other basement"}}}}}};
    Fixture() {
        for (int i = 0; i < 4; ++i) {
            points.append(QJsonObject{{"id", 100 + i}, {"label_id", 17}, {"x_pos", -100 + i * 20}, {"y_pos", 100},
                {"display_state", 1}, {"z_level", i == 3 ? 1 : 0}, {"icon_sign", i == 1 || i == 2 ? 1 : 0},
                {"author_name", "Must never enter cache or local markers"},
                {"point_group", i == 1 || i == 2 ? QJsonValue(QJsonObject{{"group_id", 47 + i}, {"floor_id", 0}}) : QJsonValue(QJsonValue::Null)}});
        }
    }
    teyvat::OfficialMap parse() const {
        return teyvat::parse_official_map(2, response({{"info", info}}), response({{"tree", labels}}),
            response({{"point_list", points}}), response({{"list", groups}}));
    }
};
QString scratch() { const auto root = QCoreApplication::applicationDirPath() + QStringLiteral("/test-tmp"); QDir().mkpath(root); return root + QStringLiteral("/official-XXXXXX"); }
void write(const QString& path, const QByteArray& data) { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(data), data.size()); }
}
class OfficialTests final : public QObject {
    Q_OBJECT
private slots:
    void resumed_hud_recovers_immediately_without_bypassing_plane_ambiguity() {
        // The first new minimap after a loading/menu/focus gap can be far from
        // the old reference, even if another search finished a moment ago.
        QVERIFY(teyvat::global_recovery_due(true, 1, 100, true, false));
        QVERIFY(!teyvat::global_recovery_due(true, 1, 100, false, false));
        QVERIFY(!teyvat::global_recovery_due(true, 3, 2999, false, false));
        QVERIFY(teyvat::global_recovery_due(true, 3, 3000, false, false));
        QVERIFY(teyvat::global_recovery_due(false, 0, 3000, false, false));
        QVERIFY(!teyvat::global_recovery_due(true, 10, 5000, true, true));
    }
    void teleport_options_preserve_levels_and_do_not_claim_terrain_cost() {
        auto map = Fixture().parse();
        map.waypoints = {{"1", "Nearby destination", "surface", {499990, 500000}, 1},
            {"2", "Second", "surface", {499980, 500000}, 1}, {"3", "Third", "surface", {499970, 500000}, 1},
            {"4", "Fourth", "surface", {499960, 500000}, 1}, {"5", "Wrong floor", "group-48-floor-0", {500000, 500000}, 1},
            {"6", "No shortcut", "surface", {499100, 500000}, 1}};
        const auto options = teyvat::official_teleport_options(map, QStringLiteral("surface"), {499000, 500000}, {500000, 500000});
        QCOMPARE(options.size(), 3); QCOMPARE(options.first().id, QStringLiteral("1")); QCOMPARE(options.last().id, QStringLiteral("3"));
        QVERIFY(teyvat::official_teleport_options(map, QStringLiteral("surface"), {499990, 500000}, {500000, 500000}).isEmpty());
        const auto underground = teyvat::official_teleport_options(map, QStringLiteral("group-48-floor-0"), {499000, 500000}, {500000, 500000});
        QCOMPARE(underground.size(), 1); QCOMPARE(underground.first().id, QStringLiteral("5"));
    }
    void bright_snow_terrain_is_not_masked_as_ui() {
        auto map = Fixture().parse(); QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); map.directory = dir.path();
        QImage atlas(1024, 1024, QImage::Format_ARGB32); atlas.fill(QColor(190, 190, 190));
        { QPainter painter(&atlas); QRandomGenerator random(0x51eeu);
            for (int i = 0; i < 4000; ++i) { const int light = 205 + random.bounded(50);
                painter.fillRect(10 + random.bounded(990), 10 + random.bounded(990), 5 + random.bounded(18), 5 + random.bounded(18), QColor(light, light, light));
            }
        }
        const auto tiles = QDir(dir.path()).filePath(map.tile_version); QVERIFY(QDir().mkpath(tiles));
        QVERIFY(atlas.scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(QDir(tiles).filePath("0_0_N2.webp"), "WEBP", 100));
        for (int y = 0; y < 4; ++y) { for (int x = 0; x < 4; ++x) {
            QVERIFY(atlas.copy(x * 256, y * 256, 256, 256).save(QDir(tiles).filePath(QStringLiteral("%1_%2_P0.webp").arg(x).arg(y)), "WEBP", 100));
        } }
        teyvat::OfficialWorldLocator locator(map); teyvat::LocalizationConfig hud; hud.roi = {0, 0, 282, 282};
        const auto reference = teyvat::official_local_reference(map, {499948.5, 499958.5}, hud);
        const auto input = reference.reference.copy(371, 371, 282, 282);
        const auto truth = reference.origin + QPointF(511.5, 511.5);
        const auto local = teyvat::localize_minimap(input, reference); QVERIFY2(local.position, qPrintable(QStringLiteral("reason=%1 features=%2 inliers=%3").arg(int(local.reason)).arg(local.features).arg(local.inliers)));
        QVERIFY(QLineF({local.position->x, local.position->y}, truth).length() < 3);
        const auto found = locator.locate_minimap(input, hud);
        if (found) { QVERIFY(QLineF({found->estimate.position->x, found->estimate.position->y}, truth).length() < 3); }
        QVERIFY(!teyvat::official_floor_reference(map, QStringLiteral("unknown-floor"), {500000, 500000}, hud, {}, false));
        QImage white(282, 282, QImage::Format_RGB32); white.fill(Qt::white); QVERIFY(!locator.locate_minimap(white, hud));
    }
    void exact_regions_and_optional_navigation_metadata_are_validated() {
        Fixture fixture;
        for (int i = 0; i < fixture.points.size(); ++i) { auto p = fixture.points[i].toObject(); p["area_id"] = i < 2 ? 16 : 3; fixture.points[i] = p; }
        const auto map = fixture.parse(); QCOMPARE(teyvat::official_region_markers(map, 16), (QSet<QString>{QStringLiteral("100"), QStringLiteral("101")}));
        QCOMPARE(teyvat::official_region_markers(map, 0).size(), 4); QVERIFY(teyvat::official_region_markers(map, 999).isEmpty());
        QVERIFY(teyvat::official_region_bounds(map, 16).contains(QPointF(499900, 500100)));
        QTemporaryDir directory(scratch()); QVERIFY(directory.isValid()); QVERIFY(QDir().mkpath(directory.filePath("map-2")));
        QJsonObject metadata{{"areas", QJsonObject{{"100", 16}}}, {"floors", QJsonArray{}}, {"waypoints", QJsonArray{}}};
        const auto checksum = [](const QJsonObject& value) { return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()); };
        QJsonObject cached{{"format", "teyvat-official-cache"}, {"version", 1}, {"map_id", 2}, {"name", map.name}, {"tile_version", map.tile_version}, {"revision", map.revision},
            {"raster_size", QJsonArray{1024, 1024}}, {"raster_origin", QJsonArray{512, 512}}, {"dataset", QJsonDocument::fromJson(teyvat::encode_json(map.dataset)).object()},
            {"navigation", metadata}, {"navigation_hash", checksum(metadata)}};
        const auto path = directory.filePath("map-2/snapshot.json"); write(path, QJsonDocument(cached).toJson());
        QCOMPARE(teyvat::load_official_map(directory.path(), 2).marker_areas.value(QStringLiteral("100")), 16);
        metadata["areas"] = QJsonObject{{"100", 3}}; cached["navigation"] = metadata; write(path, QJsonDocument(cached).toJson());
        QVERIFY_EXCEPTION_THROWN((void)teyvat::load_official_map(directory.path(), 2), std::exception);
        metadata["areas"] = QJsonObject{{"nonexistent", 3}}; cached["navigation"] = metadata; cached["navigation_hash"] = checksum(metadata); write(path, QJsonDocument(cached).toJson());
        QVERIFY_EXCEPTION_THROWN((void)teyvat::load_official_map(directory.path(), 2), std::exception);
    }
    void floor_images_identify_a_plane_and_equal_stacked_floors_abstain() {
        auto map = Fixture().parse(); QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); map.directory = dir.path();
        const auto base = localization_fixtures::atlas(1800).scaled(1024, 1024, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const auto room = localization_fixtures::atlas(2900).scaled(1024, 1024, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const auto version = dir.filePath(map.tile_version); QVERIFY(QDir().mkpath(version));
        for (int y = 0; y < 4; ++y) { for (int x = 0; x < 4; ++x) { QVERIFY(base.copy(x * 256, y * 256, 256, 256).save(QDir(version).filePath(QStringLiteral("%1_%2_P0.webp").arg(x).arg(y)), "WEBP", 100)); } }
        const QRectF bounds(map.world({0, 0}), QSizeF(1024, 1024)); const auto world = map.world({500.5, 500.5});
        const auto add_floor = [&](QString layer, QString file) {
            const QUrl url(QStringLiteral("https://act-upload.hoyoverse.com/map_manage/20261109/") + file + QStringLiteral(".png"));
            map.floors.append({layer, file, url, bounds, std::nullopt}); QVERIFY(QDir().mkpath(dir.filePath("floors")));
            const auto name = QString::fromLatin1(QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".png");
            QVERIFY(room.save(dir.filePath(QStringLiteral("floors/") + name), "PNG"));
        };
        add_floor(QStringLiteral("group-48-floor-0"), QStringLiteral("first"));
        teyvat::LocalizationConfig hud; hud.roi = {0, 0, 282, 282}; const auto image = room.copy(360, 360, 282, 282);
        const auto found = teyvat::match_official_planes(map, image, hud, world, QStringLiteral("surface"));
        QVERIFY(!found.ambiguous); QVERIFY(found.match); QCOMPARE(found.match->layer, QStringLiteral("group-48-floor-0"));
        QVERIFY(std::hypot(found.match->estimate.position->x - world.x(), found.match->estimate.position->y - world.y()) < 3);
        add_floor(QStringLiteral("group-49-floor-0"), QStringLiteral("second"));
        const auto ambiguous = teyvat::match_official_planes(map, image, hud, world, QStringLiteral("group-48-floor-0"));
        QVERIFY(ambiguous.ambiguous); QVERIFY(!ambiguous.match);
        // Continuous surface checks can use the cache without network requests.
        map.floors.append({QStringLiteral("group-99-floor-0"), QStringLiteral("Uncached"), QUrl(QStringLiteral("https://act-upload.hoyoverse.com/map_manage/test/missing.png")), bounds, std::nullopt});
        QVERIFY(!teyvat::official_floor_reference(map, QStringLiteral("group-99-floor-0"), world, hud, {}, false));
        const auto cached = teyvat::match_official_planes(map, image, hud, world, QStringLiteral("surface"), false, {}, false);
        QVERIFY(cached.ambiguous); QVERIFY(!cached.match);
    }
    void complete_snapshot_separates_floor_zero_and_excludes_authors() {
        const auto map = Fixture().parse(); QCOMPARE(map.dataset.markers.size(), 4); QCOMPARE(map.dataset.maps.size(), 4);
        QCOMPARE(map.dataset.markers[0].map, map.surface());
        QCOMPARE(map.dataset.markers[1].map.layer, QStringLiteral("group-48-floor-0"));
        QCOMPARE(map.dataset.markers[2].map.layer, QStringLiteral("group-49-floor-0"));
        QCOMPARE(map.dataset.markers[3].map.layer, QStringLiteral("unresolved"));
        QVERIFY(!teyvat::encode_json(map.dataset).contains("author"));
        QCOMPARE(map.world({512, 512}), QPointF(500000, 500000));
        QCOMPARE(map.world({128, 128}, -2), QPointF(500000, 500000));
        QCOMPARE(map.raster({500032, 500064}, -2), QPointF(136, 144));
    }
    void reject_partial_changed_or_ambiguous_payloads() {
        Fixture f; f.points.removeLast(); QVERIFY_EXCEPTION_THROWN((void)f.parse(), std::runtime_error);
        f = Fixture(); f.points[1] = f.points[0]; QVERIFY_EXCEPTION_THROWN((void)f.parse(), std::runtime_error);
        f = Fixture(); auto p = f.points[1].toObject(); p["point_group"] = QJsonObject{{"group_id", 999}, {"floor_id", 0}}; f.points[1] = p;
        QVERIFY_EXCEPTION_THROWN((void)f.parse(), std::runtime_error);
        f = Fixture(); p = f.points[0].toObject(); p["label_id"] = 186; f.points[0] = p; QVERIFY_EXCEPTION_THROWN((void)f.parse(), std::runtime_error);
        f = Fixture(); p = f.points[0].toObject(); p["x_pos"] = "NaN"; f.points[0] = p; QVERIFY_EXCEPTION_THROWN((void)f.parse(), std::runtime_error);
    }
    void changed_raster_origin_preserves_world_coordinates_and_collected_progress() {
        Fixture f; auto before = f.parse();
        QTemporaryDir directory(scratch()); QVERIFY(directory.isValid()); teyvat::Store store(directory.filePath("local.db")); const auto profile = store.active_profile().id;
        store.apply_import(before.dataset, profile, false, store.review_import(before.dataset, profile, false).token);
        store.set_status(profile, {before.dataset.markers[0]}, teyvat::CollectionStatus::ManuallyConfirmed);
        auto detail = f.info["detail_v2"].toObject(); detail["origin"] = QJsonArray{530, 510}; detail["map_version"] = QString(32, QLatin1Char('b')); f.info["detail_v2"] = detail;
        auto after = f.parse(); QCOMPARE(after.dataset.markers[0].x, before.dataset.markers[0].x); QCOMPARE(after.dataset.maps[0].frame, before.dataset.maps[0].frame);
        auto moved = f.points[0].toObject(); moved["x_pos"] = -90; f.points[0] = moved; after = f.parse();
        store.apply_import(after.dataset, profile, false, store.review_import(after.dataset, profile, false).token);
        const auto markers = store.markers(profile, after.surface()); QCOMPARE(markers[0].x, 499910.); QCOMPARE(markers[0].status, teyvat::CollectionStatus::ManuallyConfirmed);
    }
    void short_valid_webp_is_decoded_without_accepting_truncation() {
        const auto bytes = QByteArray::fromHex("524946461A000000574542505650384C0D0000002FFFC03F10071011118888FE0700");
        QCOMPARE(teyvat::decode_official_tile(bytes).size(), QSize(256, 256)); QCOMPARE(bytes.size(), 34);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_official_tile(bytes.first(33)), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_official_tile(bytes + QByteArray(1, 0)), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_official_tile(QByteArray(34, 0)), std::runtime_error);
    }
    void cached_snapshot_rejects_accidental_coordinate_changes() {
        const auto map = Fixture().parse(); QTemporaryDir directory(scratch()); QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(directory.filePath("map-2")));
        QJsonObject cached{{"format", "teyvat-official-cache"}, {"version", 1}, {"map_id", 2}, {"name", map.name},
            {"tile_version", map.tile_version}, {"revision", map.revision}, {"raster_size", QJsonArray{1024, 1024}},
            {"raster_origin", QJsonArray{512, 512}}, {"dataset", QJsonDocument::fromJson(teyvat::encode_json(map.dataset)).object()}};
        const auto path = directory.filePath("map-2/snapshot.json"); write(path, QJsonDocument(cached).toJson());
        QCOMPARE(teyvat::load_official_map(directory.path(), 2).dataset, map.dataset);
        auto data = cached["dataset"].toObject(); auto markers = data["markers"].toArray(); auto marker = markers[0].toObject(); marker["x"] = 499990;
        markers[0] = marker; data["markers"] = markers; cached["dataset"] = data; write(path, QJsonDocument(cached).toJson());
        QVERIFY_EXCEPTION_THROWN((void)teyvat::load_official_map(directory.path(), 2), std::runtime_error);
    }
    void stationary_avatar_camera_rotates_and_ambiguous_cones_are_rejected() {
        teyvat::LocalizationConfig c; c.reference = localization_fixtures::atlas().scaled(512, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        c.method = teyvat::LocalizationMethod::Orb; c.roi = {0, 0, 282, 282}; c.scales = {1};
        teyvat::LocalizationResult pose; pose.reason = teyvat::LocalizationReason::Matched; pose.position = teyvat::RecordedPosition{255.5, 255.5, 4, .99};
        const auto make = [&](double heading, bool opposite) {
            auto image = c.reference.copy(115, 115, 282, 282);
            for (int y = 0; y < image.height(); ++y) { auto* row = reinterpret_cast<QRgb*>(image.scanLine(y)); for (int x = 0; x < image.width(); ++x) {
                const auto angle = std::atan2(x - 140.5, 140.5 - y) * 180 / std::numbers::pi;
                if (std::abs(std::remainder(angle - heading, 360.)) < 45 || (opposite && std::abs(std::remainder(angle - heading - 180, 360.)) < 45)) {
                    const auto p = row[x]; row[x] = qRgb(qRound(qRed(p) * .72 + 255 * .28), qRound(qGreen(p) * .72 + 255 * .28), qRound(qBlue(p) * .72 + 255 * .28));
                }
            } } return image;
        };
        for (const auto angle : {-175., -90., 0., 90., 175.}) {
            const auto heading = teyvat::estimate_camera_heading(make(angle, false), c, pose); QVERIFY(heading);
            QVERIFY2(std::abs(std::remainder(heading->degrees - angle, 360.)) < 4, qPrintable(QString::number(heading->degrees)));
        }
        QVERIFY(!teyvat::estimate_camera_heading(make(0, true), c, pose));
        QImage blank(282, 282, QImage::Format_RGB32); blank.fill(Qt::white); QVERIFY(!teyvat::estimate_camera_heading(blank, c, pose));
        pose.position.reset(); QVERIFY(!teyvat::estimate_camera_heading(make(0, false), c, pose));
    }
    void global_map_finds_player_and_ignores_panning_center() {
        auto map = Fixture().parse(); QTemporaryDir directory(scratch()); QVERIFY(directory.isValid()); map.directory = directory.path();
        const auto version_dir = QDir(directory.path()).filePath(map.tile_version); QVERIFY(QDir().mkpath(version_dir));
        const auto atlas = localization_fixtures::atlas(1800).scaled(1024, 1024, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QVERIFY(atlas.scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(QDir(version_dir).filePath("0_0_N2.webp"), "WEBP", 100));
        teyvat::OfficialWorldLocator locator(map);
        auto image = atlas.copy(180, 220, 512, 512); const QPointF player(290, 250);
        { QPainter painter(&image); painter.setPen(Qt::NoPen); painter.setBrush(QColor(0, 220, 255));
            painter.drawPolygon(QPolygonF{player + QPointF(0, -19), player + QPointF(-18, 17), player + QPointF(18, 17)}); }
        const auto pose = locator.locate(image); QVERIFY2(pose.position, qPrintable(QString::number(static_cast<int>(pose.reason))));
        QVERIFY(std::abs(pose.position->x - (500000 + 180 + player.x() - 512)) < 5);
        QVERIFY(std::abs(pose.position->y - (500000 + 220 + player.y() - 512)) < 5);
        // Same terrain without a unique player arrow is not an acquisition.
        QVERIFY(!locator.locate(atlas.copy(180, 220, 512, 512)).position);
        teyvat::OfficialWorldLocator cached(map); QVERIFY(cached.locate(image).position);
        // Gameplay acquisition has no world-map arrow assumption: its candidate
        // is independently verified against detailed cached tiles.
        for (int y = 0; y < 4; ++y) { for (int x = 0; x < 4; ++x) { QVERIFY(atlas.copy(x * 256, y * 256, 256, 256).save(QDir(version_dir).filePath(QStringLiteral("%1_%2_P0.webp").arg(x).arg(y)), "WEBP", 100)); } }
        teyvat::LocalizationConfig hud; hud.roi = {0, 0, 512, 512};
        const auto gameplay = cached.locate_minimap(atlas.copy(180, 220, 512, 512), hud); QVERIFY(gameplay);
        QVERIFY(std::hypot(gameplay->estimate.position->x - 499923.5, gameplay->estimate.position->y - 499963.5) < 3);
        const auto arrival_image = atlas.copy(180, 220, 512, 512);
        teyvat::OfficialWaypoint waypoint{"1", "Arrival", "surface", {499923.5, 499963.5}, 1};
        const auto arrival = teyvat::verify_official_waypoint(map, arrival_image, hud, waypoint);
        QVERIFY(arrival);
        QVERIFY(QLineF({arrival->estimate.position->x, arrival->estimate.position->y}, waypoint.position).length() < 3);
        waypoint.position += QPointF(128, 0);
        QVERIFY(!teyvat::verify_official_waypoint(map, arrival_image, hud, waypoint));
        QImage blank(512, 512, QImage::Format_RGB32); blank.fill(Qt::white); QVERIFY(!cached.locate_minimap(blank, hud));
        waypoint.position -= QPointF(128, 0);
        QVERIFY(!teyvat::verify_official_waypoint(map, blank, hud, waypoint));
    }
    void first_use_hud_requires_eye_and_centered_player() {
        teyvat::LocalizationConfig config; config.roi = {0, 0, 282, 282}; config.presence_roi = {285, 0, 64, 64};
        QImage image(350, 282, QImage::Format_ARGB32); image.fill(QColor(20, 30, 40));
        { QPainter painter(&image); painter.drawImage(0, 0, localization_fixtures::atlas().scaled(282, 282));
            painter.setPen(QPen(Qt::white, 7)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(QRect(290, 8, 53, 46)); }
        QVERIFY(!teyvat::learn_hud_presence(image, config));
        { QPainter painter(&image); painter.setPen(Qt::NoPen); painter.setBrush(QColor(0, 220, 255));
            painter.drawPolygon(QPolygonF{{140, 124}, {125, 154}, {156, 154}}); }
        const auto learned = teyvat::learn_hud_presence(image, config); QVERIFY(learned); QCOMPARE(learned->size(), QSize(64, 64));
        image.fill(Qt::white); QVERIFY(!teyvat::learn_hud_presence(image, config));
    }
};
QTEST_GUILESS_MAIN(OfficialTests)
#include "official_tests.moc"
