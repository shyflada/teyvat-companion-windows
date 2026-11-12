#include "teyvat/store.hpp"
#include "sql.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <functional>
#include <stdexcept>

namespace teyvat {
using sql::fail;
using sql::query;
using sql::scalar;
using sql::transaction;
namespace {

QString checked_name(const QString& input) {
    const QString name = input.trimmed();
    if (name.isEmpty() || name.size() > 80) {
        fail(QStringLiteral("Profile name must contain 1 to 80 characters."));
    }
    for (const auto character : name) {
        if (character.category() == QChar::Other_Control) {
            fail(QStringLiteral("Profile names cannot contain control characters."));
        }
    }
    return name;
}

QString name_key(const QString& name) {
    return name.normalized(QString::NormalizationForm_KC).toCaseFolded();
}

void validate(const Settings& settings) {
    if (settings.language != QStringLiteral("en") && settings.language != QStringLiteral("uk")
        && settings.language != QStringLiteral("ru")) {
        fail(QStringLiteral("Unsupported interface language."));
    }
    if (settings.theme != QStringLiteral("dark") && settings.theme != QStringLiteral("light")) {
        fail(QStringLiteral("Unsupported theme."));
    }
    if (settings.ui_scale != 100 && settings.ui_scale != 125 && settings.ui_scale != 150) {
        fail(QStringLiteral("Unsupported interface scale."));
    }
}

QString insert_profile(const QSqlDatabase& db, const QString& name) {
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    (void)query(db, QStringLiteral("INSERT INTO profiles(id,name,name_key,created_at) VALUES(?,?,?,?)"),
                {id, name, name_key(name), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)});
    (void)query(db, QStringLiteral("INSERT INTO settings(profile_id) VALUES(?)"), {id});
    return id;
}

} // namespace

Store::Store(QString path)
    : path_(QFileInfo(path).absoluteFilePath()),
      connection_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        fail(QStringLiteral("The QSQLITE driver is missing. Re-extract the complete application folder."));
    }
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        fail(QStringLiteral("Cannot create the data directory. Choose a writable --data-dir."));
    }
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_);
    db_.setDatabaseName(path_);
    db_.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));
    try {
        if (!db_.open()) {
            fail(QStringLiteral("Cannot open %1: %2").arg(path_, db_.lastError().text()));
        }
        if (scalar(db_, QStringLiteral("PRAGMA quick_check")).toString() != QStringLiteral("ok")) {
            fail(QStringLiteral("Database integrity check failed. Preserve this file and use a known-good backup."));
        }
        (void)query(db_, QStringLiteral("PRAGMA foreign_keys=ON"));
        {
            auto violations = query(db_, QStringLiteral("PRAGMA foreign_key_check"));
            if (violations.next()) {
                fail(QStringLiteral("Database references are damaged. Preserve the database and its backups."));
            }
        }
        migrate();
        (void)query(db_, QStringLiteral("PRAGMA journal_mode=WAL"));
        (void)query(db_, QStringLiteral("PRAGMA synchronous=FULL"));
        ensure_profile();
        (void)settings(active_profile().id);
    } catch (...) {
        close();
        throw;
    }
}

Store::~Store() { close(); }

void Store::close() noexcept {
    db_.close();
    db_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection_);
}

