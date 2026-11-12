#include "teyvat/detection.hpp"
#include "teyvat/store.hpp"
#include "detection_fixtures.hpp"
#include <QCoreApplication>
#include <QProcess>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <functional>
#include <limits>

namespace {
QString scratch() {
    const auto path = QCoreApplication::applicationDirPath() + QStringLiteral("/test-tmp");
    if (!QDir().mkpath(path)) { throw std::runtime_error("Cannot create scratch directory"); }
    return path + QStringLiteral("/detection-XXXXXX");
}
void sql(const QString& path, const std::function<void(QSqlDatabase&)>& action) {
    const auto name = QUuid::createUuid().toString();
    { auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name); db.setDatabaseName(path);
      if (!db.open()) { throw std::runtime_error("Cannot open test database"); }
      try { action(db); } catch (...) { db.close(); db = {}; QSqlDatabase::removeDatabase(name); throw; }
      db.close(); }
    QSqlDatabase::removeDatabase(name);
}
void statement(QSqlDatabase& db, const QString& text) {
    QSqlQuery q(db); if (!q.exec(text)) { throw std::runtime_error(q.lastError().text().toStdString()); }
}
void seed(teyvat::Store& store) {
    const auto data = detection_fixtures::dataset(); const auto p = store.active_profile().id;
    store.apply_import(data, p, false, store.review_import(data, p, false).token);
}
}

