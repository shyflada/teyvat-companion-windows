#include "teyvat/tracking.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

namespace {
QString example() { return QCoreApplication::applicationDirPath() + QStringLiteral("/examples/localization"); }
QString scratch() { const auto root = QCoreApplication::applicationDirPath() + QStringLiteral("/test-tmp"); QDir().mkpath(root); return root + QStringLiteral("/tracking-XXXXXX"); }
void seed(teyvat::Store& store, const teyvat::TrackingSetup& setup) { const auto p = store.active_profile().id; store.apply_import(setup.dataset, p, false, store.review_import(setup.dataset, p, false).token); }
QImage frame(int i) { return QImage(example() + QStringLiteral("/frame-%1.png").arg(i, 3, 10, QLatin1Char('0'))); }
void feed(teyvat::TrackingSession& session, int i, qint64 time) {
    const auto image = frame(i); session.observe(teyvat::localize_minimap(image, session.setup().localization), image.copy(session.setup().opening_roi), time);
}
}
class TrackingTests final : public QObject {
    Q_OBJECT
private slots:
    void animation_survives_a_brief_visual_miss_but_not_a_menu_or_long_gap() {
        const auto scene = [](int kind) {
            QImage image(256, 256, QImage::Format_ARGB32); image.fill(QColor(25, 35, 45)); QPainter painter(&image);
            if (kind == 0) { painter.fillRect(195, 100, 24, 17, Qt::white); }
            if (kind == 1) { painter.setPen(Qt::NoPen); painter.setBrush(QColor(255, 225, 95)); painter.drawEllipse(QPoint(128, 130), 100, 100); }
            if (kind == 2) { painter.fillRect(160, 100, 60, 80, QColor(30, 180, 240)); }
            return image;
        };
        for (int mode = 0; mode < 7; ++mode) {
            QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
            setup.synthetic = false; setup.analysis.map.source = QStringLiteral("hoyolab-map-2"); setup.dataset.maps[0].key.source = setup.analysis.map.source;
            for (auto& marker : setup.dataset.markers) { marker.map.source = setup.analysis.map.source; }
            teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); teyvat::TrackingSession session(store, setup);
            auto pose = teyvat::localize_minimap(frame(0), setup.localization); QVERIFY(pose.position);
            pose.position = teyvat::RecordedPosition{setup.dataset.markers[0].x, setup.dataset.markers[0].y, 4, .99};
            for (int i = 0; i < 3; ++i) { session.observe(pose, {}, i * 200); session.observe_scene(scene(i == 2 ? 1 : 0), i * 200); }
            if (mode == 4) {
                session.observe_scene(scene(1), 800); session.observe_scene(scene(2), 1400); // pose expired; skip this scene
                QVERIFY(store.history(store.active_profile().id).isEmpty());
                for (int i = 0; i < 3; ++i) { session.observe(pose, {}, 1500 + i * 200); session.observe_scene(scene(2), 1500 + i * 200); }
                QCOMPARE(session.view().automatic_changes, 1); continue;
            }
            if (mode >= 5) {
                session.observe(pose, {}, 800); session.observe_scene(scene(2), 800); session.observe_scene(scene(2), 1000);
                session.observe(pose, {}, 900); // result completed after the event, but was captured before it
                QVERIFY(store.history(store.active_profile().id).isEmpty());
                if (mode == 6) { auto lost = pose; lost.position.reset(); lost.reason = teyvat::LocalizationReason::InsufficientFeatures; session.observe(lost, {}, 950); }
                session.observe(pose, {}, 1100); QCOMPARE(session.view().automatic_changes, 1); continue;
            }
            auto missing = pose;
            if (mode == 1) { missing.position->uncertainty = 6; missing.limited_support = true; }
            else { missing.position.reset(); missing.reason = mode == 2 ? teyvat::LocalizationReason::MinimapAbsent : teyvat::LocalizationReason::InsufficientFeatures; }
            session.observe(missing, {}, 600); session.observe_scene(scene(2), 600);
            if (mode != 1) { QVERIFY(!session.view().guidance); }
            QVERIFY(store.history(store.active_profile().id).isEmpty());
            const qint64 resume = mode == 3 ? 2000 : 800;
            for (int i = 0; i < 3; ++i) { session.observe(pose, {}, resume + i * 200); session.observe_scene(scene(2), resume + i * 200); }
            QCOMPARE(session.view().automatic_changes, mode < 2 ? 1 : 0);
            QCOMPARE(store.history(store.active_profile().id).size(), mode < 2 ? 1 : 0);
        }
    }
    void limited_position_keeps_guidance_without_automatic_progress() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); const auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto p = store.active_profile().id;
        teyvat::TrackingSession session(store, setup);
        for (int i = 0; i < 4; ++i) {
            const auto image = frame(i); auto estimate = teyvat::localize_minimap(image, setup.localization); QVERIFY(estimate.position);
            estimate.limited_support = true; estimate.position->uncertainty = setup.analysis.radius * .4;
            session.observe(estimate, image.copy(setup.opening_roi), i * 100);
        }
        QVERIFY(session.view().guidance); QVERIFY(session.view().limited_position); QVERIFY(store.history(p).isEmpty());
        QVERIFY(session.confirm_target()); QCOMPARE(store.history(p).first().after, QStringLiteral("manually_confirmed"));
    }
    void region_filters_and_saved_search_hint_never_create_a_live_pose() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); const auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto p = store.active_profile().id;
        {
            teyvat::TrackingSession session(store, setup); feed(session, 0, 0);
            session.set_allowed_targets(QSet<QString>{QStringLiteral("demo-chest-b")}); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-b"));
            session.set_allowed_targets(QSet<QString>{}); QVERIFY(!session.view().guidance); QVERIFY(!session.confirm_target());
            session.set_allowed_targets(std::nullopt); QVERIFY(session.view().guidance); session.pause();
            QVERIFY(!session.confirm_target()); QVERIFY(store.history(p).isEmpty());
        }
        teyvat::TrackingSession restored(store, setup); QVERIFY(restored.view().last_position); QVERIFY(!restored.view().guidance);
        QVERIFY(!restored.confirm_target()); QVERIFY(store.history(p).isEmpty());
        feed(restored, 0, 0); QVERIFY(restored.view().guidance); QVERIFY(store.history(p).isEmpty());
    }
    void configurable_near_distance_and_manual_collection_advance_without_fake_pose() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        const auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto p = store.active_profile().id;
        teyvat::TrackingSession session(store, setup); QVERIFY(!session.confirm_target()); feed(session, 0, 0);
        auto estimate = teyvat::localize_minimap(frame(0), setup.localization); QVERIFY(estimate.position);
        estimate.position->x = setup.dataset.markers[0].x + 12; estimate.position->y = setup.dataset.markers[0].y;
        session.observe(estimate, {}, 100); QVERIFY(session.view().near_target); QCOMPARE(session.view().state, teyvat::TrackingState::NearTarget);
        session.set_near_distance(5); QVERIFY(!session.view().near_target); session.set_near_distance(20); QVERIFY(session.view().near_target);
        QVERIFY_EXCEPTION_THROWN(session.set_near_distance(0), std::invalid_argument);
        const auto key = session.confirm_target(); QVERIFY(key); QCOMPARE(key->id, std::string("demo-chest-a"));
        QCOMPARE(store.markers(p, setup.analysis.map)[0].status, teyvat::CollectionStatus::ManuallyConfirmed);
        QCOMPARE(session.view().confirmed_changes, 1); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-b"));
        QCOMPARE(store.history_after(p, 0).size(), 1); QVERIFY(store.history_after(p, store.history(p).first().id).isEmpty());
        session.undo(); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-a"));
        const auto count = store.history(p).size(); session.set_remote_collected({QStringLiteral("demo-chest-a")});
        QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-b")); QCOMPARE(store.history(p).size(), count);
        session.set_remote_collected({}); QCOMPARE(store.history(p).size(), count);
        session.pause(); QVERIFY(!session.view().near_target); QVERIFY(!session.view().guidance);
    }
    void complete_visual_loop_advances_saves_and_undoes() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        const auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        QVERIFY(setup.synthetic); QCOMPARE(setup.dataset.markers.size(), 2);
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto profile = store.active_profile().id;
        teyvat::TrackingSession session(store, setup);
        feed(session, 0, 0); QVERIFY(session.view().guidance); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-a"));
        for (int i = 1; i <= 3; ++i) { feed(session, i, i * 100); }
        QCOMPARE(session.view().automatic_changes, 1); QVERIFY(session.view().guidance); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-b"));
        QVERIFY(session.view().guidance->minimap_bearing); QVERIFY(store.can_undo(profile));
        session.undo(); QCOMPARE(store.markers(profile, setup.analysis.map)[0].status, teyvat::CollectionStatus::NotCollected);
        QVERIFY(session.view().guidance); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-a"));
        // A fresh closed/open transition is needed after undo; an open image alone is insufficient.
        feed(session, 3, 400); feed(session, 4, 500); QCOMPARE(store.markers(profile, setup.analysis.map)[0].status, teyvat::CollectionStatus::NotCollected);
        feed(session, 0, 600); feed(session, 1, 700); feed(session, 2, 800); feed(session, 3, 900);
        QCOMPARE(store.markers(profile, setup.analysis.map)[0].status, teyvat::CollectionStatus::Collected);
        for (int i = 5; i <= 9; ++i) { feed(session, i, (i + 5) * 100); }
        QCOMPARE(session.view().automatic_changes, 3); QVERIFY(!session.view().guidance);
        QCOMPARE(session.view().state, teyvat::TrackingState::NoTarget);
        const auto markers = store.markers(profile, setup.analysis.map); QVERIFY(teyvat::is_confirmed(markers[0].status)); QVERIFY(teyvat::is_confirmed(markers[1].status));
        session.pause(); QCOMPARE(session.view().state, teyvat::TrackingState::Paused); QVERIFY(!session.view().guidance);
    }
    void ambiguous_opening_preserves_progress_and_review_until_skip() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        auto nearby = setup.dataset.markers[0]; nearby.id = QStringLiteral("nearby-chest"); nearby.x += 1; setup.dataset.markers.append(nearby);
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto p = store.active_profile().id;
        teyvat::TrackingSession session(store, setup);
        for (int i = 0; i < 5; ++i) { feed(session, i, i * 100); }
        QCOMPARE(session.view().state, teyvat::TrackingState::Review);
        QCOMPARE(session.view().review_reason, std::optional{teyvat::DetectionReason::AmbiguousMarkers});
        QVERIFY(store.history(p).isEmpty()); QCOMPARE(session.view().automatic_changes, 0);
        feed(session, 4, 500); QCOMPARE(session.view().state, teyvat::TrackingState::Review);
        session.skip(); QVERIFY(!session.view().review_reason); QVERIFY(store.history(p).isEmpty());
    }
    void map_without_opening_crop_resets_evidence_and_navigation_continues() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); const auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto profile = store.active_profile().id;
        teyvat::TrackingSession session(store, setup); feed(session, 0, 0); feed(session, 1, 100);
        teyvat::LocalizationResult menu; menu.reason = teyvat::LocalizationReason::MinimapAbsent;
        session.observe(menu, {}, 200); QCOMPARE(session.view().state, teyvat::TrackingState::PositionUnknown);
        feed(session, 2, 300); feed(session, 3, 400); QVERIFY(store.history(profile).isEmpty());
        feed(session, 0, 500); feed(session, 1, 600); feed(session, 2, 700); feed(session, 3, 800);
        QCOMPARE(session.view().automatic_changes, 1); QCOMPARE(store.history(profile).size(), 1);
    }
    void reacquisition_after_teleport_reselects_nearest_without_progress_writes() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); const auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto profile = store.active_profile().id;
        teyvat::TrackingSession session(store, setup); feed(session, 0, 0); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-a"));
        session.reanchor({230, 140}); QVERIFY(!session.view().guidance); QCOMPARE(session.view().last_position, (std::optional<QPointF>{QPointF(230, 140)}));
        feed(session, 5, 200); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-b")); QVERIFY(store.history(profile).isEmpty());
        session.reanchor({229, 140}); feed(session, 5, 400); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-b"));
        QVERIFY_EXCEPTION_THROWN(session.reanchor({-1, -1}), std::invalid_argument); QVERIFY(store.history(profile).isEmpty());
    }
    void skip_restart_profile_isolation_and_lost_position() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); const auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto first = store.active_profile().id;
        { teyvat::TrackingSession session(store, setup); feed(session, 0, 0); session.skip(); QCOMPARE(session.view().guidance->target.id, std::string("demo-chest-b")); }
        teyvat::TrackingSession restored(store, setup); feed(restored, 0, 0); QCOMPARE(restored.view().guidance->target.id, std::string("demo-chest-b"));
        auto weak = teyvat::LocalizationResult{}; restored.observe(weak, frame(0).copy(setup.opening_roi), 100);
        QCOMPARE(restored.view().state, teyvat::TrackingState::PositionUnknown); QVERIFY(!restored.view().guidance);
        feed(restored, 0, 200); QCOMPARE(restored.view().guidance->target.id, std::string("demo-chest-b"));
        const auto other = store.create_profile(QStringLiteral("Other")); store.activate_profile(other);
        feed(restored, 3, 300); QCOMPARE(restored.view().state, teyvat::TrackingState::ProfileChanged); QVERIFY(store.history(first).isEmpty());
        teyvat::TrackingSession second(store, setup); feed(second, 0, 0); QCOMPARE(second.view().guidance->target.id, std::string("demo-chest-a"));
        QVERIFY(store.history(other).isEmpty());
    }
    void locked_target_guard_is_transactional_and_does_not_create_receipt() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); const auto setup = teyvat::load_tracking_setup(example() + QStringLiteral("/session.json"));
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store, setup); const auto p = store.active_profile().id;
        auto analysis = teyvat::analyze_recording(example() + QStringLiteral("/recording.json")); analysis.openings.resize(1);
        const auto rejected = store.apply_detections(p, analysis, QStringLiteral("demo-chest-b"));
        QCOMPARE(rejected[0].reason, teyvat::DetectionReason::TargetMismatch); QVERIFY(store.history(p).isEmpty());
        QCOMPARE(store.apply_detections(p, analysis, QStringLiteral("demo-chest-a"))[0].outcome, teyvat::DetectionOutcome::Applied);
    }
    void setup_rejects_traversal_incompatible_regions_and_supplied_positions() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        for (const auto& name : QDir(example()).entryList(QDir::Files)) { QVERIFY(QFile::copy(QDir(example()).filePath(name), dir.filePath(name))); }
        QFile input(dir.filePath(QStringLiteral("session.json"))); QVERIFY(input.open(QIODevice::ReadOnly)); const auto original = QJsonDocument::fromJson(input.readAll()).object(); input.close();
        const auto write = [&](const QJsonObject& root) { QFile file(input.fileName()); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(root).toJson()); };
        auto bad = original; bad[QStringLiteral("dataset")] = QStringLiteral("../map.json"); write(bad); QVERIFY_EXCEPTION_THROWN(teyvat::load_tracking_setup(input.fileName()), std::exception);
        bad = original; bad[QStringLiteral("position")] = QJsonObject{{QStringLiteral("x"), 100}, {QStringLiteral("y"), 120}}; write(bad); QVERIFY_EXCEPTION_THROWN(teyvat::load_tracking_setup(input.fileName()), std::exception);
        bad = original; bad[QStringLiteral("capture_size")] = QJsonObject{{QStringLiteral("width"), 160}, {QStringLiteral("height"), 100}}; write(bad); QVERIFY_EXCEPTION_THROWN(teyvat::load_tracking_setup(input.fileName()), std::exception);
        auto options = original.value(QStringLiteral("localization")).toObject();
        options[QStringLiteral("method")] = QStringLiteral("orb"); options[QStringLiteral("allow_rotation")] = true;
        bad = original; bad[QStringLiteral("localization")] = options; write(bad);
        QVERIFY(teyvat::load_tracking_setup(input.fileName()).localization.allow_rotation);
        options[QStringLiteral("allow_rotation")] = QStringLiteral("yes"); bad[QStringLiteral("localization")] = options; write(bad);
        QVERIFY_EXCEPTION_THROWN(teyvat::load_tracking_setup(input.fileName()), std::exception);
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); TrackingTests tests;
    try { return QTest::qExec(&tests, argc, argv); }
    catch (const std::exception& error) { qCritical("Tracking test exception: %s", error.what()); return 1; }
}
#include "tracking_tests.moc"
