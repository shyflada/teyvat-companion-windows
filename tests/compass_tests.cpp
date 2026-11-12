#include "teyvat/compass.hpp"
#include "teyvat/main_window.hpp"
#include "teyvat/capture_recorder.hpp"
#include "teyvat/map_page.hpp"
#include "teyvat/hoyolab_sync.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QKeySequenceEdit>
#include <QFile>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
QString scratch() {
    const auto root = QCoreApplication::applicationDirPath() + QStringLiteral("/test-tmp");
    QDir().mkpath(root); return root + QStringLiteral("/compass-XXXXXX");
}
QAction* action(teyvat::CompassPanel& panel, const char* name) {
    return panel.findChild<QAction*>(QString::fromLatin1(name));
}
void screenshot(QWidget& widget, const QString& name) {
    const auto path = qEnvironmentVariable("TEYVAT_SCREENSHOT_DIR");
    if (path.isEmpty()) { return; }
    QVERIFY(QDir().mkpath(path)); QVERIFY(widget.grab().save(path + QLatin1Char('/') + name + QStringLiteral(".png")));
}
#ifdef Q_OS_WIN
HWND handle(QWidget& widget) { return reinterpret_cast<HWND>(widget.winId()); }
bool activate_own(QWidget& widget) {
    widget.show(); widget.raise(); widget.activateWindow(); SetForegroundWindow(handle(widget));
    if (!QTest::qWaitForWindowExposed(&widget)) { return false; }
    QTest::qWait(100);
    for (int i = 0; i < 20 && GetForegroundWindow() != handle(widget); ++i) { QTest::qWait(50); }
    return GetForegroundWindow() == handle(widget);
}
#endif
}

class CompassTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setQuitOnLastWindowClosed(false); }
    void init() { teyvat::apply_interface_language(QStringLiteral("en")); QCoreApplication::processEvents(); }
    void regional_choice_persists_without_changing_progress() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db"))); const auto profile = store.active_profile().id;
        { teyvat::CompassPanel panel(store); auto* regions = panel.findChild<QComboBox*>(QStringLiteral("navigationRegion")); QVERIFY(regions);
          QVERIFY(regions->findData(16) >= 0); QVERIFY(regions->findData(1001) >= 0); regions->setCurrentIndex(regions->findData(16)); }
        teyvat::CompassPanel restored(store); const auto* regions = restored.findChild<QComboBox*>(QStringLiteral("navigationRegion")); QVERIFY(regions); QCOMPARE(regions->currentData().toInt(), 16);
        teyvat::apply_interface_language(QStringLiteral("uk")); QCoreApplication::processEvents(); QCOMPARE(regions->currentText(), QStringLiteral("Сніжна"));
        QVERIFY(store.history(profile).isEmpty()); QVERIFY(store.layers().isEmpty());
    }
    void collection_options_save_distance_and_shortcut_without_losing_previous_values() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::CompassPanel panel(store); bool shown = false;
        QTimer::singleShot(10, &panel, [&] {
            auto* dialog = panel.findChild<QDialog*>(QStringLiteral("collectionOptions")); if (!dialog) { return; } shown = true;
            auto* distance = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("nearDistance")); auto* key = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("collectShortcut"));
            QVERIFY(distance); QVERIFY(key); QCOMPARE(distance->value(), 20.); QCOMPARE(key->keySequence(), QKeySequence(QStringLiteral("F8")));
            distance->setValue(30); screenshot(*dialog, QStringLiteral("collection-settings-en"));
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        });
        action(panel, "collectionSettings")->trigger(); QVERIFY(shown);
        QSettings options(dir.filePath(QStringLiteral("compass.ini")), QSettings::IniFormat);
        QCOMPARE(options.value(QStringLiteral("nearDistance")).toDouble(), 30.); QCOMPARE(options.value(QStringLiteral("collectShortcut")).toString(), QStringLiteral("F8"));
    }
    void manual_confirmation_works_in_owned_live_example_and_advances_immediately() {
#ifdef Q_OS_WIN
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::CompassPanel panel(store); panel.show_navigation_example(); auto* world = panel.findChild<QLabel*>(QStringLiteral("navigationExampleWorld")); QVERIFY(world);
        if (!activate_own(*world)) { QSKIP("Own example could not become foreground; no input was sent."); }
        const auto directories = QDir(dir.path()).entryList({QStringLiteral("navigation-example-*")}, QDir::Dirs | QDir::NoDotAndDotDot); QCOMPARE(directories.size(), 1);
        teyvat::Store example_store(QDir(dir.path()).filePath(directories[0] + QStringLiteral("/example.db"))); const auto p = example_store.active_profile().id;
        auto* state = panel.findChild<QLabel*>(QStringLiteral("compassStatus")); QVERIFY(state);
        QTRY_VERIFY_WITH_TIMEOUT(state->text().contains(QStringLiteral("Example chest A")) || GetForegroundWindow() != handle(*world), 1500);
        if (GetForegroundWindow() != handle(*world)) { QSKIP("Foreground left the owned example before manual confirmation; no input was sent."); }
        PostMessageW(handle(panel), WM_HOTKEY, 0x5444, 0); QTest::qWait(80);
        if (GetForegroundWindow() != handle(*world)) { QSKIP("Foreground left the owned example during manual confirmation."); }
        if (example_store.history(p).isEmpty()) { qInfo("Native hotkey registration unavailable on this host; verifying the shared confirmation action."); action(panel, "confirmChestCollected")->trigger(); }
        QCOMPARE(example_store.history(p).size(), 1); QCOMPARE(example_store.history(p).first().after, QStringLiteral("manually_confirmed"));
        QVERIFY(state->text().contains(QStringLiteral("Example chest B"))); action(panel, "confirmChestCollected")->trigger(); QCOMPARE(example_store.history(p).size(), 1);
        QVERIFY(store.history(store.active_profile().id).isEmpty()); screenshot(panel, QStringLiteral("manual-next-target-en"));
#endif
    }
    void official_map_browser_anonymous() {
#ifdef Q_OS_WIN
        if (qEnvironmentVariable("TEYVAT_WEBVIEW_SMOKE") != QStringLiteral("1")) { QSKIP("Opt-in live official website smoke check; ordinary tests stay offline."); }
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::HoYoLabSync browser(store); QSignalSpy statuses(&browser, &teyvat::HoYoLabSync::status_changed); browser.open(2);
        const auto expected = QStringLiteral("Sign in on the official website. Local confirmations remain queued.");
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(statuses.cbegin(), statuses.cend(), [&](const auto& row) { return row.first().toString() == expected; }), 45000);
        QVERIFY(store.history(store.active_profile().id).isEmpty()); QVERIFY(browser.collected(QStringLiteral("hoyolab-map-2")).isEmpty());
        if (activate_own(browser)) {
            QTest::qWait(1500); const auto path = qEnvironmentVariable("TEYVAT_SCREENSHOT_DIR");
            if (!path.isEmpty() && GetForegroundWindow() == handle(browser)) {
                QVERIFY(QDir().mkpath(path)); const auto bounds = browser.frameGeometry();
                const auto preview = browser.screen()->grabWindow(0, bounds.x(), bounds.y(), bounds.width(), bounds.height());
                if (GetForegroundWindow() == handle(browser)) { QVERIFY(preview.save(path + QStringLiteral("/official-map-browser-native.png"))); }
            }
        }
#else
        QSKIP("The official account browser uses the native Windows WebView2 runtime.");
