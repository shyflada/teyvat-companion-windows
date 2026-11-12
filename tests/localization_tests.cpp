#include "teyvat/detection.hpp"
#include "teyvat/store.hpp"
#include "localization_fixtures.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

namespace {
QString scratch() {
    const auto root = QCoreApplication::applicationDirPath() + QStringLiteral("/test-tmp");
    if (!QDir().mkpath(root)) { throw std::runtime_error("Cannot create test directory"); }
    return root + QStringLiteral("/localization-XXXXXX");
}
QImage feature_atlas(int seed) {
    return localization_fixtures::atlas(seed).scaled(640, 480, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
QImage feature_minimap(const QImage& atlas, QPointF center, double angle = 0, double scale = 1) {
    QImage image(282, 282, QImage::Format_ARGB32); image.fill(QColor(45, 55, 65));
    QPainter p(&image); p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.translate(141, 141); p.rotate(-angle); p.scale(1 / scale, 1 / scale);
    p.drawImage(QPointF(-center.x() - .5, -center.y() - .5), atlas); p.resetTransform();
    p.setPen(QPen(Qt::white, 3)); p.drawEllipse(2, 2, 277, 277);
    p.setBrush(Qt::white); p.drawPolygon(QPolygonF{{140, 125}, {130, 150}, {151, 150}});
    p.drawEllipse(195, 90, 12, 12); return image;
}
teyvat::LocalizationConfig feature_config(const QImage& reference) {
    teyvat::LocalizationConfig c; c.reference = reference; c.method = teyvat::LocalizationMethod::Orb;
    c.roi = {0, 0, 282, 282}; c.scales = {.7, 1.3}; c.rotations = {0, 35, 90, 180}; return c;
}
}
class LocalizationTests final : public QObject {
    Q_OBJECT
private slots:
    void narrow_low_contrast_island_requires_dense_verification_and_reports_uncertainty_data() {
        QTest::addColumn<bool>("amber");
        QTest::newRow("grass") << false;
        QTest::newRow("amber rock") << true;
    }
    void narrow_low_contrast_island_requires_dense_verification_and_reports_uncertainty() {
        QFETCH(bool, amber);
        teyvat::LocalizationConfig config; config.reference = QImage(1024, 1024, QImage::Format_ARGB32); config.reference.fill(QColor(38, 62, 86));
        quint32 random = 1847;
        for (int y = 435; y < 575; ++y) { for (int x = 525; x < 585; ++x) {
            random = random * 1664525U + 1013904223U; const int shade = static_cast<int>((random >> 16) % 35);
            config.reference.setPixel(x, y, amber ? qRgb(199 + shade, 139 + shade, 40 + shade)
                                                   : qRgb(55 + shade, 76 + shade, 64 + shade));
        } }
        config.roi = {0, 0, 282, 282}; config.method = teyvat::LocalizationMethod::Orb; config.scales = {.5, 4}; config.allow_rotation = true;
        const auto image = config.reference.copy(370, 370, 282, 282); const auto result = teyvat::localize_minimap(image, config);
        QVERIFY(result.position); QVERIFY(result.limited_support); QVERIFY(result.position->uncertainty >= 6);
        QVERIFY(std::hypot(result.position->x - 510.5, result.position->y - 510.5) < 2);
        QVERIFY(!teyvat::localize_minimap(image.flipped(Qt::Horizontal), config).position);
    }
    void orb_original_terrain_has_bounded_position_rotation_and_scale() {
        int count = 0; double maximum = 0;
        for (int seed = 1101; seed <= 1103; ++seed) {
            const auto atlas = feature_atlas(seed); const auto config = feature_config(atlas);
            for (int pose = 0; pose < 4; ++pose) {
                const QPointF center(290 + pose * 10, 220 + pose * 8);
                const double scale = pose % 2 ? 1.15 : .85;
                const auto result = teyvat::localize_minimap(feature_minimap(atlas, center, config.rotations[pose], scale), config);
                QVERIFY2(result.position, qPrintable(QStringLiteral("seed=%1 pose=%2 inliers=%3 reason=%4").arg(seed).arg(pose).arg(result.inliers).arg(static_cast<int>(result.reason))));
                const double error = std::hypot(result.position->x - center.x(), result.position->y - center.y());
                maximum = std::max(maximum, error); QVERIFY(error <= 3);
                QVERIFY(std::abs(std::remainder(result.rotation - config.rotations[pose], 360.0)) <= 2);
                QVERIFY(std::abs(result.scale - scale) < .03); QVERIFY(result.inliers >= 12); ++count;
            }
        }
        qInfo("ORB original synthetic terrain: %d/12 matched; max position error %.3f reference pixels", count, maximum);
        QCOMPARE(count, 12);
    }
    void orb_rejects_unrelated_blank_wrong_geometry_and_missing_hud() {
        const auto atlas = feature_atlas(1200); auto c = feature_config(atlas);
        const auto image = feature_minimap(atlas, {300, 240});
        QImage blank(image.size(), QImage::Format_ARGB32); blank.fill(Qt::gray);
        QVERIFY(!teyvat::localize_minimap(blank, c).position);
        QVERIFY(!teyvat::localize_minimap(feature_minimap(feature_atlas(1300), {300, 240}), c).position);
        c.rotations = {90}; QVERIFY(!teyvat::localize_minimap(image, c).position);
        c.rotations = {0}; c.scales = {1.2, 1.3}; QVERIFY(!teyvat::localize_minimap(image, c).position);
        c.scales = {.7, 1.3}; c.presence_roi = {20, 20, 32, 32}; c.presence_template = image.copy(c.presence_roi);
        QVERIFY(teyvat::localize_minimap(image, c).position);
        auto hidden = image; { QPainter painter(&hidden); painter.fillRect(c.presence_roi, Qt::gray); }
        QCOMPARE(teyvat::localize_minimap(hidden, c).reason, teyvat::LocalizationReason::MinimapAbsent);
        c.presence_template.fill(Qt::gray); QVERIFY_EXCEPTION_THROWN((void)teyvat::localize_minimap(image, c), std::exception);
        c.presence_template = {}; std::stop_source stop; stop.request_stop();
        QVERIFY_EXCEPTION_THROWN((void)teyvat::localize_minimap(image, c, stop.get_token()), std::exception);
    }
    void rotating_minimap_keeps_position_and_measures_continuous_angle() {
        const auto atlas = feature_atlas(1102); auto c = feature_config(atlas);
        c.rotations = {0}; c.allow_rotation = true; const QPointF center(310, 240);
        for (double angle : {17, 43, 97, 137, 179, 227, 271, 311}) {
            const auto estimate = teyvat::localize_minimap(feature_minimap(atlas, center, angle, .85), c);
            QVERIFY2(estimate.position, qPrintable(QStringLiteral("angle=%1 reason=%2").arg(angle).arg(static_cast<int>(estimate.reason))));
            QVERIFY(std::hypot(estimate.position->x - center.x(), estimate.position->y - center.y()) < 3);
            QVERIFY(std::abs(std::remainder(estimate.rotation - angle, 360.0)) < 2);
            c.allow_rotation = false; QVERIFY(!teyvat::localize_minimap(feature_minimap(atlas, center, angle, .85), c).position);
            c.allow_rotation = true;
        }
    }
    void held_out_positions_rotation_scale_and_noise() {
        int accepted = 0, clean_accepted = 0; double largest_error = 0; QElapsedTimer timer; timer.start();
        for (int seed = 801; seed <= 808; ++seed) {
            const auto reference = localization_fixtures::atlas(seed); const auto config = localization_fixtures::config(reference);
            for (int pose = 0; pose < 4; ++pose) {
                const QPointF center(90.25 + (seed % 4) * 21, 78.4 + pose * 22);
                const QPointF integer_center(std::round(center.x()), std::round(center.y()));
                const auto clean = teyvat::localize_minimap(localization_fixtures::minimap(reference, integer_center, config.rotations[pose], pose % 2 ? 1.5 : 1), config);
                QVERIFY(clean.position); QVERIFY(std::hypot(clean.position->x - integer_center.x(), clean.position->y - integer_center.y()) <= 1); ++clean_accepted;
                auto image = localization_fixtures::minimap(reference, center, config.rotations[pose], pose % 2 ? 1.5 : 1, 3);
                for (int y = 0; y < image.height(); ++y) { auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
                    for (int x = 0; x < image.width(); ++x) { row[x] = qRgb(qMin(255, qRed(row[x]) + 7), qMin(255, qGreen(row[x]) + 7), qMin(255, qBlue(row[x]) + 7)); } }
                const auto result = teyvat::localize_minimap(image, config);
                if (result.position) { ++accepted; largest_error = std::max(largest_error, std::hypot(result.position->x - center.x(), result.position->y - center.y())); }
                else { qInfo("Missed synthetic seed=%d pose=%d reason=%d score=%.4f runner=%.4f", seed, pose, static_cast<int>(result.reason), result.similarity, result.runner_up); }
            }
        }
        qInfo("SYNTHETIC LOCALIZATION ONLY: clean=%d/32 perturbed_accepted=%d/32 abstained=%d max_error=%.3f units elapsed=%lld ms", clean_accepted, accepted, 32 - accepted, largest_error, timer.elapsed());
        QCOMPARE(clean_accepted, 32); QVERIFY(accepted > 0); QVERIFY(largest_error <= 2);
        const auto output = qEnvironmentVariable("TEYVAT_LOCALIZATION_METRICS_PATH");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(QFileInfo(output).absolutePath()));
            const QJsonObject metrics{{QStringLiteral("scope"), QStringLiteral("Original synthetic terrain only; not Genshin accuracy")},
                {QStringLiteral("clean_accepted"), clean_accepted}, {QStringLiteral("clean_cases"), 32},
                {QStringLiteral("perturbed_accepted"), accepted}, {QStringLiteral("perturbed_cases"), 32}, {QStringLiteral("perturbed_abstentions"), 32 - accepted},
                {QStringLiteral("max_accepted_error_local_units"), largest_error}, {QStringLiteral("accepted_error_tolerance"), 2},
                {QStringLiteral("minimum_similarity"), 0.97}, {QStringLiteral("elapsed_ms_including_clean_cases"), timer.elapsed()},
                {QStringLiteral("real_game_accuracy"), QJsonValue(QJsonValue::Null)}};
            detection_fixtures::write(output, QJsonDocument(metrics).toJson());
        }
    }
    void ambiguity_blank_occlusion_and_unrelated_image_abstain() {
        auto reference = localization_fixtures::atlas(); auto config = localization_fixtures::config(reference);
        QImage blank(65, 65, QImage::Format_ARGB32); blank.fill(Qt::gray);
        QCOMPARE(teyvat::localize_minimap(blank, config).reason, teyvat::LocalizationReason::LowTexture);
        const auto unrelated = localization_fixtures::minimap(localization_fixtures::atlas(999), {100, 120});
        QVERIFY(!teyvat::localize_minimap(unrelated, config).position);
        auto occluded = localization_fixtures::minimap(reference, {100, 120});
        { QPainter p(&occluded); p.fillRect(0, 0, 33, 65, Qt::gray); }
        QVERIFY(!teyvat::localize_minimap(occluded, config).position);
        auto uncalibrated = config; uncalibrated.rotations = {0}; uncalibrated.scales = {1};
        QVERIFY(!teyvat::localize_minimap(localization_fixtures::minimap(reference, {100, 120}, 45, 1.5), uncalibrated).position);
        const auto patch = reference.copy(50, 60, 81, 81);
        { QPainter p(&reference); p.drawImage(200, 60, patch); }
        config.reference = reference;
        const auto repeated = teyvat::localize_minimap(localization_fixtures::minimap(reference, {90, 100}), config);
        QCOMPARE(repeated.reason, teyvat::LocalizationReason::Ambiguous); QVERIFY(!repeated.position);
    }
    void pixel_positions_drive_collection_without_supplied_coordinates() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        const auto path = localization_fixtures::bundle(dir.filePath(QStringLiteral("bundle")));
        const auto analysis = teyvat::analyze_recording(path);
        QCOMPARE(analysis.localizations.size(), 10); QCOMPARE(analysis.openings.size(), 2);
        for (const auto& item : analysis.localizations) { QVERIFY(item.estimate.position); }
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); const auto data = localization_fixtures::dataset(); const auto p = store.active_profile().id;
        store.apply_import(data, p, false, store.review_import(data, p, false).token);
        const auto results = store.apply_detections(p, analysis); QCOMPARE(results.size(), 2);
        for (const auto& result : results) { QCOMPARE(result.outcome, teyvat::DetectionOutcome::Applied); }
        store.undo_last(p); QCOMPARE(store.apply_detections(p, analysis).first().outcome, teyvat::DetectionOutcome::Duplicate);
        const auto output = qEnvironmentVariable("TEYVAT_LOCALIZATION_EXAMPLE_DIR");
        if (!output.isEmpty()) { (void)localization_fixtures::bundle(output); }
    }
    void calibration_input_and_cancellation_guards() {
        auto config = localization_fixtures::config(localization_fixtures::atlas()); const auto image = localization_fixtures::minimap(config.reference, {100, 120});
        config.scales = {0}; QVERIFY_EXCEPTION_THROWN((void)teyvat::localize_minimap(image, config), std::exception);
        config = localization_fixtures::config(localization_fixtures::atlas()); config.roi.moveLeft(2);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::localize_minimap(image, config), std::exception);
        std::stop_source stop; stop.request_stop(); config.roi.moveLeft(0);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::localize_minimap(image, config, stop.get_token()), std::exception);
        QTemporaryDir dir(scratch()); const auto path = localization_fixtures::bundle(dir.path());
        QVERIFY_EXCEPTION_THROWN(teyvat::analyze_recording(path, stop.get_token()), std::exception);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); auto root = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        auto frames = root[QStringLiteral("frames")].toArray(); auto first = frames[0].toObject(); first[QStringLiteral("position")] = QJsonObject{{QStringLiteral("x"), 100}, {QStringLiteral("y"), 120}}; frames[0] = first; root[QStringLiteral("frames")] = frames;
        detection_fixtures::save_bundle(dir.path(), root);
        QVERIFY_EXCEPTION_THROWN(teyvat::analyze_recording(path), std::exception);
    }
    void missing_minimap_and_wrong_calibration_never_collect() {
        QTemporaryDir dir(scratch()); const auto path = localization_fixtures::bundle(dir.filePath(QStringLiteral("bundle")));
        QFile manifest(path); QVERIFY(manifest.open(QIODevice::ReadOnly)); auto root = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
        auto options = root[QStringLiteral("localization")].toObject(); options[QStringLiteral("units_per_pixel")] = QJsonObject{{QStringLiteral("x"), 10}, {QStringLiteral("y"), 10}};
        auto changed = root; changed[QStringLiteral("localization")] = options; detection_fixtures::save_bundle(QFileInfo(path).absolutePath(), changed);
        const auto bad_calibration = teyvat::analyze_recording(path);
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); const auto data = localization_fixtures::dataset(); const auto profile = store.active_profile().id;
        store.apply_import(data, profile, false, store.review_import(data, profile, false).token);
        QVERIFY_EXCEPTION_THROWN(store.apply_detections(profile, bad_calibration), std::exception); QVERIFY(store.history(profile).isEmpty());
        detection_fixtures::save_bundle(QFileInfo(path).absolutePath(), root);
        for (const auto& entry : root[QStringLiteral("frames")].toArray()) {
            const auto frame_path = QDir(QFileInfo(path).absolutePath()).filePath(entry.toObject()[QStringLiteral("image")].toString());
            QImage image(frame_path); { QPainter p(&image); p.fillRect(144, 24, 65, 65, Qt::gray); } QVERIFY(image.save(frame_path, "PNG"));
        }
        const auto missing = teyvat::analyze_recording(path); QCOMPARE(missing.openings.size(), 2);
        const auto results = store.apply_detections(profile, missing);
        for (const auto& item : results) { QCOMPARE(item.outcome, teyvat::DetectionOutcome::Unmatched); }
        QVERIFY(store.history(profile).isEmpty());
    }
};
QTEST_GUILESS_MAIN(LocalizationTests)
#include "localization_tests.moc"
