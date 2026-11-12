#include "teyvat/main_window.hpp"
#include "teyvat/compass.hpp"
#include "teyvat/store.hpp"
#include "teyvat/version.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <cstdio>
#include <exception>
#include <stdexcept>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("TeyvatCompanion"));
    QApplication::setApplicationDisplayName(QStringLiteral("Teyvat Companion"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/teyvat-companion.png")));
    QApplication::setApplicationVersion(QString::fromLatin1(teyvat::application_version));
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 10));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Teyvat Companion · Release candidate for in-game navigation"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("data-dir"), QStringLiteral("Use this directory for local profiles and settings."), QStringLiteral("directory")});
    parser.addOption({QStringLiteral("check-storage"), QStringLiteral("Check database startup and exit without opening the window.")});
    parser.process(app);
    const auto data_dir = parser.isSet(QStringLiteral("data-dir"))
        ? QDir(parser.value(QStringLiteral("data-dir"))).absolutePath()
        : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    try {
        if (data_dir.isEmpty() || !QDir().mkpath(data_dir)) {
            throw std::runtime_error("Cannot create the data directory. Choose a writable --data-dir.");
        }
        QLockFile lock(QDir(data_dir).filePath(QStringLiteral("application.lock")));
        if (!lock.tryLock(0)) {
            throw std::runtime_error("This data directory is already open or not writable. Close the other instance or choose another --data-dir.");
        }
        teyvat::Store store(QDir(data_dir).filePath(QStringLiteral("companion.db")));
        if (parser.isSet(QStringLiteral("check-storage"))) { return 0; }
        teyvat::apply_interface_language(store.settings(store.active_profile().id).language);
        app.setQuitOnLastWindowClosed(false);
        teyvat::CompassPanel window(store); window.show();
        return app.exec();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Teyvat Companion: %s\n", error.what());
        if (!parser.isSet(QStringLiteral("check-storage"))) {
            QMessageBox::critical(nullptr, QStringLiteral("Teyvat Companion — помилка запуску / Startup error"),
                QStringLiteral("Не вдалося відкрити локальні дані. Збережіть базу та резервні копії.\n"
                               "Could not open local data. Preserve the database and backups.\n\n%1\n\n%2")
                    .arg(data_dir, QString::fromUtf8(error.what())));
        }
        return 1;
    }
}
