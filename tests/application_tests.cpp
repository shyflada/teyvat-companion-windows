#include "teyvat/capture_recorder.hpp"
#include "teyvat/hoyolab_sync.hpp"
#include "teyvat/main_window.hpp"
#include "teyvat/map_page.hpp"
#include "teyvat/store.hpp"
#include "detection_fixtures.hpp"
#include "localization_fixtures.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QUuid>
#include <cstdlib>
#include <functional>
#include <stdexcept>

namespace {

QString test_temp_template() {
    const auto root = QCoreApplication::applicationDirPath() + QStringLiteral("/test-tmp");
    if (!QDir().mkpath(root)) { throw std::runtime_error("Cannot create test scratch directory"); }
    return root + QStringLiteral("/case-XXXXXX");
}

void sql(const QSqlDatabase& db, const QString& statement) {
    QSqlQuery q(db);
    if (!q.exec(statement)) { throw std::runtime_error(q.lastError().text().toStdString()); }
}

void with_sql(const QString& path, const std::function<void(QSqlDatabase&)>& operation) {
    const auto name = QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (!db.open()) { throw std::runtime_error(db.lastError().text().toStdString()); }
        try { operation(db); } catch (...) {
            db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase(name); throw;
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
}

void legacy_database(const QString& path) {
    with_sql(path, [](QSqlDatabase& db) {
        sql(db, QStringLiteral("CREATE TABLE profiles(id TEXT PRIMARY KEY,name TEXT,name_key TEXT UNIQUE,created_at TEXT)"));
        sql(db, QStringLiteral("CREATE TABLE settings(profile_id TEXT PRIMARY KEY REFERENCES profiles(id),language TEXT,theme TEXT,ui_scale INTEGER)"));
        sql(db, QStringLiteral("INSERT INTO profiles VALUES('legacy','Existing traveler','existing traveler','2026-11-01T12:00:00Z')"));
        sql(db, QStringLiteral("INSERT INTO settings VALUES('legacy','en','light',125)"));
        sql(db, QStringLiteral("PRAGMA application_id=%1").arg(teyvat::Store::application_id));
        sql(db, QStringLiteral("PRAGMA user_version=1"));
    });
}

QByteArray bytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error("Cannot read fixture"); }
    return file.readAll();
}

} // namespace

class UrlReceiver final : public QObject {
    Q_OBJECT
public:
    QUrl received;
public slots:
    void receive(const QUrl& url) { received = url; }
};