void Store::migrate() {
    const int version = scalar(db_, QStringLiteral("PRAGMA user_version")).toInt();
    const int app_id = scalar(db_, QStringLiteral("PRAGMA application_id")).toInt();
    if (version > schema_version) {
        fail(QStringLiteral("This database needs a newer Teyvat Companion version. It was not downgraded."));
    }
    if (version == 0) {
        const int tables = scalar(db_, QStringLiteral(
            "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'")).toInt();
        if (app_id != 0 || tables != 0) {
            fail(QStringLiteral("Unrecognized database. Choose a different data directory; this file was preserved."));
        }
    } else if (app_id != application_id) {
        fail(QStringLiteral("This file does not belong to Teyvat Companion."));
    }
    if (version == schema_version) {
        return;
    }
    if (version > 0) {
        const auto backup_path = path_ + QStringLiteral(".before-v%1-to-v%2-%3.bak")
            .arg(version).arg(schema_version).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        // VACUUM INTO produces a consistent SQLite snapshot, including committed WAL data.
        (void)query(db_, QStringLiteral("VACUUM INTO ?"), {backup_path});
    }
    transaction(db_, [&] {
        if (version < 1) {
            (void)query(db_, QStringLiteral(
                "CREATE TABLE profiles("
                "id TEXT PRIMARY KEY NOT NULL, name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 80),"
                "name_key TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL)"));
            (void)query(db_, QStringLiteral(
                "CREATE TABLE settings("
                "profile_id TEXT PRIMARY KEY NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,"
                "language TEXT NOT NULL DEFAULT 'uk' CHECK(language IN ('uk','en')),"
                "theme TEXT NOT NULL DEFAULT 'dark' CHECK(theme IN ('dark','light')),"
                "ui_scale INTEGER NOT NULL DEFAULT 100 CHECK(ui_scale IN (100,125,150)))"));
            (void)query(db_, QStringLiteral("PRAGMA application_id=%1").arg(application_id));
            (void)query(db_, QStringLiteral("PRAGMA user_version=1"));
        }
        if (version < 2) {
            (void)query(db_, QStringLiteral(
                "CREATE TABLE app_state("
                "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                "active_profile TEXT NOT NULL REFERENCES profiles(id), window_geometry BLOB)"));
            (void)query(db_, QStringLiteral(
                "INSERT INTO app_state(singleton,active_profile) "
                "SELECT 1,id FROM profiles ORDER BY created_at,id LIMIT 1"));
            (void)query(db_, QStringLiteral("PRAGMA user_version=2"));
        }
        if (version < 3) {
            sql::create_map_schema(db_);
            (void)query(db_, QStringLiteral("PRAGMA user_version=3"));
        }
        if (version < 4) {
            sql::create_detection_schema(db_);
            (void)query(db_, QStringLiteral("PRAGMA user_version=4"));
        }
        if (version < 5) {
            sql::create_animation_schema(db_);
            (void)query(db_, QStringLiteral("PRAGMA user_version=5"));
        }
        if (version < 6) {
            (void)query(db_, QStringLiteral(
                "CREATE TABLE settings_v6("
                "profile_id TEXT PRIMARY KEY NOT NULL REFERENCES profiles(id) ON DELETE CASCADE,"
                "language TEXT NOT NULL DEFAULT 'en' CHECK(language IN ('en','uk','ru')),"
                "theme TEXT NOT NULL DEFAULT 'dark' CHECK(theme IN ('dark','light')),"
                "ui_scale INTEGER NOT NULL DEFAULT 100 CHECK(ui_scale IN (100,125,150)))"));
            (void)query(db_, QStringLiteral("INSERT INTO settings_v6(profile_id,language,theme,ui_scale) SELECT profile_id,language,theme,ui_scale FROM settings"));
            (void)query(db_, QStringLiteral("DROP TABLE settings"));
            (void)query(db_, QStringLiteral("ALTER TABLE settings_v6 RENAME TO settings"));
            (void)query(db_, QStringLiteral("PRAGMA user_version=6"));
        }
    });
}

void Store::ensure_profile() {
    transaction(db_, [&] {
        if (scalar(db_, QStringLiteral("SELECT count(*) FROM profiles")).toInt() == 0) {
            const auto id = insert_profile(db_, QStringLiteral("Traveler"));
            (void)query(db_, QStringLiteral("INSERT INTO app_state(singleton,active_profile) VALUES(1,?)"), {id});
        } else if (scalar(db_, QStringLiteral("SELECT count(*) FROM app_state")).toInt() != 1) {
            fail(QStringLiteral("The active profile record is missing. Preserve the database for recovery."));
        }
    });
}

QString Store::path() const { return path_; }

QVector<Profile> Store::profiles() const {
    QVector<Profile> result;
    auto q = query(db_, QStringLiteral("SELECT id,name,created_at FROM profiles ORDER BY created_at,id"));
    while (q.next()) {
        result.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString()});
    }
    return result;
}

Profile Store::active_profile() const {
    auto q = query(db_, QStringLiteral(
        "SELECT p.id,p.name,p.created_at FROM profiles p JOIN app_state a ON a.active_profile=p.id WHERE a.singleton=1"));
    if (!q.next()) {
        fail(QStringLiteral("Active local profile is missing."));
    }
    return {q.value(0).toString(), q.value(1).toString(), q.value(2).toString()};
}

QString Store::create_profile(const QString& input) {
    const auto name = checked_name(input);
    QString id;
    transaction(db_, [&] {
        id = insert_profile(db_, name);
        (void)query(db_, QStringLiteral("UPDATE app_state SET active_profile=? WHERE singleton=1"), {id});
    });
    return id;
}

void Store::activate_profile(const QString& id) {
    (void)settings(id); // Reject nonexistent profiles or malformed stored settings before changing state.
    auto q = query(db_, QStringLiteral("UPDATE app_state SET active_profile=? WHERE singleton=1"), {id});
    if (q.numRowsAffected() != 1) {
        fail(QStringLiteral("Cannot select the local profile."));
    }
}

void Store::rename_profile(const QString& id, const QString& input) {
    const auto name = checked_name(input);
    auto q = query(db_, QStringLiteral("UPDATE profiles SET name=?,name_key=? WHERE id=?"), {name, name_key(name), id});
    if (q.numRowsAffected() != 1) {
        fail(QStringLiteral("Local profile does not exist."));
    }
}

Settings Store::settings(const QString& id) const {
    auto q = query(db_, QStringLiteral("SELECT language,theme,ui_scale FROM settings WHERE profile_id=?"), {id});
    if (!q.next()) {
        fail(QStringLiteral("Settings for the local profile are missing."));
    }
    Settings result{q.value(0).toString(), q.value(1).toString(), q.value(2).toInt()};
    validate(result);
    return result;
}

void Store::save_settings(const QString& id, const Settings& settings) {
    validate(settings);
    auto q = query(db_, QStringLiteral("UPDATE settings SET language=?,theme=?,ui_scale=? WHERE profile_id=?"),
                   {settings.language, settings.theme, settings.ui_scale, id});
    if (q.numRowsAffected() != 1) {
        fail(QStringLiteral("Cannot save settings for an unknown local profile."));
    }
}

QByteArray Store::window_geometry() const {
    return scalar(db_, QStringLiteral("SELECT window_geometry FROM app_state WHERE singleton=1")).toByteArray();
}

void Store::save_window_geometry(const QByteArray& geometry) {
    (void)query(db_, QStringLiteral("UPDATE app_state SET window_geometry=? WHERE singleton=1"), {geometry});
}

} // namespace teyvat