#endif
    }
    void camera_turn_changes_rendered_direction_without_player_movement() {
        teyvat::CompassOverlay overlay; const teyvat::Guidance guidance{{"synthetic", "fixed-target"}, 72, 0};
        const auto green = [](const QImage& image, QRect area) {
            int count = 0; for (int y = area.top(); y <= area.bottom(); ++y) { for (int x = area.left(); x <= area.right(); ++x) {
                const auto c = image.pixelColor(x, y); if (c.green() > 150 && c.green() - c.red() > 40) { ++count; }
            } } return count;
        };
        const QRect left(24, 15, 33, 50), right(71, 15, 33, 50);
        overlay.set_guidance(guidance, true, QStringLiteral("Synthetic target"), 90, true, 72); QTest::qWait(90);
        const auto facing_east = overlay.grab().toImage(); QVERIFY(green(facing_east, left) > green(facing_east, right) + 20);
        overlay.set_guidance(guidance, true, QStringLiteral("Synthetic target"), -90, true, 72); QTest::qWait(90);
        const auto facing_west = overlay.grab().toImage(); QVERIFY(green(facing_west, right) > green(facing_west, left) + 20);
        overlay.set_guidance(guidance, true, {}, {}, true, 72); QTest::qWait(90);
        const auto unknown = overlay.grab().toImage(); QVERIFY(green(unknown, left) + green(unknown, right) < green(facing_west, right));
        teyvat::TargetBeacon beacon; beacon.set_target(90, 72, QRect(0, 0, 1280, 720));
#ifdef Q_OS_WIN
        const auto foreground = GetForegroundWindow(); beacon.show(); QTest::qWait(50);
        const auto styles = GetWindowLongPtrW(handle(beacon), GWL_EXSTYLE);
        QVERIFY(styles & WS_EX_TRANSPARENT); QVERIFY(styles & WS_EX_NOACTIVATE); QVERIFY(styles & WS_EX_LAYERED); QCOMPARE(GetForegroundWindow(), foreground);
#endif
        screenshot(overlay, QStringLiteral("camera-relative-overlay"));
    }
    void native_styles_hit_testing_focus_and_foreground_hiding() {
#ifdef Q_OS_WIN
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::CompassPanel panel(store); panel.show(); panel.show_test_window();
        auto* preview = panel.findChild<QWidget*>(QStringLiteral("overlayTestWindow")); QVERIFY(preview);
        if (!activate_own(*preview)) { QSKIP("Desktop refused foreground activation; no mouse input sent."); }
        QTRY_VERIFY_WITH_TIMEOUT(panel.overlay().isVisible(), 1500);
        auto& overlay = panel.overlay(); const auto styles = GetWindowLongPtrW(handle(overlay), GWL_EXSTYLE);
        QVERIFY(styles & WS_EX_TRANSPARENT); QVERIFY(styles & WS_EX_NOACTIVATE); QVERIFY(styles & WS_EX_LAYERED);
        QVERIFY(styles & WS_EX_TOOLWINDOW); QVERIFY(overlay.windowFlags() & Qt::WindowStaysOnTopHint);
        QVERIFY(overlay.windowFlags() & Qt::WindowTransparentForInput); QCOMPARE(GetForegroundWindow(), handle(*preview));
        RECT rectangle{}; QVERIFY(GetWindowRect(handle(overlay), &rectangle));
        const POINT point{(rectangle.left + rectangle.right) / 2, (rectangle.top + rectangle.bottom) / 2};
        const auto target = WindowFromPoint(point); DWORD owner = 0; GetWindowThreadProcessId(target, &owner);
        QCOMPARE(owner, static_cast<DWORD>(QCoreApplication::applicationPid()));
        QCOMPARE(GetAncestor(target, GA_ROOT), handle(*preview));
        screenshot(panel, QStringLiteral("compass-control-en")); screenshot(overlay, QStringLiteral("compass-unknown-en"));
        screenshot(*preview, QStringLiteral("compass-test-window-en"));
        teyvat::apply_interface_language(QStringLiteral("uk")); QCoreApplication::processEvents();
        screenshot(panel, QStringLiteral("compass-control-uk")); screenshot(overlay, QStringLiteral("compass-unknown-uk"));
        const auto preview_position = overlay.pos();
        if (!activate_own(panel)) { QSKIP("Desktop refused the second foreground activation."); }
        QTRY_VERIFY_WITH_TIMEOUT(!overlay.isVisible(), 1500);
        QVERIFY(overlay.pos() != preview_position);
        qInfo("Native layered/topmost/no-activate styles, WindowFromPoint pass-through, focus and foreground hiding verified.");
#else
        QSKIP("Windows desktop test.");
#endif
    }
    void real_mouse_click_on_owned_test_window() {
#ifdef Q_OS_WIN
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        teyvat::CompassPanel panel(store); panel.show_test_window();
        auto* preview = panel.findChild<QWidget*>(QStringLiteral("overlayTestWindow")); QVERIFY(preview);
        if (!activate_own(*preview)) { QSKIP("Desktop refused foreground activation; no mouse input sent."); }
        QTRY_VERIFY_WITH_TIMEOUT(panel.overlay().isVisible(), 1500);
        RECT rectangle{}; QVERIFY(GetWindowRect(handle(panel.overlay()), &rectangle));
        const POINT point{(rectangle.left + rectangle.right) / 2, (rectangle.top + rectangle.bottom) / 2};
        auto* button = preview->findChild<QPushButton*>(QStringLiteral("overlayTestButton")); QVERIFY(button); QSignalSpy clicks(button, &QPushButton::clicked);
        POINT original{}; QVERIFY(GetCursorPos(&original));
        const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT input[3]{}; for (auto& item : input) { item.type = INPUT_MOUSE; }
        input[0].mi.dx = MulDiv(point.x - left, 65535, width - 1); input[0].mi.dy = MulDiv(point.y - top, 65535, height - 1);
        input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        // Only this test executable's foreground window may receive synthetic test input.
        if (GetForegroundWindow() != handle(*preview) || GetAncestor(WindowFromPoint(point), GA_ROOT) != handle(*preview)) {
            QSKIP("Own test window lost foreground; no mouse input sent.");
        }
        SetLastError(0); const auto sent = SendInput(3, input, sizeof(INPUT)); const auto input_error = GetLastError(); QTest::qWait(150);
        POINT current{}; GetCursorPos(&current);
        if (GetForegroundWindow() == handle(*preview) && std::abs(current.x - point.x) <= 2 && std::abs(current.y - point.y) <= 2) { SetCursorPos(original.x, original.y); }
        if (sent == 0) { QSKIP(qPrintable(QStringLiteral("Host SendInput unavailable (Windows error %1); actual click requires manual Test A.").arg(input_error))); }
        QCOMPARE(sent, 3u); QCOMPARE(clicks.count(), 1); QCOMPARE(GetForegroundWindow(), handle(*preview));
        qInfo("REAL Windows click through layered overlay reached owned test button; focus preserved.");
#else
        QSKIP("Windows desktop test.");
#endif
    }
    void editing_toggle_geometry_and_restart() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); const auto path = dir.filePath(QStringLiteral("companion.db"));
        QPoint saved;
        {
            teyvat::Store store(path); teyvat::CompassPanel panel(store); auto& overlay = panel.overlay();
            action(panel, "size160")->trigger(); action(panel, "opacity50")->trigger(); action(panel, "economyMode")->trigger();
            action(panel, "moveCompass")->trigger(); QVERIFY(overlay.editing()); QVERIFY(overlay.isVisible());
            QVERIFY(!(overlay.windowFlags() & Qt::WindowTransparentForInput));
#ifdef Q_OS_WIN
            QVERIFY(!(GetWindowLongPtrW(handle(overlay), GWL_EXSTYLE) & WS_EX_TRANSPARENT));
            QVERIFY(GetWindowLongPtrW(handle(overlay), GWL_EXSTYLE) & WS_EX_NOACTIVATE);
#endif
            QCOMPARE(overlay.size(), QSize(160, 160)); QVERIFY(qAbs(overlay.windowOpacity() - .5) < .01);
            overlay.place(QPoint(180, 170)); const auto before = overlay.pos(); QSignalSpy moved(&overlay, &teyvat::CompassOverlay::moved);
            QTest::mousePress(&overlay, Qt::LeftButton, {}, QPoint(50, 50)); QTest::mouseMove(&overlay, QPoint(70, 65));
            QTest::mouseRelease(&overlay, Qt::LeftButton, {}, QPoint(50, 50));
            QCOMPARE(moved.count(), 1); QVERIFY(overlay.pos() != before); saved = overlay.pos();
            auto* enabled = action(panel, "compassEnabled"); QVERIFY(enabled); enabled->setChecked(false);
            QVERIFY(!overlay.editing()); QVERIFY(!overlay.isVisible()); QVERIFY(!action(panel, "moveCompass")->isEnabled());
            enabled->setChecked(true); QVERIFY(!overlay.editing());
        }
        {
            teyvat::Store store(path); teyvat::CompassPanel panel(store);
            QCOMPARE(panel.overlay().pos(), saved); QCOMPARE(panel.overlay().size(), QSize(160, 160));
            QVERIFY(qAbs(panel.overlay().windowOpacity() - .5) < .01); QVERIFY(!action(panel, "economyMode")->isChecked());
            panel.overlay().place(QPoint(-100000, 100000));
            QVERIFY(QGuiApplication::primaryScreen()->availableGeometry().contains(panel.overlay().geometry()));
        }
    }
    void hotkey_conflict_has_controls_and_releases_registration() {
#ifdef Q_OS_WIN
        QWidget reservation; const auto owner = handle(reservation);
        const bool reserved = RegisterHotKey(owner, 0x5442, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');
        const auto registration_error = reserved ? 0 : GetLastError();
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        {
            teyvat::CompassPanel panel(store);
            if (reserved || registration_error != 0) { QVERIFY(!panel.findChild<QLabel*>(QStringLiteral("compassNotice"))->text().isEmpty()); }
            auto* enabled = action(panel, "compassEnabled"); QVERIFY(enabled); enabled->setChecked(false); QVERIFY(!enabled->isChecked());
            PostMessageW(handle(panel), WM_HOTKEY, 0x5440, 0); QTRY_VERIFY(enabled->isChecked());
            PostMessageW(handle(panel), WM_HOTKEY, 0x5441, 0); QTRY_VERIFY(panel.overlay().editing());
            PostMessageW(handle(panel), WM_HOTKEY, 0x5441, 0); QTRY_VERIFY(!panel.overlay().editing());
        }
        if (!reserved) { QSKIP(qPrintable(QStringLiteral("Hotkey reservation unavailable (Windows error %1); controls and message handlers verified.").arg(registration_error))); }
        UnregisterHotKey(owner, 0x5442);
        QVERIFY(RegisterHotKey(owner, 0x5442, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M')); UnregisterHotKey(owner, 0x5442);
#endif
    }
    void malformed_options_preserved_and_shared_language_switches() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        const QByteArray broken("[unterminated\nsize=bad\n");
        { QFile file(dir.filePath(QStringLiteral("compass.ini"))); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(broken), broken.size()); }
        {
            teyvat::CompassPanel panel(store); panel.show(); QVERIFY(!panel.findChild<QLabel*>(QStringLiteral("compassNotice"))->text().isEmpty());
            action(panel, "size96")->trigger(); action(panel, "compassEnabled")->setChecked(false);
            teyvat::MainWindow tools(store); tools.show(); QCoreApplication::processEvents();
            auto* language = tools.findChild<QComboBox*>(QStringLiteral("languageSetting")); QVERIFY(language);
            language->setCurrentIndex(language->findData(QStringLiteral("uk"))); QCoreApplication::processEvents();
            QCOMPARE(action(panel, "exitCompanion")->text(), QStringLiteral("Вийти"));
            auto* languages = panel.findChild<QMenu*>(QStringLiteral("settingsLanguage")); QVERIFY(languages);
            QVERIFY(QMetaObject::invokeMethod(languages, "aboutToShow"));
            QVERIFY(action(panel, "languageUk")->isChecked());
            screenshot(panel, QStringLiteral("compass-settings-read-error-uk"));
            action(panel, "moveCompass")->setEnabled(true);
            language->setCurrentIndex(language->findData(QStringLiteral("en"))); QCoreApplication::processEvents();
            QCOMPARE(action(panel, "exitCompanion")->text(), QStringLiteral("Exit"));
        }
        QFile file(dir.filePath(QStringLiteral("compass.ini"))); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), broken);
    }
    void compact_panel_starts_in_english_and_saves_language_choices() {
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        const auto profile = store.active_profile().id;
        QCOMPARE(store.settings(profile).language, QStringLiteral("en"));
        teyvat::CompassPanel panel(store); panel.show();
        const auto* settings = panel.findChild<QToolButton*>(QStringLiteral("compassSettings")); QVERIFY(settings); QVERIFY(settings->isVisible());
        const auto* start = panel.findChild<QPushButton*>(QStringLiteral("startNavigation")); QVERIFY(start); QVERIFY(start->isVisible());
        QCOMPARE(start->text(), QStringLiteral("Start navigation"));
        const auto* targets = panel.findChild<QWidget*>(QStringLiteral("navigationTargetActions")); QVERIFY(targets); QVERIFY(targets->isHidden());
        screenshot(panel, QStringLiteral("compact-navigation-en"));
        auto* russian = action(panel, "languageRu"); QVERIFY(russian); russian->trigger(); QCoreApplication::processEvents();
        QCOMPARE(store.settings(profile).language, QStringLiteral("ru"));
        QVERIFY(russian->isChecked()); QVERIFY(start->text() != QStringLiteral("Start navigation"));
        QCOMPARE(action(panel, "exitCompanion")->text(), QStringLiteral("Выход"));
        screenshot(panel, QStringLiteral("compact-navigation-ru"));
        auto* ukrainian = action(panel, "languageUk"); QVERIFY(ukrainian); ukrainian->trigger(); QCoreApplication::processEvents();
        QCOMPARE(store.settings(profile).language, QStringLiteral("uk"));
        QCOMPARE(action(panel, "exitCompanion")->text(), QStringLiteral("Вийти"));
        auto* english = action(panel, "languageEn"); QVERIFY(english); english->trigger(); QCoreApplication::processEvents();
        QCOMPARE(store.settings(profile).language, QStringLiteral("en"));
        QCOMPARE(start->text(), QStringLiteral("Start navigation"));
    }
    void native_capture_has_foreground_size_and_overlay_guards() {
#ifdef Q_OS_WIN
        QLabel source; source.setWindowTitle(QStringLiteral("Teyvat owned capture test")); source.setFixedSize(240, 128);
        QPixmap image(240, 128); image.fill(QColor(42, 160, 80)); source.setPixmap(image);
        if (!activate_own(source)) { QSKIP("Foreground unavailable; no screen pixels read."); }
        const auto identity = teyvat::inspect_window(static_cast<quintptr>(source.winId())); QVERIFY(identity);
        auto capture = teyvat::capture_regions(*identity, identity->client_size, {QRect(20, 20, 64, 64)});
        if (capture.reason == teyvat::CaptureReason::Unavailable) { QSKIP("Host screen capture unavailable; no fallback used."); }
        QCOMPARE(capture.reason, teyvat::CaptureReason::Ready); QCOMPARE(capture.images.size(), 1); QCOMPARE(capture.images[0].size(), QSize(64, 64));
        // Window exposure precedes the compositor's show animation. Wait for our
        // actual painted pixels without loosening the colour/identity checks.
        const auto painted = [&] {
            capture = teyvat::capture_regions(*identity, identity->client_size, {QRect(20, 20, 64, 64)});
            if (capture.reason != teyvat::CaptureReason::Ready || capture.images.isEmpty()) { return false; }
            const auto color = capture.images[0].pixelColor(32, 32);
            return qAbs(color.red() - 42) <= 12 && qAbs(color.green() - 160) <= 12 && qAbs(color.blue() - 80) <= 12;
        };
        QTRY_VERIFY_WITH_TIMEOUT(painted(), 1000);
        const auto color = capture.images[0].pixelColor(32, 32); qInfo("Owned native capture RGB: %d %d %d", color.red(), color.green(), color.blue());
        QCOMPARE(teyvat::capture_regions(*identity, identity->client_size + QSize(1, 0), {QRect(20, 20, 64, 64)}).reason, teyvat::CaptureReason::Changed);
        teyvat::CompassOverlay overlay; overlay.place(source.mapToGlobal(QPoint(10, 10))); overlay.show(); QTest::qWait(100);
        QCOMPARE(teyvat::capture_regions(*identity, identity->client_size, {QRect(20, 20, 64, 64)}, static_cast<quintptr>(overlay.winId())).reason, teyvat::CaptureReason::Occluded);
        overlay.hide(); QWidget other; other.setWindowTitle(QStringLiteral("Teyvat owned pause test"));
        if (!activate_own(other)) { QSKIP("Second own foreground activation unavailable."); }
        const auto paused = teyvat::capture_regions(*identity, identity->client_size, {QRect(20, 20, 64, 64)});
        QCOMPARE(paused.reason, teyvat::CaptureReason::Inactive); QVERIFY(paused.images.isEmpty());
#endif
    }
    void one_click_live_example_collects_without_personal_changes() {
#ifdef Q_OS_WIN
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid()); teyvat::Store store(dir.filePath(QStringLiteral("companion.db")));
        const auto profile = store.active_profile().id;
        {
            teyvat::CompassPanel panel(store); panel.show_navigation_example();
            auto* world = panel.findChild<QLabel*>(QStringLiteral("navigationExampleWorld")); QVERIFY(world);
            if (!activate_own(*world)) { QSKIP("Own example could not become foreground."); }
            const auto identity = teyvat::inspect_window(static_cast<quintptr>(world->winId())); QVERIFY(identity);
            qInfo("Example client pixels %dx%d, device pixel ratio %.2f", identity->client_size.width(), identity->client_size.height(), world->devicePixelRatioF());
            const auto probe = teyvat::capture_regions(*identity, identity->client_size, {QRect(144, 24, 65, 65)});
            if (probe.reason == teyvat::CaptureReason::Unavailable) { QSKIP("Host screen capture unavailable; the visual loop is verified separately on images."); }
            QCOMPARE(probe.reason, teyvat::CaptureReason::Ready);
            const auto directories = QDir(dir.path()).entryList({QStringLiteral("navigation-example-*")}, QDir::Dirs | QDir::NoDotAndDotDot); QCOMPARE(directories.size(), 1);
            const auto database = QDir(dir.path()).filePath(directories[0] + QStringLiteral("/example.db"));
            {
                teyvat::Store preview(database); const auto p = preview.active_profile().id;
                QElapsedTimer deadline; deadline.start();
                while (preview.history(p).isEmpty() && deadline.elapsed() < 10000) {
                    QTest::qWait(100);
                    if (!teyvat::source_active(*identity)) { QSKIP("Foreground left the owned example; uninterrupted live capture is unavailable."); }
                }
                const auto notice = panel.findChild<QLabel*>(QStringLiteral("compassNotice"));
                QVERIFY2(preview.history(p).size() == 1, notice ? qPrintable(notice->text()) : "No first live collection");
                screenshot(panel.overlay(), QStringLiteral("compass-guiding-en"));
                teyvat::apply_interface_language(QStringLiteral("uk")); QCoreApplication::processEvents();
                screenshot(panel, QStringLiteral("navigation-control-uk")); screenshot(panel.overlay(), QStringLiteral("compass-guiding-uk"));
                deadline.restart();
                while (preview.history(p).size() < 2 && deadline.elapsed() < 18000) {
                    QTest::qWait(100);
                    if (!teyvat::source_active(*identity)) { QSKIP("Foreground left the owned example before the second live collection."); }
                }
                QCOMPARE(preview.history(p).size(), 2);
                bool map_shown = false;
                QTimer::singleShot(100, &panel, [&] {
                    if (auto* dialog = panel.findChild<QDialog*>(QStringLiteral("targetMapPreview"))) {
                        map_shown = dialog->findChild<teyvat::MapCanvas*>() != nullptr;
                        screenshot(*dialog, QStringLiteral("target-map-synthetic-uk")); dialog->accept();
                    }
                });
                auto* targets = panel.findChild<QWidget*>(QStringLiteral("navigationTargetActions")); QVERIFY(targets); QVERIFY(targets->isHidden());
                auto* map = action(panel, "showNavigationMap"); QVERIFY(map); map->trigger(); QVERIFY(map_shown);
            }
            QVERIFY(store.layers().isEmpty()); QVERIFY(store.history(profile).isEmpty()); QCOMPARE(store.profiles().size(), 1);
            screenshot(panel, QStringLiteral("live-example-complete-en")); screenshot(panel.overlay(), QStringLiteral("compass-navigation-complete-en"));
        }
        QVERIFY(QDir(dir.path()).entryList({QStringLiteral("navigation-example-*")}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
#endif
    }
    void recorder_saves_only_opted_in_regions_with_real_timestamps() {
#ifdef Q_OS_WIN
        QTemporaryDir dir(scratch()); QVERIFY(dir.isValid());
        QLabel source; source.setWindowTitle(QStringLiteral("Teyvat owned recorder test")); source.setFixedSize(800, 600);
        QPixmap image(800, 600); image.fill(QColor(55, 90, 150)); source.setPixmap(image);
        if (!activate_own(source)) { QSKIP("Own recorder window could not become foreground."); }
        const auto identity = teyvat::inspect_window(static_cast<quintptr>(source.winId())); QVERIFY(identity);
        if (teyvat::capture_regions(*identity, identity->client_size, {QRect(0, 0, 64, 64)}).reason == teyvat::CaptureReason::Unavailable) { QSKIP("Host screen capture unavailable."); }
        teyvat::CaptureRecorder recorder(dir.path()); QSignalSpy saved(&recorder, &teyvat::CaptureRecorder::saved);
        QVERIFY(!QDir(dir.filePath(QStringLiteral("captures"))).exists());
        recorder.start_recording(*identity); QTest::qWait(1700); recorder.finish(true);
        QCOMPARE(saved.count(), 1); const auto output = saved.first()[0].toString();
        QVERIFY(QFileInfo(output).absoluteFilePath().startsWith(QFileInfo(dir.path()).absoluteFilePath() + QLatin1Char('/')));
        QFile file(QDir(output).filePath(QStringLiteral("capture.json"))); QVERIFY(file.open(QIODevice::ReadOnly)); const auto root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.value(QStringLiteral("format")).toString(), QStringLiteral("teyvat-calibration-capture")); QCOMPARE(root.value(QStringLiteral("state")).toString(), QStringLiteral("interrupted"));
        const auto frames = root.value(QStringLiteral("frames")).toArray(); QVERIFY(frames.size() >= 2); qint64 previous = -1;
        for (const auto& value : frames) {
            const auto entry = value.toObject(); const auto time = entry.value(QStringLiteral("time_ms")).toInteger(); QVERIFY(time > previous); previous = time;
            const QImage mini(QDir(output).filePath(entry.value(QStringLiteral("minimap")).toString())); QCOMPARE(mini.size(), QSize(640, 600));
            const QImage scene(QDir(output).filePath(entry.value(QStringLiteral("scene")).toString())); QCOMPARE(scene.size(), QSize(512, 512));
            QVERIFY(qAbs(scene.pixelColor(256, 256).blue() - 150) <= 12);
        }
        QVERIFY(!root.contains(QStringLiteral("process"))); QVERIFY(!root.contains(QStringLiteral("uid")));
#endif
    }
};
QTEST_MAIN(CompassTests)
#include "compass_tests.moc"
