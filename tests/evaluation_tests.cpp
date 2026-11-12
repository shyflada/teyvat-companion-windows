#include "teyvat/evaluation.hpp"
#include "localization_fixtures.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {
QString scratch() {
    const auto root = QCoreApplication::applicationDirPath() + QStringLiteral("/test-tmp");
    if (!QDir().mkpath(root)) { throw std::runtime_error("Cannot create test directory"); } return root + QStringLiteral("/evaluation-XXXXXX");
}
QByteArray bytes(const QString& path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) { throw std::runtime_error("Cannot read fixture"); } return f.readAll(); }
}
class EvaluationTests final : public QObject {
    Q_OBJECT
private slots:
    void correct_and_intentionally_wrong_labels_are_measured() {
        QTemporaryDir dir(scratch()); const auto path = localization_fixtures::bundle(dir.path());
        const auto analysis = teyvat::analyze_recording(path); const auto data = localization_fixtures::dataset();
        const auto labels = bytes(dir.filePath(QStringLiteral("labels.json")));
        const auto report = teyvat::evaluate_recording(analysis, data, labels);
        QCOMPARE(report[QStringLiteral("positions")].toObject()[QStringLiteral("within_tolerance")].toInt(), 10);
        QCOMPARE(report[QStringLiteral("events")].toObject()[QStringLiteral("correct_automatic_markers_on_matched_events")].toInt(), 2);
        auto root = QJsonDocument::fromJson(labels).object(); auto points = root[QStringLiteral("positions")].toArray();
        auto first = points[0].toObject(); first[QStringLiteral("position")] = QJsonValue::Null; points[0] = first;
        auto second = points[1].toObject(); second[QStringLiteral("position")] = QJsonObject{{QStringLiteral("x"), 140}, {QStringLiteral("y"), 120}}; points[1] = second; root[QStringLiteral("positions")] = points;
        auto events = root[QStringLiteral("openings")].toArray(); auto event = events[0].toObject(); event[QStringLiteral("marker_id")] = QStringLiteral("demo-chest-b"); events[0] = event; root[QStringLiteral("openings")] = events;
        const auto wrong = teyvat::evaluate_recording(analysis, data, QJsonDocument(root).toJson());
        QCOMPARE(wrong[QStringLiteral("positions")].toObject()[QStringLiteral("unexpected_accepts_for_unknown_labels")].toInt(), 1);
        QCOMPARE(wrong[QStringLiteral("positions")].toObject()[QStringLiteral("max_error")].toDouble(), 40.0);
        QCOMPARE(wrong[QStringLiteral("events")].toObject()[QStringLiteral("wrong_automatic_markers_on_matched_events")].toInt(), 1);
        root = QJsonDocument::fromJson(labels).object(); events = root[QStringLiteral("openings")].toArray(); event = events[0].toObject(); event[QStringLiteral("time_ms")] = 0; events[0] = event; root[QStringLiteral("openings")] = events;
        const auto shifted = teyvat::evaluate_recording(analysis, data, QJsonDocument(root).toJson())[QStringLiteral("events")].toObject();
        QCOMPARE(shifted[QStringLiteral("false_positives")].toInt(), 1); QCOMPARE(shifted[QStringLiteral("false_negatives")].toInt(), 1);
        QCOMPARE(shifted[QStringLiteral("automatic_markers_on_unmatched_events")].toInt(), 1);
    }
    void maximum_temporal_matching_and_empty_denominators() {
        QTemporaryDir dir(scratch()); auto analysis = teyvat::analyze_recording(localization_fixtures::bundle(dir.path()));
        auto root = QJsonDocument::fromJson(bytes(dir.filePath(QStringLiteral("labels.json")))).object();
        auto events = root[QStringLiteral("openings")].toArray(); auto first = events[0].toObject(), second = events[1].toObject();
        first[QStringLiteral("time_ms")] = 100; second[QStringLiteral("time_ms")] = 300; events[0] = first; events[1] = second;
        root[QStringLiteral("openings")] = events; root[QStringLiteral("event_tolerance_ms")] = 200;
        analysis.openings[0].closed_ms = 100; analysis.openings[0].opened_ms = 290;
        analysis.openings[1].closed_ms = 300; analysis.openings[1].opened_ms = 390;
        for (auto& event : analysis.openings) { event.event_id = QCryptographicHash::hash(analysis.fingerprint + QByteArray::number(event.closed_ms) + ':' + QByteArray::number(event.opened_ms), QCryptographicHash::Sha256); }
        auto report = teyvat::evaluate_recording(analysis, localization_fixtures::dataset(), QJsonDocument(root).toJson());
        QCOMPARE(report[QStringLiteral("events")].toObject()[QStringLiteral("true_positives")].toInt(), 2);
        analysis.openings.clear(); root[QStringLiteral("openings")] = QJsonArray{};
        report = teyvat::evaluate_recording(analysis, localization_fixtures::dataset(), QJsonDocument(root).toJson());
        QVERIFY(report[QStringLiteral("events")].toObject()[QStringLiteral("precision")].isNull());
        QVERIFY(report[QStringLiteral("events")].toObject()[QStringLiteral("recall")].isNull());
    }
    void malformed_or_incomplete_labels_are_rejected() {
        QTemporaryDir dir(scratch()); const auto analysis = teyvat::analyze_recording(localization_fixtures::bundle(dir.path())); const auto data = localization_fixtures::dataset();
        QVERIFY_EXCEPTION_THROWN((void)teyvat::evaluate_recording(analysis, data, QByteArrayLiteral("{}")), std::exception);
        auto root = QJsonDocument::fromJson(bytes(dir.filePath(QStringLiteral("labels.json")))).object(); auto positions = root[QStringLiteral("positions")].toArray(); positions.removeLast(); root[QStringLiteral("positions")] = positions;
        QVERIFY_EXCEPTION_THROWN((void)teyvat::evaluate_recording(analysis, data, QJsonDocument(root).toJson()), std::exception);
    }
    void command_line_creates_report_preserves_inputs_and_refuses_overwrite() {
        QTemporaryDir dir(scratch()); const auto recording = localization_fixtures::bundle(dir.path());
        const auto original = bytes(recording), original_labels = bytes(dir.filePath(QStringLiteral("labels.json")));
        const auto output = dir.filePath(QStringLiteral("report.json"));
        QProcess process; const auto exe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("TeyvatEvaluate.exe"));
        const QStringList args{QStringLiteral("--recording"), recording, QStringLiteral("--dataset"), dir.filePath(QStringLiteral("map.json")), QStringLiteral("--labels"), dir.filePath(QStringLiteral("labels.json")), QStringLiteral("--output"), output};
        process.start(exe, args); QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        const auto report = bytes(output); QCOMPARE(QJsonDocument::fromJson(report).object()[QStringLiteral("events")].toObject()[QStringLiteral("true_positives")].toInt(), 2);
        process.start(exe, args); QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 1); QCOMPARE(bytes(output), report);
        QCOMPARE(bytes(recording), original); QCOMPARE(bytes(dir.filePath(QStringLiteral("labels.json"))), original_labels);
        QVERIFY(QDir(dir.path()).entryList({QStringLiteral("*.db")}, QDir::Files).isEmpty());
    }
};
QTEST_GUILESS_MAIN(EvaluationTests)
#include "evaluation_tests.moc"