class ApplicationTests final : public QObject {
    Q_OBJECT
private slots:
    void defaults_and_restart() {
        QTemporaryDir dir(test_temp_template());
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("companion.db"));
        QString id;
        {
            teyvat::Store store(path);
            QCOMPARE(store.profiles().size(), 1);
            id = store.active_profile().id;
            QVERIFY(!id.isEmpty());
            QVERIFY(store.settings(id) == teyvat::Settings{});
            store.save_window_geometry(QByteArray("test-geometry"));
        }
        {
            teyvat::Store store(path);
            QCOMPARE(store.active_profile().id, id);
            QCOMPARE(store.window_geometry(), QByteArray("test-geometry"));
        }
        with_sql(path, [](QSqlDatabase& db) {
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), teyvat::Store::schema_version);
        });
    }

    void profile_isolation_and_unicode() {
        QTemporaryDir dir(test_temp_template());
        const auto path = dir.filePath(QStringLiteral("дані/companion.db"));
        QString first;
        QString second;
        const teyvat::Settings custom{QStringLiteral("ru"), QStringLiteral("light"), 150};
        {
            teyvat::Store store(path);
            first = store.active_profile().id;
            store.save_settings(first, custom);
            second = store.create_profile(QStringLiteral("  Мандрівниця ✦  "));
            QCOMPARE(store.active_profile().name, QStringLiteral("Мандрівниця ✦"));
            QVERIFY(store.settings(second) == teyvat::Settings{});
            store.activate_profile(first);
            QVERIFY(store.settings(first) == custom);
            store.rename_profile(second, QStringLiteral("Інший профіль"));
        }
        teyvat::Store reopened(path);
        QCOMPARE(reopened.profiles().size(), 2);
        QCOMPARE(reopened.active_profile().id, first);
        QVERIFY(reopened.settings(first) == custom);
        QVERIFY(reopened.settings(second) == teyvat::Settings{});
    }

    void invalid_changes_are_rejected() {
        QTemporaryDir dir(test_temp_template());
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        const auto first = store.active_profile();
        for (const auto& name : {QString(), QStringLiteral(" \t "), QString(81, QChar('a')), QStringLiteral("bad\nname")}) {
            QVERIFY_EXCEPTION_THROWN((void)store.create_profile(name), std::runtime_error);
        }
        QVERIFY_EXCEPTION_THROWN((void)store.create_profile(QStringLiteral("TRAVELER")), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(store.activate_profile(QStringLiteral("missing")), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(store.rename_profile(QStringLiteral("missing"), QStringLiteral("Name")), std::runtime_error);
        const auto original = store.settings(first.id);
        for (const auto& bad : {
            teyvat::Settings{QStringLiteral("xx"), QStringLiteral("dark"), 100},
            teyvat::Settings{QStringLiteral("uk"), QStringLiteral("system"), 100},
            teyvat::Settings{QStringLiteral("uk"), QStringLiteral("dark"), 999}}) {
            QVERIFY_EXCEPTION_THROWN(store.save_settings(first.id, bad), std::runtime_error);
        }
        QCOMPARE(store.profiles().size(), 1);
        QCOMPARE(store.active_profile().id, first.id);
        QVERIFY(store.settings(first.id) == original);
        const auto quoted = store.create_profile(QStringLiteral("O'Brien; DROP TABLE profiles;"));
        QCOMPARE(store.active_profile().id, quoted);
        QCOMPARE(store.profiles().size(), 2);
    }

    void migration_preserves_data_and_backups() {
        QTemporaryDir dir(test_temp_template());
        const auto path = dir.filePath(QStringLiteral("companion.db"));
        legacy_database(path);
        {
            teyvat::Store store(path);
            QCOMPARE(store.active_profile().id, QStringLiteral("legacy"));
            QVERIFY((store.settings(QStringLiteral("legacy")) == teyvat::Settings{QStringLiteral("en"), QStringLiteral("light"), 125}));
        }
        const auto backups = QDir(dir.path()).entryList({QStringLiteral("*.bak")}, QDir::Files);
        QCOMPARE(backups.size(), 1);
        with_sql(dir.filePath(backups.first()), [](QSqlDatabase& db) {
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA integrity_check"))); QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QStringLiteral("ok"));
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version"))); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
            QVERIFY(q.exec(QStringLiteral("SELECT name FROM profiles WHERE id='legacy'"))); QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QStringLiteral("Existing traveler"));
        });
    }

    void failed_migration_rolls_back() {
        QTemporaryDir dir(test_temp_template());
        const auto path = dir.filePath(QStringLiteral("companion.db"));
        legacy_database(path);
        // The pending migration first creates app_state, then fails when reading
        // this damaged legacy column. Rollback must undo that earlier CREATE too.
        with_sql(path, [](QSqlDatabase& db) { sql(db, QStringLiteral("ALTER TABLE profiles RENAME COLUMN created_at TO damaged_column")); });
        const auto before = bytes(path);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::Store{path}, std::runtime_error);
        QCOMPARE(bytes(path), before);
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*.bak")}, QDir::Files).size(), 1);
        with_sql(path, [](QSqlDatabase& db) {
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE name='app_state'"))); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0);
        });
    }

    void newer_foreign_and_corrupt_files_are_preserved() {
        QTemporaryDir dir(test_temp_template());
        const auto future = dir.filePath(QStringLiteral("future.db"));
        { teyvat::Store store(future); }
        with_sql(future, [](QSqlDatabase& db) { sql(db, QStringLiteral("PRAGMA user_version=999")); });
        const auto future_before = bytes(future);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::Store{future}, std::runtime_error);
        QCOMPARE(bytes(future), future_before);
        const auto foreign = dir.filePath(QStringLiteral("foreign.db"));
        with_sql(foreign, [](QSqlDatabase& db) { sql(db, QStringLiteral("CREATE TABLE unrelated(secret TEXT)")); });
        const auto foreign_before = bytes(foreign);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::Store{foreign}, std::runtime_error);
        QCOMPARE(bytes(foreign), foreign_before);
        const auto corrupt = dir.filePath(QStringLiteral("corrupt.db"));
        { QFile f(corrupt); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("not a SQLite database\n"); }
        const auto corrupt_before = bytes(corrupt);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::Store{corrupt}, std::runtime_error);
        QCOMPARE(bytes(corrupt), corrupt_before);
        const auto broken_refs = dir.filePath(QStringLiteral("broken-refs.db"));
        { teyvat::Store store(broken_refs); }
        with_sql(broken_refs, [](QSqlDatabase& db) { sql(db, QStringLiteral("UPDATE app_state SET active_profile='missing'")); });
        const auto refs_before = bytes(broken_refs);
        QVERIFY_EXCEPTION_THROWN((void)teyvat::Store{broken_refs}, std::runtime_error);
        QCOMPARE(bytes(broken_refs), refs_before);
    }

    void abrupt_process_exit_recovers_committed_state() {
        QTemporaryDir dir(test_temp_template());
        const auto path = dir.filePath(QStringLiteral("companion.db"));
        QString id;
        { teyvat::Store store(path); id = store.active_profile().id; store.rename_profile(id, QStringLiteral("Committed")); }
        QProcess writer;
        writer.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--crash-writer"), path});
        QVERIFY(writer.waitForFinished(15000));
        QCOMPARE(writer.exitCode(), 77);
        teyvat::Store recovered(path);
        QCOMPARE(recovered.profiles().size(), 1);
        QCOMPARE(recovered.active_profile().name, QStringLiteral("Committed"));
        QCOMPARE(recovered.active_profile().id, id);
    }

    void actual_application_storage_startup() {
        QTemporaryDir dir(test_temp_template());
        QProcess app;
        const auto binary = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("TeyvatCompanion.exe"));
        for (int run = 0; run < 2; ++run) {
            app.start(binary, {QStringLiteral("--data-dir"), dir.path(), QStringLiteral("--check-storage")});
            QVERIFY2(app.waitForFinished(15000), qPrintable(app.errorString()));
            QCOMPARE(app.exitCode(), 0);
        }
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        QCOMPARE(store.profiles().size(), 1);
    }

    void ui_profiles_settings_and_restart() {
        QTemporaryDir dir(test_temp_template());
        const auto path = dir.filePath(QStringLiteral("companion.db"));
        QString first;
        {
            teyvat::Store store(path);
            first = store.active_profile().id;
            teyvat::MainWindow window(store);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto* language = window.findChild<QComboBox*>(QStringLiteral("languageSetting"));
            auto* theme = window.findChild<QComboBox*>(QStringLiteral("themeSetting"));
            auto* scale = window.findChild<QComboBox*>(QStringLiteral("scaleSetting"));
            auto* nav_settings = window.findChild<QPushButton*>(QStringLiteral("nav2"));
            QVERIFY(language && theme && scale && nav_settings);
            QTest::mouseClick(nav_settings, Qt::LeftButton);
            QCOMPARE(language->currentData().toString(), QStringLiteral("en"));
            teyvat::HoYoLabSync sync(store);
            teyvat::CaptureRecorder recorder(dir.path());
            auto* record = recorder.findChild<QPushButton*>(QStringLiteral("startCalibrationCapture"));
            QVERIFY(record);
            QCOMPARE(sync.windowTitle(), QStringLiteral("Official HoYoLAB map"));
            QCOMPARE(record->text(), QStringLiteral("Start local recording"));
            language->setCurrentIndex(language->findData(QStringLiteral("ru")));
            QTRY_COMPARE(nav_settings->text(), QStringLiteral("Настройки"));
            QTRY_COMPARE(sync.windowTitle(), QStringLiteral("Официальная карта HoYoLAB"));
            QTRY_COMPARE(record->text(), QStringLiteral("Начать локальную запись"));
            QCOMPARE(store.settings(first).language, QStringLiteral("ru"));
            language->setCurrentIndex(language->findData(QStringLiteral("en")));
            theme->setCurrentIndex(theme->findData(QStringLiteral("light")));
            scale->setCurrentIndex(scale->findData(125));
            QCOMPARE(nav_settings->text(), QStringLiteral("Settings"));
            QVERIFY((store.settings(first) == teyvat::Settings{QStringLiteral("en"), QStringLiteral("light"), 125}));
            auto* nav_profiles = window.findChild<QPushButton*>(QStringLiteral("nav1"));
            QVERIFY(nav_profiles);
            QTest::mouseClick(nav_profiles, Qt::LeftButton);
            auto* name = window.findChild<QLineEdit*>(QStringLiteral("newProfileName"));
            auto* create = window.findChild<QPushButton*>(QStringLiteral("createProfile"));
            QVERIFY(name && create);
            name->setText(QStringLiteral("UI traveler"));
            QTest::mouseClick(create, Qt::LeftButton);
            QCOMPARE(store.profiles().size(), 2);
            QCOMPARE(store.active_profile().name, QStringLiteral("UI traveler"));
            auto* rename = window.findChild<QLineEdit*>(QStringLiteral("renameProfileName"));
            auto* save_name = window.findChild<QPushButton*>(QStringLiteral("renameProfile"));
            QVERIFY(rename && save_name);
            rename->setText(QStringLiteral("Renamed UI traveler"));
            QTest::mouseClick(save_name, Qt::LeftButton);
            QCOMPARE(store.active_profile().name, QStringLiteral("Renamed UI traveler"));
            auto* selector = window.findChild<QComboBox*>(QStringLiteral("profileSelector"));
            QVERIFY(selector);
            selector->setCurrentIndex(selector->findData(first));
            QCOMPARE(language->currentData().toString(), QStringLiteral("en"));
            QCOMPARE(theme->currentData().toString(), QStringLiteral("light"));
            QCOMPARE(scale->currentData().toInt(), 125);
            window.close();
        }
        {
            teyvat::Store store(path);
            teyvat::MainWindow window(store);
            QCOMPARE(store.active_profile().id, first);
            auto* language = window.findChild<QComboBox*>(QStringLiteral("languageSetting"));
            QVERIFY(language);
            QCOMPARE(language->currentData().toString(), QStringLiteral("en"));
            QVERIFY(!store.window_geometry().isEmpty());
        }
    }

    void official_map_opens_exact_url_without_credentials() {
        QTemporaryDir dir(test_temp_template());
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::MainWindow window(store);
        window.show();
        auto* open = window.findChild<QPushButton*>(QStringLiteral("openOfficialMap"));
        auto* note = window.findChild<QLabel*>(QStringLiteral("officialLoginNote"));
        QVERIFY(open && note);
        QVERIFY(note->text().contains(QStringLiteral("cookies")));
        UrlReceiver receiver;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &receiver, "receive");
        QTest::mouseClick(open, Qt::LeftButton);
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        QCOMPARE(receiver.received, QUrl(QStringLiteral("https://act.hoyolab.com/ys/app/interactive-map/index.html")));
        for (const auto* input : window.findChildren<QLineEdit*>()) { QVERIFY(input->echoMode() == QLineEdit::Normal); }
    }

    void map_ui_create_mark_filter_undo_and_zoom() {
        QTemporaryDir dir(test_temp_template()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::MainWindow window(store); window.resize(1280, 940); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* nav = window.findChild<QPushButton*>(QStringLiteral("nav3")); QVERIFY(nav); QTest::mouseClick(nav, Qt::LeftButton);
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("showAdvanced")), Qt::LeftButton);
        auto* create_map = window.findChild<QPushButton*>(QStringLiteral("newMap")); QVERIFY(create_map);
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                dialog->findChild<QLineEdit*>(QStringLiteral("layerTitle"))->setText(QStringLiteral("UI coordinate grid"));
                QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
            }
        });
        QTest::mouseClick(create_map, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(store.layers().size(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(create_map->isEnabled(), 10000);
        const auto layer = store.layers().first();
        auto* add = window.findChild<QPushButton*>(QStringLiteral("addMarker")); QVERIFY(add);
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                dialog->findChild<QLineEdit*>(QStringLiteral("markerLabel"))->setText(QStringLiteral("Ручна скриня"));
                dialog->findChild<QDoubleSpinBox*>(QStringLiteral("markerX"))->setValue(250);
                dialog->findChild<QDoubleSpinBox*>(QStringLiteral("markerY"))->setValue(350);
                QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
            }
        });
        QTest::mouseClick(add, Qt::LeftButton);
        const auto profile = store.active_profile().id;
        QTRY_COMPARE_WITH_TIMEOUT(store.markers(profile, layer.key).size(), 1, 10000);
        QCOMPARE(store.markers(profile, layer.key).first().category, QStringLiteral("common"));
        QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 10000);
        auto* table = window.findChild<QTableWidget*>(QStringLiteral("markerTable")); QVERIFY(table); QCOMPARE(table->rowCount(), 1); table->selectRow(0);
        auto* status = window.findChild<QComboBox*>(QStringLiteral("collectionStatus"));
        auto* apply = window.findChild<QPushButton*>(QStringLiteral("applyStatus")); QVERIFY(status && apply);
        status->setCurrentIndex(status->findData(QStringLiteral("manually_confirmed"))); QTest::mouseClick(apply, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(store.markers(profile, layer.key).first().status == teyvat::CollectionStatus::ManuallyConfirmed, 10000);
        auto* undo = window.findChild<QPushButton*>(QStringLiteral("undoProgress")); QVERIFY(undo); QTRY_VERIFY_WITH_TIMEOUT(undo->isEnabled(), 10000);
        QTest::mouseClick(undo, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(store.markers(profile, layer.key).first().status == teyvat::CollectionStatus::NotCollected, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 10000);
        auto* search = window.findChild<QLineEdit*>(QStringLiteral("mapSearch")); QVERIFY(search);
        search->setText(QStringLiteral("no match")); QTRY_COMPARE(table->rowCount(), 0);
        search->clear(); QTRY_COMPARE(table->rowCount(), 1);
        auto other = store.markers(profile, layer.key).first(); other.id = QStringLiteral("zz-other"); other.category = QStringLiteral("precious"); other.label = QStringLiteral("Other marker");
        store.save_marker(profile, other); window.findChild<teyvat::MapPage*>()->reload(); table->selectRow(0);
        auto* category = window.findChild<QComboBox*>(QStringLiteral("categoryFilter")); QVERIFY(category);
        category->setCurrentIndex(category->findData(QStringLiteral("precious")));
        QCOMPARE(table->rowCount(), 1); QVERIFY(table->selectedItems().isEmpty()); QVERIFY(!apply->isEnabled());
        auto* filter = window.findChild<QComboBox*>(QStringLiteral("statusFilter")); QVERIFY(filter);
        filter->setCurrentIndex(filter->findData(QStringLiteral("possibly_collected"))); QCOMPARE(table->rowCount(), 0);
        filter->setCurrentIndex(0); category->setCurrentIndex(0); QCOMPARE(table->rowCount(), 2);
        auto* canvas = window.findChild<teyvat::MapCanvas*>(); QVERIFY(canvas);
        canvas->fit_layer(); const auto scale = canvas->transform().m11();
        QTest::keyClick(canvas, Qt::Key_Plus); QVERIFY(canvas->transform().m11() > scale);
        QTest::keyClick(canvas, Qt::Key_F); QVERIFY(std::abs(canvas->transform().m11() - scale) < 1e-8);
        QCOMPARE(create_map->text(), QStringLiteral("New layer"));
    }

    void canvas_pan_and_double_click_position() {
        teyvat::MapCanvas canvas; canvas.resize(560, 400);
        teyvat::MapLayer layer{{QStringLiteral("test"), QStringLiteral("region"), QStringLiteral("surface")}, QStringLiteral("Test"), QStringLiteral("test:v1")};
        canvas.set_layer(layer, {}); canvas.show(); QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.fit_layer(); canvas.zoom(3.0);
        const auto before = canvas.mapToScene(canvas.viewport()->rect().center());
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(70, 70));
        QMouseEvent move(QEvent::MouseMove, QPointF(140, 130), QPointF(canvas.viewport()->mapToGlobal(QPoint(140, 130))), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas.viewport(), &move);
        QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(140, 130));
        const auto after = canvas.mapToScene(canvas.viewport()->rect().center()); QVERIFY(before != after);
        QSignalSpy placed(&canvas, &teyvat::MapCanvas::addAt);
        const auto pixel = canvas.viewport()->rect().center(); const auto expected = canvas.mapToScene(pixel);
        QTest::mouseDClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, pixel);
        QCOMPARE(placed.size(), 1); QCOMPARE(placed.first().first().toPointF(), expected);
    }

    void migration_from_actual_phase2_database() {
        const auto previous = qEnvironmentVariable("TEYVAT_PHASE2_EXE");
        if (previous.isEmpty()) { QSKIP("Set TEYVAT_PHASE2_EXE for the previous binary upgrade check."); }
        QTemporaryDir dir(test_temp_template()); QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment(); environment.remove(QStringLiteral("QT_QPA_PLATFORM")); environment.remove(QStringLiteral("QT_PLUGIN_PATH"));
        process.setProcessEnvironment(environment);
        process.start(previous, {QStringLiteral("--data-dir"), dir.path(), QStringLiteral("--check-storage")});
        QVERIFY2(process.waitForFinished(15000), qPrintable(process.errorString())); QCOMPARE(process.exitCode(), 0);
        const auto path = dir.filePath(QStringLiteral("companion.db"));
        with_sql(path, [](QSqlDatabase& db) {
            QSqlQuery q(db); QVERIFY(q.exec(QStringLiteral("PRAGMA user_version"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 2);
            QVERIFY(q.exec(QStringLiteral("UPDATE profiles SET name='Existing phase 2',name_key='existing phase 2'")));
            QVERIFY(q.exec(QStringLiteral("UPDATE settings SET language='en',theme='light',ui_scale=125")));
        });
        {
            teyvat::Store upgraded(path); QCOMPARE(upgraded.active_profile().name, QStringLiteral("Existing phase 2"));
            QVERIFY((upgraded.settings(upgraded.active_profile().id) == teyvat::Settings{QStringLiteral("en"), QStringLiteral("light"), 125}));
            QVERIFY(upgraded.layers().isEmpty());
        }
        const auto backups = QDir(dir.path()).entryList({QStringLiteral("*.before-v2-to-v%1-*.bak").arg(teyvat::Store::schema_version)}, QDir::Files); QCOMPARE(backups.size(), 1);
        with_sql(dir.filePath(backups.first()), [](QSqlDatabase& db) {
            QSqlQuery q(db); QVERIFY(q.exec(QStringLiteral("PRAGMA user_version"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 2);
            QVERIFY(q.exec(QStringLiteral("SELECT name FROM profiles"))); QVERIFY(q.next()); QCOMPARE(q.value(0).toString(), QStringLiteral("Existing phase 2"));
        });
    }

    void map_ui_import_review_preserves_progress_by_default() {
        QTemporaryDir dir(test_temp_template()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::MapLayer layer{{QStringLiteral("owned-test"), QStringLiteral("Region"), QStringLiteral("Surface")}, QStringLiteral("Imported test grid"), QStringLiteral("test:v1")};
        teyvat::MapMarker marker{layer.key, QStringLiteral("one"), QStringLiteral("common"), QStringLiteral("Imported marker"), {}, 100, 120};
        const teyvat::Dataset data{{layer}, {marker}};
        teyvat::MainWindow window(store); window.show();
        auto* page = window.findChild<teyvat::MapPage*>(); QVERIFY(page);
        bool saw_review = false, default_preserves = false;
        QTimer timer;
        connect(&timer, &QTimer::timeout, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog && dialog->objectName() == QStringLiteral("importReview")) {
                saw_review = true;
                default_preserves = !dialog->findChild<QCheckBox*>(QStringLiteral("overwriteProgress"))->isChecked();
                timer.stop(); QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
            }
        });
        timer.start(10); page->import_bytes(teyvat::encode_json(data), false);
        QTRY_COMPARE_WITH_TIMEOUT(store.layers().size(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(store.markers(store.active_profile().id, layer.key).size(), 1, 10000);
        QVERIFY(saw_review && default_preserves);
    }

    void map_ui_5000_markers_searches_without_data_loss() {
        QTemporaryDir dir(test_temp_template()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::MapLayer layer{{QStringLiteral("workload"), QStringLiteral("Test"), QStringLiteral("Surface")}, QStringLiteral("5000 synthetic markers"), QStringLiteral("test:v1")};
        teyvat::Dataset data{{layer}, {}};
        for (int i = 0; i < 5000; ++i) {
            data.markers.append({layer.key, QStringLiteral("load-%1").arg(i), QStringLiteral("common"), QStringLiteral("Workload marker %1").arg(i), {}, (i % 100) * 10.0, (i / 100) * 10.0});
        }
        const auto profile = store.active_profile().id;
        store.apply_import(data, profile, false, store.review_import(data, profile, false).token);
        teyvat::MainWindow window(store); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("nav3")), Qt::LeftButton);
        auto* table = window.findChild<QTableWidget*>(QStringLiteral("markerTable")); auto* search = window.findChild<QLineEdit*>(QStringLiteral("mapSearch"));
        QVERIFY(table && search); QCOMPARE(table->rowCount(), 5000);
        int heartbeat = 0; QTimer timer; connect(&timer, &QTimer::timeout, &window, [&] { ++heartbeat; }); timer.start(10);
        search->setText(QStringLiteral("load-4999")); QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 10000);
        QVERIFY(heartbeat > 0); QCOMPARE(store.markers(profile, layer.key).size(), 5000);
        QVERIFY(store.history(profile).isEmpty());
    }

    void offline_recording_automatically_updates_ui_and_respects_undo() {
        QTemporaryDir dir(test_temp_template()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::change_interface_language(store, QStringLiteral("uk"));
        const auto data = detection_fixtures::dataset(); const auto p = store.active_profile().id;
        store.apply_import(data, p, false, store.review_import(data, p, false).token);
        const auto path = detection_fixtures::save_bundle(dir.filePath(QStringLiteral("recording")),
            detection_fixtures::make_bundle(dir.filePath(QStringLiteral("recording")), {0, 0, 1, 1, 1, 0, 0, 1, 1, 1}, 42, 3, true));
        teyvat::MainWindow window(store); window.resize(1280, 940); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("nav3")), Qt::LeftButton);
        auto* page = window.findChild<teyvat::MapPage*>(); QVERIFY(page);
        int results_seen = 0; bool screenshot_ok = true, translated = true;
        QTimer closer; closer.setInterval(20);
        connect(&closer, &QTimer::timeout, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("detectionResults")) { return; }
            auto* summary = dialog->findChild<QLabel*>(QStringLiteral("detectionSummary"));
            translated &= summary && summary->text().contains(QStringLiteral("Автоматично зібрано"));
            const auto output = qEnvironmentVariable("TEYVAT_SCREENSHOT_DIR");
            if (!output.isEmpty() && results_seen == 0) {
                screenshot_ok &= QDir().mkpath(output);
                screenshot_ok &= dialog->grab().save(QDir(output).filePath(QStringLiteral("automation-results-uk.png")));
                screenshot_ok &= window.grab().save(QDir(output).filePath(QStringLiteral("automation-map-uk.png")));
            }
            ++results_seen; dialog->reject();
        });
        closer.start(); page->analyze_recording_file(path);
        QTRY_COMPARE_WITH_TIMEOUT(results_seen, 1, 10000); QVERIFY(translated); QVERIFY(screenshot_ok);
        const auto markers = store.markers(p, data.maps.first().key);
        QCOMPARE(markers[0].status, teyvat::CollectionStatus::Collected); QCOMPARE(markers[1].status, teyvat::CollectionStatus::Collected);
        QCOMPARE(store.history(p).size(), 2);
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("undoProgress")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(store.markers(p, data.maps.first().key).first().status, teyvat::CollectionStatus::NotCollected, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>(QStringLiteral("analyzeRecording"))->isEnabled(), 10000);
        page->analyze_recording_file(path); QTRY_COMPARE_WITH_TIMEOUT(results_seen, 2, 10000);
        QCOMPARE(store.markers(p, data.maps.first().key).first().status, teyvat::CollectionStatus::NotCollected);
        QCOMPARE(store.history(p).size(), 4); closer.stop();
    }

    void offline_ambiguous_candidates_can_be_reviewed_without_changes() {
        QTemporaryDir dir(test_temp_template()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        auto data = detection_fixtures::dataset(); data.markers[1].x = 103; data.markers[1].y = 120;
        const auto p = store.active_profile().id; store.apply_import(data, p, false, store.review_import(data, p, false).token);
        const auto path = detection_fixtures::bundle(dir.filePath(QStringLiteral("recording")));
        teyvat::MainWindow window(store); window.resize(1280, 940); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("nav3")), Qt::LeftButton);
        auto* page = window.findChild<teyvat::MapPage*>(); QVERIFY(page);
        bool reviewed = false; QTimer inspector; inspector.setInterval(20);
        connect(&inspector, &QTimer::timeout, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("detectionResults")) { return; }
            auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("detectionEvents"));
            table->setCurrentCell(0, 0); table->selectRow(0);
            auto* review = dialog->findChild<QPushButton*>(QStringLiteral("reviewDetection"));
            reviewed = review && review->isEnabled(); if (reviewed) { review->click(); } else { dialog->reject(); }
        });
        inspector.start(); page->analyze_recording_file(path); QTRY_VERIFY_WITH_TIMEOUT(reviewed, 10000); inspector.stop();
        QVERIFY(store.history(p).isEmpty());
        QCOMPARE(window.findChild<teyvat::MapCanvas*>()->selected_ids().size(), 2);
        QCOMPARE(window.findChild<QTableWidget*>(QStringLiteral("markerTable"))->selectionModel()->selectedRows().size(), 2);
        auto* confirm = window.findChild<QPushButton*>(QStringLiteral("confirmCollected")); QVERIFY(!confirm->isEnabled());
        auto* table = window.findChild<QTableWidget*>(QStringLiteral("markerTable")); table->clearSelection(); table->selectRow(0); QVERIFY(confirm->isEnabled());
        QTest::mouseClick(confirm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(store.markers(p, data.maps.first().key).first().status, teyvat::CollectionStatus::ManuallyConfirmed, 10000);
        QCOMPARE(store.markers(p, data.maps.first().key)[1].status, teyvat::CollectionStatus::NotCollected);
    }

    void minimap_pixels_drive_ui_progress_and_show_position() {
        QTemporaryDir dir(test_temp_template()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        const auto data = localization_fixtures::dataset(); const auto profile = store.active_profile().id;
        store.apply_import(data, profile, false, store.review_import(data, profile, false).token);
        const auto folder = dir.filePath(QStringLiteral("recording")); const auto path = localization_fixtures::bundle(folder);
        store.set_background(data.maps.first().key, bytes(QDir(folder).filePath(QStringLiteral("reference.png"))), QStringLiteral("Original synthetic terrain"), QStringLiteral("MIT"));
        teyvat::MainWindow window(store); window.resize(1280, 940); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("nav3")), Qt::LeftButton);
        auto* page = window.findChild<teyvat::MapPage*>(); bool seen = false, correct = false, saved = true;
        QTimer closer; closer.setInterval(20);
        connect(&closer, &QTimer::timeout, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); if (!dialog || dialog->objectName() != QStringLiteral("detectionResults")) { return; }
            auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("localizationResults"));
            correct = table && table->rowCount() == 10 && table->item(9, 1)->text() == QStringLiteral("230.00, 140.00");
            correct &= dialog->findChild<QLabel*>(QStringLiteral("localizationSummary"))->text().contains(QStringLiteral("10 / 10"));
            if (table) { dialog->findChild<QPushButton*>(QStringLiteral("showDetectionDetails"))->setChecked(true); dialog->resize(900, 620); dialog->findChild<QTabWidget*>()->setCurrentWidget(table); }
            const auto output = qEnvironmentVariable("TEYVAT_SCREENSHOT_DIR");
            if (!output.isEmpty()) {
                saved &= QDir().mkpath(output); saved &= dialog->grab().save(QDir(output).filePath(QStringLiteral("localization-results-uk.png")));
                saved &= window.grab().save(QDir(output).filePath(QStringLiteral("localization-map-uk.png")));
            }
            seen = true; dialog->reject();
        });
        closer.start(); page->analyze_recording_file(path); QTRY_VERIFY_WITH_TIMEOUT(seen, 10000); closer.stop(); QVERIFY(correct); QVERIFY(saved);
        QCOMPARE(store.markers(profile, data.maps.first().key).first().status, teyvat::CollectionStatus::Collected);
        int arrows = 0; for (auto* item : window.findChild<teyvat::MapCanvas*>()->scene()->items()) { if (item->data(1) == QStringLiteral("recordedPosition")) { ++arrows; QCOMPARE(item->pos(), QPointF(230, 140)); } }
        QCOMPARE(arrows, 1);
    }

    void minimap_analysis_can_be_cancelled_without_progress() {
        QTemporaryDir dir(test_temp_template()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        const auto data = localization_fixtures::dataset(); const auto profile = store.active_profile().id;
        store.apply_import(data, profile, false, store.review_import(data, profile, false).token);
        const auto path = localization_fixtures::bundle(dir.filePath(QStringLiteral("recording")));
        teyvat::MainWindow window(store); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("nav3")), Qt::LeftButton);
        window.findChild<teyvat::MapPage*>()->analyze_recording_file(path);
        auto* cancel = window.findChild<QPushButton*>(QStringLiteral("cancelAnalysis")); QVERIFY(cancel->isVisible()); cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>(QStringLiteral("analyzeRecording"))->isEnabled(), 10000);
        QVERIFY(store.history(profile).isEmpty());
        QCOMPARE(store.markers(profile, data.maps.first().key).first().status, teyvat::CollectionStatus::NotCollected);
    }

    void one_click_example_is_repeatable_and_keeps_personal_progress() {
        QTemporaryDir dir(test_temp_template()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::change_interface_language(store, QStringLiteral("uk"));
        const auto data = detection_fixtures::dataset(); const auto profile = store.active_profile().id;
        store.apply_import(data, profile, false, store.review_import(data, profile, false).token);
        store.set_status(profile, {data.markers.first()}, teyvat::CollectionStatus::ManuallyConfirmed);
        const auto original = store.export_layer(profile, data.maps.first().key); const auto history_count = store.history(profile).size();
        teyvat::MainWindow window(store); window.resize(900, 650); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* example = window.findChild<QPushButton*>(QStringLiteral("tryExample")); QVERIFY(example && example->isVisible());
        const auto output = qEnvironmentVariable("TEYVAT_SCREENSHOT_DIR");
        if (!output.isEmpty()) { QVERIFY(QDir().mkpath(output)); QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("simple-start-uk.png")))); }
        for (int pass = 0; pass < 2; ++pass) {
            bool summary_seen = false, finished = false, correct = true, saved = true;
            QElapsedTimer deadline; deadline.start(); QTimer driver; driver.setInterval(20);
            connect(&driver, &QTimer::timeout, &window, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) { return; }
                if (deadline.elapsed() > 12000) { correct = false; dialog->reject(); return; }
                if (dialog->objectName() == QStringLiteral("detectionResults")) {
                    auto* summary = dialog->findChild<QLabel*>(QStringLiteral("detectionSummary"));
                    correct &= summary && summary->text().contains(QStringLiteral("Автоматично зібрано: 2"));
                    correct &= !dialog->findChild<QPushButton*>(QStringLiteral("showDetectionDetails"))->isChecked(); summary_seen = true;
                    if (!output.isEmpty() && pass == 0) { saved &= dialog->grab().save(QDir(output).filePath(QStringLiteral("simple-result-uk.png"))); }
                    if (pass == 1) { dialog->findChild<QPushButton*>(QStringLiteral("undoDetection"))->click(); } else { dialog->reject(); }
                } else if (dialog->objectName() == QStringLiteral("exampleWindow") && summary_seen) {
                    auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("markerTable"));
                    auto* history = dialog->findChild<QPushButton*>(QStringLiteral("showProgress"));
                    if (!history->isEnabled()) { return; }
                    const auto expected = QCoreApplication::translate("teyvat::MapPage", pass == 0 ? "Collected" : "Not collected");
                    if (table->rowCount() != 2 || table->item(0, 2)->text() != expected || table->item(1, 2)->text() != expected) { return; }
                    correct &= dialog->findChild<QWidget*>(QStringLiteral("advancedTools"))->isHidden();
                    correct &= dialog->findChild<QPushButton*>(QStringLiteral("showAdvanced"))->isHidden();
                    if (!output.isEmpty() && pass == 0) { saved &= dialog->grab().save(QDir(output).filePath(QStringLiteral("simple-example-uk.png"))); }
                    finished = true; dialog->reject();
                } else if (dialog->objectName() != QStringLiteral("exampleWindow")) {
                    if (auto* message = qobject_cast<QMessageBox*>(dialog)) { qWarning("Unexpected example message: %s | %s", qPrintable(message->text()), qPrintable(message->detailedText())); }
                    correct = false; dialog->reject();
                }
            });
            driver.start(); example->click(); driver.stop(); QVERIFY(summary_seen && finished && correct && saved);
            QCOMPARE(store.active_profile().id, profile); QCOMPARE(store.profiles().size(), 1); QCOMPARE(store.layers().size(), 1);
            QVERIFY(store.export_layer(profile, data.maps.first().key) == original); QCOMPARE(store.history(profile).size(), history_count);
            QVERIFY(QDir(dir.path()).entryList({QStringLiteral("example-*")}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        }
        window.findChild<QPushButton*>(QStringLiteral("openMyMap"))->click();
        QVERIFY(window.findChild<QWidget*>(QStringLiteral("advancedTools"))->isHidden());
        if (!output.isEmpty()) { QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("simple-map-uk.png")))); }
    }

    void capture_actual_preview() {
        const auto output = qEnvironmentVariable("TEYVAT_SCREENSHOT_DIR");
        if (output.isEmpty()) { QSKIP("Set TEYVAT_SCREENSHOT_DIR to capture the actual native widgets."); }
        QVERIFY(QDir().mkpath(output));
        QTemporaryDir dir(test_temp_template());
        teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::change_interface_language(store, QStringLiteral("uk"));
        int unexpected_dialogs = 0;
        QTimer dialog_watchdog;
        connect(&dialog_watchdog, &QTimer::timeout, this, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                ++unexpected_dialogs; qWarning("Unexpected screenshot message: %s", qPrintable(box->text())); box->reject();
            }
        });
        dialog_watchdog.start(1000);
        teyvat::MainWindow window(store);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(150);
        QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("overview-uk.png"))));
        auto* settings = window.findChild<QPushButton*>(QStringLiteral("nav2"));
        auto* language = window.findChild<QComboBox*>(QStringLiteral("languageSetting"));
        QVERIFY(settings && language);
        QTest::mouseClick(settings, Qt::LeftButton);
        language->setCurrentIndex(language->findData(QStringLiteral("en")));
        QTest::qWait(150);
        QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("settings-en.png"))));
        teyvat::MapLayer layer{{QStringLiteral("qa-owned"), QStringLiteral("Тестовий регіон"), QStringLiteral("Поверхня")},
            QStringLiteral("Перевірочна координатна карта"), QStringLiteral("qa:local-units-v1"), 1000, 650,
            QStringLiteral("Synthetic QA grid — not game geography"), QStringLiteral("MIT; original test data")};
        teyvat::Dataset data{{layer}, {}};
        for (int i = 0; i < 8; ++i) {
            data.markers.append({layer.key, QStringLiteral("qa-%1").arg(i), i % 2 ? QStringLiteral("common") : QStringLiteral("precious"),
                QStringLiteral("Тестова скриня %1").arg(i + 1), QStringLiteral("Synthetic QA marker"),
                100.0 + (i % 4) * 250, 150.0 + (i / 4) * 320});
        }
        const auto profile = store.active_profile().id;
        store.apply_import(data, profile, false, store.review_import(data, profile, false).token);
        store.set_status(profile, {data.markers[0], data.markers[1]}, teyvat::CollectionStatus::ManuallyConfirmed);
        store.set_status(profile, {data.markers[2]}, teyvat::CollectionStatus::PossiblyCollected);
        window.resize(1280, 940); language->setCurrentIndex(language->findData(QStringLiteral("uk")));
        auto* map_page = window.findChild<teyvat::MapPage*>(); QVERIFY(map_page); map_page->reload();
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("nav3")), Qt::LeftButton);
        QTest::qWait(150); QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("map-uk.png"))));
        language->setCurrentIndex(language->findData(QStringLiteral("en")));
        QTest::qWait(150); QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("map-en.png"))));
        QCOMPARE(unexpected_dialogs, 0);
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen")) {
        // Offscreen's font backend does not enumerate Windows fonts. Use installed
        // fonts for QA; do not copy or redistribute them in application packages.
        const auto fonts = QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("Fonts"));
        (void)QFontDatabase::addApplicationFont(QDir(fonts).filePath(QStringLiteral("segoeui.ttf")));
        (void)QFontDatabase::addApplicationFont(QDir(fonts).filePath(QStringLiteral("seguisym.ttf")));
    }
    QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 10));
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--crash-writer")) {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("crash"));
        db.setDatabaseName(QString::fromLocal8Bit(argv[2]));
        if (!db.open() || !db.transaction()) { return 2; }
        sql(db, QStringLiteral("INSERT INTO profiles VALUES('uncommitted','Uncommitted','uncommitted','2026-11-07T12:00:00Z')"));
        std::_Exit(77); // Deliberately bypass destructors to exercise SQLite crash recovery.
    }
    ApplicationTests tests;
    try {
        return QTest::qExec(&tests, argc, argv);
    } catch (const std::exception& error) {
        qCritical("Unhandled test error: %s", error.what());
        return 1;
    }
}

#include "application_tests.moc"
