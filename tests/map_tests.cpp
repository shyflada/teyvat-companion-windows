#include "teyvat/store.hpp"
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <limits>
#include <stdexcept>

namespace {
QString temporary_pattern() {
    const auto root = QCoreApplication::applicationDirPath() + QStringLiteral("/test-tmp");
    QDir().mkpath(root); return root + QStringLiteral("/map-XXXXXX");
}
teyvat::Dataset fixture(int count = 3) {
    teyvat::MapLayer layer{{QStringLiteral("test-owned"), QStringLiteral("Test region"), QStringLiteral("Surface")},
        QStringLiteral("Synthetic test grid"), QStringLiteral("test:cartesian-v1")};
    teyvat::Dataset data{{layer}, {}};
    for (int i = 0; i < count; ++i) {
        data.markers.append({layer.key, QStringLiteral("test-%1").arg(i), QStringLiteral("common"),
            QStringLiteral("Тестова скриня %1").arg(i), QStringLiteral("Note, \"quoted\"\r\nДругий рядок"),
            static_cast<double>(i % 1000), static_cast<double>(i % 700)});
    }
    return data;
}
void import(teyvat::Store& store, const teyvat::Dataset& data, bool overwrite = false) {
    const auto profile = store.active_profile().id;
    store.apply_import(data, profile, overwrite, store.review_import(data, profile, overwrite).token);
}
QByteArray png() {
    QImage image(16, 12, QImage::Format_RGB32); image.fill(QColor(45, 70, 80));
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return bytes;
}
QByteArray file_bytes(const QString& path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) { throw std::runtime_error("Cannot read test file"); } return f.readAll(); }
} // namespace

class MapTests final : public QObject {
    Q_OBJECT
private slots:
    void json_csv_unicode_formula_round_trip() {
        auto data = fixture();
        data.maps[0].title = QStringLiteral("=2+3");
        data.markers[0].label = QStringLiteral("'=SUM(A1:A2)");
        data.markers[1].label = QStringLiteral("@unsafe");
        data.markers[2].notes = QStringLiteral("\t=HYPERLINK(\"x\")\nnotes");
        QVERIFY(teyvat::decode_json(teyvat::encode_json(data)) == data);
        const auto csv = teyvat::encode_csv(data);
        QVERIFY(csv.contains("\"'@unsafe\""));
        QVERIFY(csv.contains("\"''=SUM(A1:A2)\""));
        QVERIFY(teyvat::decode_csv(csv) == data);
        QVERIFY(teyvat::decode_json(QByteArray("\xef\xbb\xbf") + teyvat::encode_json(data)) == data);
    }

