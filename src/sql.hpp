#pragma once
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <functional>
#include <stdexcept>

namespace teyvat::sql {
[[noreturn]] inline void fail(const QString& message) {
    throw std::runtime_error(message.toUtf8().constData());
}
inline QSqlQuery query(const QSqlDatabase& db, const QString& statement, const QVariantList& values = {}) {
    QSqlQuery q(db);
    if (!q.prepare(statement)) { fail(QStringLiteral("Cannot prepare database operation: %1").arg(q.lastError().text())); }
    for (const auto& value : values) { q.addBindValue(value); }
    if (!q.exec()) { fail(QStringLiteral("Database operation failed: %1").arg(q.lastError().text())); }
    return q;
}
inline QVariant scalar(const QSqlDatabase& db, const QString& statement) {
    auto q = query(db, statement);
    if (!q.next()) { fail(QStringLiteral("Database metadata is missing.")); }
    return q.value(0);
}
inline void transaction(const QSqlDatabase& db, const std::function<void()>& operation) {
    (void)query(db, QStringLiteral("BEGIN IMMEDIATE"));
    try { operation(); (void)query(db, QStringLiteral("COMMIT")); }
    catch (...) { QSqlQuery q(db); q.exec(QStringLiteral("ROLLBACK")); throw; }
}
void create_map_schema(const QSqlDatabase& db);
void create_detection_schema(const QSqlDatabase& db);
void create_animation_schema(const QSqlDatabase& db);
} // namespace teyvat::sql