class DetectionTests final : public QObject {
    Q_OBJECT
private slots:
    void animation_requires_interaction_burst_dissolution_and_reliable_pose() {
        const auto scene = [](int type) {
            QImage image(256, 256, QImage::Format_ARGB32); image.fill(QColor(25, 35, 45)); QPainter p(&image);
            if (type == 0) { p.fillRect(195, 100, 24, 17, Qt::white); }
            if (type == 1) { p.setPen(Qt::NoPen); p.setBrush(QColor(255, 225, 95)); p.drawEllipse(QPoint(128, 130), 100, 100); }
            if (type == 2) { p.fillRect(160, 100, 60, 80, QColor(30, 180, 240)); }
            return image;
        };
        const teyvat::RecordedPosition pose{100, 120, 1, .99};
        QVERIFY(teyvat::opening_scene_cues(scene(0)).interaction);
        QVERIFY(teyvat::opening_scene_cues(scene(1), true).ring >= .70);
        teyvat::ChestAnimationTracker tracker;
        QVERIFY(!tracker.push(scene(0), 0, pose)); QVERIFY(!tracker.push(scene(0), 500, pose));
        QVERIFY(!tracker.push(scene(1), 1000, pose)); QVERIFY(!tracker.push(scene(2), 1500, pose));
        const auto event = tracker.push(scene(2), 2000, pose); QVERIFY(event); QVERIFY(event->animation); QVERIFY(event->position_stable);
        QVERIFY(!tracker.push(scene(2), 2500, pose)); // one event per opening
        tracker.reset();
        QVERIFY(!tracker.push(scene(1), 0, pose)); QVERIFY(!tracker.push(scene(2), 500, pose)); QVERIFY(!tracker.push(scene(2), 1000, pose));
        tracker.reset(); QVERIFY(!tracker.push(scene(0), 0, pose)); QVERIFY(!tracker.push(scene(0), 500, pose));
        QVERIFY(!tracker.push(scene(1), 1000, pose)); QVERIFY(!tracker.push(scene(2), 1500, {})); QVERIFY(!tracker.push(scene(2), 2000, pose));
        tracker.reset(); QVERIFY(!tracker.push(scene(0), 0, pose)); QVERIFY(!tracker.push(scene(0), 500, pose));
        QVERIFY(!tracker.push(scene(1), 1000, pose)); QVERIFY(!tracker.push(scene(2), 2501, pose)); QVERIFY(!tracker.push(scene(2), 3001, pose));
        for (int field = 0; field < 5; ++field) {
            auto invalid = pose; const auto nan = std::numeric_limits<double>::quiet_NaN();
            if (field == 0) { invalid.x = nan; } else if (field == 1) { invalid.confidence = nan; }
            else if (field == 2) { invalid.uncertainty = nan; } else if (field == 3) { invalid.uncertainty = -1; } else { invalid.confidence = 1.1; }
            tracker.reset(); QVERIFY(!tracker.push(scene(0), 0, pose)); QVERIFY(!tracker.push(scene(0), 500, pose));
            QVERIFY(!tracker.push(scene(1), 1000, pose)); QVERIFY(!tracker.push(scene(2), 1500, invalid)); QVERIFY(!tracker.push(scene(2), 2000, pose));
        }
    }
    void animation_receipts_are_atomic_deduplicated_and_restored_without_faking_template_scores() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        auto analysis = teyvat::analyze_recording(detection_fixtures::bundle(dir.filePath(QStringLiteral("images"))));
        auto& event = analysis.openings.first();
        event.animation = teyvat::OpeningAnimationProof{event.closed_ms + 100, event.opened_ms - 50, .85, .25, .04, 3, 2};
        event.closed_similarity = event.opened_similarity = 0;
        teyvat::validate_analysis(analysis);
        const auto path = dir.filePath(QStringLiteral("companion.db")); teyvat::Store store(path); seed(store); const auto p = store.active_profile().id;
        sql(path, [](QSqlDatabase& db) { statement(db, QStringLiteral("CREATE TRIGGER reject_animation BEFORE INSERT ON animation_receipts BEGIN SELECT RAISE(ABORT,'test failure'); END")); });
        QVERIFY_EXCEPTION_THROWN(store.apply_detections(p, analysis), std::exception);
        QVERIFY(store.history(p).isEmpty()); QCOMPARE(store.markers(p, analysis.map).first().status, teyvat::CollectionStatus::NotCollected);
        sql(path, [](QSqlDatabase& db) { statement(db, QStringLiteral("DROP TRIGGER reject_animation")); });
        QCOMPARE(store.apply_detections(p, analysis).first().outcome, teyvat::DetectionOutcome::Applied);
        QCOMPARE(store.apply_detections(p, analysis).first().outcome, teyvat::DetectionOutcome::Duplicate);
        auto legacy = analysis; legacy.openings.first().animation.reset(); legacy.openings.first().closed_similarity = legacy.openings.first().opened_similarity = .99;
        QCOMPARE(store.apply_detections(p, legacy).first().outcome, teyvat::DetectionOutcome::Duplicate);
        store.backup(dir.filePath(QStringLiteral("saved.db"))); store.undo_last(p);
        store.restore(dir.filePath(QStringLiteral("saved.db")));
        QCOMPARE(store.markers(p, analysis.map).first().status, teyvat::CollectionStatus::Collected);
        QCOMPARE(store.apply_detections(p, analysis).first().outcome, teyvat::DetectionOutcome::Duplicate);
        auto bad = analysis; bad.openings.first().animation->ring_coverage = .5;
        QVERIFY_EXCEPTION_THROWN(store.apply_detections(p, bad), std::exception);
        sql(path, [](QSqlDatabase& db) {
            QSqlQuery q(db); QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM animation_receipts"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 1);
            QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM detection_receipts"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0);
        });
    }
    void actual_v4_upgrade_creates_animation_storage_and_preserves_existing_records() {
        const auto exe = qEnvironmentVariable("TEYVAT_BETA3_EXE");
        if (exe.isEmpty()) { QSKIP("Set TEYVAT_BETA3_EXE to verify the actual beta.3 database migration."); }
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); QProcess old;
        old.start(exe, {QStringLiteral("--data-dir"), dir.path(), QStringLiteral("--check-storage")});
        QVERIFY(old.waitForFinished(15000)); QCOMPARE(old.exitCode(), 0);
        const auto path = dir.filePath(QStringLiteral("companion.db"));
        sql(path, [](QSqlDatabase& db) { QSqlQuery q(db); QVERIFY(q.exec(QStringLiteral("PRAGMA user_version"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 4); });
        { teyvat::Store store(path); seed(store); QVERIFY(!store.profiles().isEmpty()); }
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.before-v4-to-v5-*.bak")}, QDir::Files).size(), 1);
        sql(path, [](QSqlDatabase& db) { QSqlQuery q(db); QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM animation_receipts"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0); });
        const auto broken = dir.filePath(QStringLiteral("broken")); old.start(exe, {QStringLiteral("--data-dir"), broken, QStringLiteral("--check-storage")});
        QVERIFY(old.waitForFinished(15000)); QCOMPARE(old.exitCode(), 0); const auto broken_path = QDir(broken).filePath(QStringLiteral("companion.db"));
        sql(broken_path, [](QSqlDatabase& db) { statement(db, QStringLiteral("CREATE TABLE animation_receipts(forced_conflict TEXT)")); });
        QVERIFY_EXCEPTION_THROWN(teyvat::Store{broken_path}, std::exception);
        sql(broken_path, [](QSqlDatabase& db) { QSqlQuery q(db); QVERIFY(q.exec(QStringLiteral("PRAGMA user_version"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 4); });
    }
    void held_out_synthetic_sequences_and_example() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        int tp = 0, fp = 0, fn = 0, tn = 0;
        for (int i = 0; i < 32; ++i) {
            const auto root = detection_fixtures::make_bundle(dir.path(), {0, 0, 1, 1, 1, 1}, 200 + i, 2 + i % 7);
            const auto result = teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root));
            if (result.openings.size() == 1) { ++tp; } else { ++fn; }
        }
        const QVector<QVector<int>> negatives{{2, 2, 2, 2, 2}, {1, 1, 1, 1, 1}, {0, 0, 0, 0, 0},
            {0, 0, 2, 2, 2}, {0, 1, 0, 1, 0}, {0, 0, 4, 4, 4}, {0, 0, 5, 5, 5}, {3, 3, 3, 3, 3}};
        for (int i = 0; i < 32; ++i) {
            const auto root = detection_fixtures::make_bundle(dir.path(), negatives[i % negatives.size()], 400 + i, 2 + i % 7);
            const auto result = teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root));
            if (result.openings.isEmpty()) { ++tn; } else { ++fp; }
        }
        qInfo("SYNTHETIC ONLY: TP=%d FN=%d FP=%d TN=%d; not a game-accuracy measurement", tp, fn, fp, tn);
        QCOMPARE(tp, 32); QCOMPARE(fn, 0); QCOMPARE(fp, 0); QCOMPARE(tn, 32);
        const auto output = qEnvironmentVariable("TEYVAT_AUTOMATION_EXAMPLE_DIR");
        if (!output.isEmpty()) {
            const auto root = detection_fixtures::make_bundle(output, {0, 0, 1, 1, 1, 0, 0, 1, 1, 1}, 42, 3, true);
            const auto example = teyvat::analyze_recording(detection_fixtures::save_bundle(output, root));
            QCOMPARE(example.openings.size(), 2);
            detection_fixtures::write(QDir(output).filePath(QStringLiteral("map.json")), teyvat::encode_json(detection_fixtures::dataset()));
        }
    }
    void input_limits_temporal_and_position_guards() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        const auto original = detection_fixtures::make_bundle(dir.path());
        auto root = original; root[QStringLiteral("closed_template")] = QStringLiteral("../closed.png");
        QVERIFY_EXCEPTION_THROWN(teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root)), std::exception);
        root = original; root[QStringLiteral("opened_template")] = QStringLiteral("closed.png");
        QVERIFY_EXCEPTION_THROWN(teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root)), std::exception);
        root = original; auto frames = root[QStringLiteral("frames")].toArray(); auto frame = frames[2].toObject(); frame[QStringLiteral("time_ms")] = 100; frames[2] = frame; root[QStringLiteral("frames")] = frames;
        QVERIFY_EXCEPTION_THROWN(teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root)), std::exception);
        root = original; frames = root[QStringLiteral("frames")].toArray();
        for (int i = 2; i < frames.size(); ++i) { frame = frames[i].toObject(); frame[QStringLiteral("time_ms")] = i * 100 + 2000; frames[i] = frame; }
        root[QStringLiteral("frames")] = frames;
        QVERIFY(teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root)).openings.isEmpty());
        root = original; frames = root[QStringLiteral("frames")].toArray();
        for (int i = 0; i < frames.size(); ++i) { frame = frames[i].toObject(); frame.remove(QStringLiteral("position")); frames[i] = frame; }
        root[QStringLiteral("frames")] = frames;
        const auto unknown = teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root)); QCOMPARE(unknown.openings.size(), 1);
        const auto data = detection_fixtures::dataset();
        QCOMPARE(teyvat::match_opening(unknown.openings.first(), unknown, data.maps.first(), data.markers).reason, teyvat::DetectionReason::NoPosition);
        root = original; frames = root[QStringLiteral("frames")].toArray();
        for (int i = 2; i < frames.size(); ++i) { frame = frames[i].toObject(); auto p = frame[QStringLiteral("position")].toObject(); p[QStringLiteral("x")] = 350; p[QStringLiteral("y")] = 150; frame[QStringLiteral("position")] = p; frames[i] = frame; }
        root[QStringLiteral("frames")] = frames;
        const auto jump = teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root));
        QVERIFY(!jump.openings.first().position_stable);
        QCOMPARE(teyvat::match_opening(jump.openings.first(), jump, data.maps.first(), data.markers).outcome, teyvat::DetectionOutcome::Review);
        root = detection_fixtures::make_bundle(dir.path(), {0, 0, 3, 0, 1, 1});
        // An interrupted closed sequence must not reuse an earlier armed state
        // after observing only one closed frame of a potentially different object.
        QVERIFY(teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root)).openings.isEmpty());
        root = detection_fixtures::make_bundle(dir.path(), {0, 0, 3, 1, 1});
        frames = root[QStringLiteral("frames")].toArray(); frame = frames[2].toObject(); frame.remove(QStringLiteral("position")); frames[2] = frame; root[QStringLiteral("frames")] = frames;
        const auto lost = teyvat::analyze_recording(detection_fixtures::save_bundle(dir.path(), root)); QCOMPARE(lost.openings.size(), 1);
        QVERIFY(!lost.openings.first().position_stable);
        QCOMPARE(teyvat::match_opening(lost.openings.first(), lost, data.maps.first(), data.markers).outcome, teyvat::DetectionOutcome::Review);
    }
    void automatic_progress_undo_restart_profiles_and_restore() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        const auto manifest = detection_fixtures::bundle(dir.filePath(QStringLiteral("images")));
        const auto analysis = teyvat::analyze_recording(manifest);
        { QFile file(manifest); QVERIFY(file.open(QIODevice::ReadOnly));
          const auto compact = QJsonDocument::fromJson(file.readAll()).toJson(QJsonDocument::Compact); file.close();
          detection_fixtures::write(manifest, compact);
          QCOMPARE(teyvat::analyze_recording(manifest).fingerprint, analysis.fingerprint); }
        const auto path = dir.filePath(QStringLiteral("companion.db")); QString profile;
        const auto layer = detection_fixtures::dataset().maps.first().key;
        {
            teyvat::Store store(path); seed(store); profile = store.active_profile().id;
            QCOMPARE(store.apply_detections(profile, analysis).first().outcome, teyvat::DetectionOutcome::Applied);
            QCOMPARE(store.markers(profile, layer).first().status, teyvat::CollectionStatus::Collected);
            QCOMPARE(store.history(profile).first().origin, QStringLiteral("detection"));
            QCOMPARE(store.apply_detections(profile, analysis).first().outcome, teyvat::DetectionOutcome::Duplicate);
            QCOMPARE(store.history(profile).size(), 1);
            store.undo_last(profile); QCOMPARE(store.markers(profile, layer).first().status, teyvat::CollectionStatus::NotCollected);
            QCOMPARE(store.apply_detections(profile, analysis).first().outcome, teyvat::DetectionOutcome::Duplicate);
            const auto second = store.create_profile(QStringLiteral("Another traveler"));
            QCOMPARE(store.apply_detections(second, analysis).first().outcome, teyvat::DetectionOutcome::Applied);
            QCOMPARE(store.markers(profile, layer).first().status, teyvat::CollectionStatus::NotCollected);
            store.backup(dir.filePath(QStringLiteral("snapshot.db")));
        }
        {
            teyvat::Store store(path);
            QCOMPARE(store.apply_detections(profile, analysis).first().outcome, teyvat::DetectionOutcome::Duplicate);
            store.set_status(profile, {store.markers(profile, layer).first()}, teyvat::CollectionStatus::Collected);
            store.restore(dir.filePath(QStringLiteral("snapshot.db")));
            QCOMPARE(store.markers(profile, layer).first().status, teyvat::CollectionStatus::NotCollected);
            QCOMPARE(store.apply_detections(profile, analysis).first().outcome, teyvat::DetectionOutcome::Duplicate);
        }
    }
    void ambiguity_stale_markers_and_wrong_frame_do_not_write() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        auto analysis = teyvat::analyze_recording(detection_fixtures::bundle(dir.filePath(QStringLiteral("images"))));
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); seed(store); const auto p = store.active_profile().id;
        auto data = detection_fixtures::dataset(); auto second = data.markers[1]; second.x = 103; second.y = 120; store.save_marker(p, second);
        QCOMPARE(store.apply_detections(p, analysis).first().reason, teyvat::DetectionReason::AmbiguousMarkers); QVERIFY(store.history(p).isEmpty());
        store.set_retired({second}, true);
        QCOMPARE(store.apply_detections(p, analysis).first().reason, teyvat::DetectionReason::AmbiguousMarkers); QVERIFY(store.history(p).isEmpty());
        second.x = 350; second.y = 150; second.retired = false; store.save_marker(p, second);
        auto first = data.markers[0]; first.x = 200; store.save_marker(p, first);
        QCOMPARE(store.apply_detections(p, analysis).first().outcome, teyvat::DetectionOutcome::Unmatched); QVERIFY(store.history(p).isEmpty());
        analysis.frame = QStringLiteral("wrong-frame");
        QVERIFY_EXCEPTION_THROWN(store.apply_detections(p, analysis), std::exception); QVERIFY(store.history(p).isEmpty());
    }
    void receipt_failure_rolls_back_progress_and_history() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        const auto analysis = teyvat::analyze_recording(detection_fixtures::bundle(dir.filePath(QStringLiteral("images"))));
        const auto path = dir.filePath(QStringLiteral("companion.db")); teyvat::Store store(path); seed(store); const auto p = store.active_profile().id;
        sql(path, [](QSqlDatabase& db) { statement(db, QStringLiteral("CREATE TRIGGER reject_detection BEFORE INSERT ON detection_receipts BEGIN SELECT RAISE(ABORT,'test failure'); END")); });
        QVERIFY_EXCEPTION_THROWN(store.apply_detections(p, analysis), std::exception);
        QVERIFY(store.history(p).isEmpty()); QCOMPARE(store.markers(p, analysis.map).first().status, teyvat::CollectionStatus::NotCollected);
        sql(path, [](QSqlDatabase& db) { statement(db, QStringLiteral("DROP TRIGGER reject_detection")); });
        QCOMPARE(store.apply_detections(p, analysis).first().outcome, teyvat::DetectionOutcome::Applied);
    }
    void actual_v3_upgrade_preserves_undo() {
        const auto exe = qEnvironmentVariable("TEYVAT_PHASE3_EXE");
        if (exe.isEmpty()) { QSKIP("Set TEYVAT_PHASE3_EXE to verify the actual previous preview upgrade."); }
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        QProcess old; old.start(exe, {QStringLiteral("--data-dir"), dir.path(), QStringLiteral("--check-storage")});
        QVERIFY(old.waitForFinished(15000)); QCOMPARE(old.exitCode(), 0);
        const auto path = dir.filePath(QStringLiteral("companion.db"));
        sql(path, [](QSqlDatabase& db) {
            statement(db, QStringLiteral("INSERT INTO map_regions VALUES('legacy','region','Region')"));
            statement(db, QStringLiteral("INSERT INTO map_layers(source,region,layer,title,frame,width,height,provenance,license,data_version) VALUES('legacy','region','surface','Map','v1',1000,1000,'Own','MIT','v1')"));
            statement(db, QStringLiteral("INSERT INTO marker_categories VALUES('legacy','common')"));
            statement(db, QStringLiteral("INSERT INTO markers VALUES('legacy','chest','region','surface','common','Chest','',10,10,0,'manual')"));
            statement(db, QStringLiteral("INSERT INTO user_progress SELECT id,'legacy','chest','not_collected',2,'2026-11-08T12:00:00Z' FROM profiles"));
            statement(db, QStringLiteral("INSERT INTO detection_history SELECT 1,id,'legacy','chest','not_collected','collected',0,1,'legacy-a','manual','2026-11-08T11:00:00Z',NULL FROM profiles"));
            statement(db, QStringLiteral("INSERT INTO detection_history SELECT 2,id,'legacy','chest','collected','not_collected',1,2,'legacy-b','undo','2026-11-08T12:00:00Z',1 FROM profiles"));
        });
        teyvat::Store store(path); const auto p = store.active_profile().id;
        QCOMPARE(store.history(p).size(), 2); QVERIFY(!store.can_undo(p));
        const teyvat::LayerKey layer{QStringLiteral("legacy"), QStringLiteral("region"), QStringLiteral("surface")};
        store.set_status(p, store.markers(p, layer), teyvat::CollectionStatus::PossiblyCollected);
        store.undo_last(p); QCOMPARE(store.markers(p, layer).first().status, teyvat::CollectionStatus::NotCollected);
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.before-v3-to-v%1-*.bak").arg(teyvat::Store::schema_version)}, QDir::Files).size(), 1);
        const auto broken_dir = dir.filePath(QStringLiteral("migration-failure"));
        old.start(exe, {QStringLiteral("--data-dir"), broken_dir, QStringLiteral("--check-storage")});
        QVERIFY(old.waitForFinished(15000)); QCOMPARE(old.exitCode(), 0);
        const auto broken = QDir(broken_dir).filePath(QStringLiteral("companion.db"));
        sql(broken, [](QSqlDatabase& db) { statement(db, QStringLiteral("CREATE TABLE detection_receipts(forced_conflict TEXT)")); });
        QVERIFY_EXCEPTION_THROWN(teyvat::Store{broken}, std::exception);
        sql(broken, [](QSqlDatabase& db) {
            QSqlQuery q(db); QVERIFY(q.exec(QStringLiteral("PRAGMA user_version"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 3);
            QVERIFY(q.exec(QStringLiteral("SELECT sql FROM sqlite_master WHERE name='detection_history'"))); QVERIFY(q.next());
            QVERIFY(!q.value(0).toString().contains(QStringLiteral("'detection'")));
            QVERIFY(q.exec(QStringLiteral("PRAGMA integrity_check"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(), QStringLiteral("ok"));
        });
    }
};
QTEST_GUILESS_MAIN(DetectionTests)
#include "detection_tests.moc"