    void malformed_input_and_bounds() {
        const auto valid = fixture();
        auto bad = valid; bad.markers.append(bad.markers.first());
        QVERIFY_EXCEPTION_THROWN(teyvat::validate_dataset(bad), std::runtime_error);
        bad = valid; bad.markers[0].x = std::numeric_limits<double>::infinity();
        QVERIFY_EXCEPTION_THROWN(teyvat::validate_dataset(bad), std::runtime_error);
        bad = valid; bad.markers[0].y = 1001;
        QVERIFY_EXCEPTION_THROWN(teyvat::validate_dataset(bad), std::runtime_error);
        bad = valid; bad.markers[0].map.layer = QStringLiteral("Underground");
        QVERIFY_EXCEPTION_THROWN(teyvat::validate_dataset(bad), std::runtime_error);
        bad = valid; bad.maps[0].license.clear();
        QVERIFY_EXCEPTION_THROWN(teyvat::validate_dataset(bad), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_json(QByteArray("\xff")), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_json(QByteArray(teyvat::max_transfer_bytes + 1, 'a')), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_json("{\"version\":999}"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_csv("bad,header\n"), std::runtime_error);
        auto csv = teyvat::encode_csv(valid); csv.chop(3);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_csv(csv), std::runtime_error);
        auto json = teyvat::encode_json(valid); json.replace("not_collected", "bogus_status_");
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_json(json), std::runtime_error);
    }

    void map_progress_profile_restart_and_repeated_undo() {
        QTemporaryDir dir(temporary_pattern()); const auto path = dir.filePath(QStringLiteral("companion.db"));
        auto data = fixture(); QString first, second;
        {
            teyvat::Store store(path); first = store.active_profile().id; import(store, data);
            auto markers = store.markers(first, data.maps[0].key);
            store.set_status(first, markers, teyvat::CollectionStatus::ManuallyConfirmed);
            QCOMPARE(store.history(first).size(), 3);
            store.set_status(first, markers, teyvat::CollectionStatus::ManuallyConfirmed);
            QCOMPARE(store.history(first).size(), 3); // No duplicate events.
            store.set_status(first, markers, teyvat::CollectionStatus::PossiblyCollected);
            store.undo_last(first);
            QVERIFY(store.markers(first, data.maps[0].key)[0].status == teyvat::CollectionStatus::ManuallyConfirmed);
            store.undo_last(first);
            QVERIFY(store.markers(first, data.maps[0].key)[0].status == teyvat::CollectionStatus::NotCollected);
            QVERIFY(!store.can_undo(first));
            second = store.create_profile(QStringLiteral("Other"));
            store.set_status(second, markers, teyvat::CollectionStatus::Collected);
            QVERIFY(store.markers(first, data.maps[0].key)[0].status == teyvat::CollectionStatus::NotCollected);
            store.set_retired(markers, true);
        }
        teyvat::Store reopened(path);
        QCOMPARE(reopened.active_profile().id, second);
        auto markers = reopened.markers(second, data.maps[0].key);
        QVERIFY(markers[0].retired);
        QVERIFY(markers[0].status == teyvat::CollectionStatus::Collected);
        reopened.set_retired(markers, false);
        QVERIFY(!reopened.markers(second, data.maps[0].key)[0].retired);
    }

    void preview_conflicts_retention_and_atomicity() {
        QTemporaryDir dir(temporary_pattern()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        auto data = fixture(); import(store, data); const auto profile = store.active_profile().id;
        store.set_status(profile, {data.markers[0]}, teyvat::CollectionStatus::ManuallyConfirmed);
        data.markers[0].label = QStringLiteral("Updated label");
        data.markers[0].status = teyvat::CollectionStatus::Unknown;
        auto preview = store.review_import(data, profile, false);
        QCOMPARE(preview.updated_markers, 1); QCOMPARE(preview.progress_changes, 0);
        store.apply_import(data, profile, false, preview.token);
        QVERIFY(store.markers(profile, data.maps[0].key)[0].status == teyvat::CollectionStatus::ManuallyConfirmed);
        preview = store.review_import(data, profile, true); QCOMPARE(preview.progress_changes, 1);
        store.set_status(profile, {data.markers[1]}, teyvat::CollectionStatus::Collected);
        QVERIFY_EXCEPTION_THROWN(store.apply_import(data, profile, true, preview.token), std::runtime_error);
        import(store, data, true);
        store.undo_last(profile);
        QVERIFY(store.markers(profile, data.maps[0].key)[0].status == teyvat::CollectionStatus::ManuallyConfirmed);
        auto missing = data.markers[0]; missing.id = QStringLiteral("not-existing");
        const auto before = teyvat::encode_json(store.export_layer(profile, data.maps[0].key));
        const auto history_count = store.history(profile).size();
        QVERIFY_EXCEPTION_THROWN(store.set_status(profile, {data.markers[2], missing}, teyvat::CollectionStatus::Collected), std::runtime_error);
        QCOMPARE(teyvat::encode_json(store.export_layer(profile, data.maps[0].key)), before);
        QCOMPARE(store.history(profile).size(), history_count);
        auto shifted = data; shifted.maps[0].frame = QStringLiteral("different-scale-v2");
        QVERIFY_EXCEPTION_THROWN((void)store.review_import(shifted, profile, false), std::runtime_error);
    }

    void background_backup_restore_and_invalid_restore() {
        QTemporaryDir dir(temporary_pattern()); const auto path = dir.filePath(QStringLiteral("companion.db"));
        teyvat::Store store(path); auto data = fixture(); import(store, data);
        const auto profile = store.active_profile().id;
        store.set_background(data.maps[0].key, png(), QStringLiteral("Generated fixture"), QStringLiteral("MIT"));
        QVERIFY(!store.background(data.maps[0].key).isEmpty());
        QVERIFY_EXCEPTION_THROWN((void)teyvat::decode_background("not png"), std::runtime_error);
        store.set_status(profile, {data.markers[0]}, teyvat::CollectionStatus::Collected);
        const auto snapshot = dir.filePath(QStringLiteral("snapshot.db"));
        store.backup(snapshot); const auto original_snapshot = file_bytes(snapshot);
        QVERIFY_EXCEPTION_THROWN(store.backup(snapshot), std::runtime_error);
        store.rename_profile(profile, QStringLiteral("Changed after backup"));
        store.set_status(profile, data.markers, teyvat::CollectionStatus::Unknown);
        const auto stale = store.review_import(data, profile, false);
        store.restore(snapshot);
        QCOMPARE(store.active_profile().name, QStringLiteral("Traveler"));
        QVERIFY(store.markers(profile, data.maps[0].key)[0].status == teyvat::CollectionStatus::Collected);
        QVERIFY(!store.background(data.maps[0].key).isEmpty());
        QCOMPARE(file_bytes(snapshot), original_snapshot);
        QVERIFY_EXCEPTION_THROWN(store.apply_import(data, profile, false, stale.token), std::runtime_error);
        QFile corrupt(dir.filePath(QStringLiteral("bad.db"))); QVERIFY(corrupt.open(QIODevice::WriteOnly)); corrupt.write("bad database"); corrupt.close();
        const auto before = teyvat::encode_json(store.export_layer(profile, data.maps[0].key));
        QVERIFY_EXCEPTION_THROWN(store.restore(corrupt.fileName()), std::runtime_error);
        QCOMPARE(teyvat::encode_json(store.export_layer(profile, data.maps[0].key)), before);
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.before-restore-*.bak")}, QDir::Files).size(), 1);
    }

    void import_sql_failure_rolls_back_every_table() {
        QTemporaryDir dir(temporary_pattern()); const auto path = dir.filePath(QStringLiteral("companion.db"));
        teyvat::Store store(path); const auto profile = store.active_profile().id;
        const auto connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection); db.setDatabaseName(path); QVERIFY(db.open());
            { QSqlQuery q(db); QVERIFY(q.exec(QStringLiteral("CREATE TRIGGER test_reject BEFORE INSERT ON markers WHEN NEW.id='test-2' BEGIN SELECT RAISE(ABORT,'test rollback'); END"))); }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        auto data = fixture(); data.markers[0].status = teyvat::CollectionStatus::Collected;
        const auto review = store.review_import(data, profile, true);
        QVERIFY_EXCEPTION_THROWN(store.apply_import(data, profile, true, review.token), std::runtime_error);
        QVERIFY(store.layers().isEmpty()); QVERIFY(store.history(profile).isEmpty());
        QCOMPARE(store.profiles().size(), 1); QCOMPARE(store.active_profile().id, profile);
    }

    void bounded_5000_marker_transfer() {
        QTemporaryDir dir(temporary_pattern()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        const auto data = fixture(5000); import(store, data);
        QCOMPARE(store.markers(store.active_profile().id, data.maps[0].key).size(), 5000);
        const auto csv = teyvat::encode_csv(data);
        QVERIFY(teyvat::decode_csv(csv) == data);
        store.set_status(store.active_profile().id, data.markers, teyvat::CollectionStatus::ManuallyConfirmed);
        store.undo_last(store.active_profile().id);
        QVERIFY(store.markers(store.active_profile().id, data.maps[0].key)[4999].status == teyvat::CollectionStatus::NotCollected);
        const auto page = store.history(store.active_profile().id, 250); QCOMPARE(page.size(), 250);
        const auto older = store.history(store.active_profile().id, 250, page.last().id); QCOMPARE(older.size(), 250);
        QVERIFY(older.first().id < page.last().id);
    }
};
QTEST_GUILESS_MAIN(MapTests)
#include "map_tests.moc"
