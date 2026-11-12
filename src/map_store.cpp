#include "teyvat/store.hpp"
#include "sql.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

namespace teyvat {
using sql::fail;
using sql::query;
using sql::scalar;
using sql::transaction;
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString nonnull(const QString& text) { return text.isNull() ? QStringLiteral("") : text; }
QVariantList layer_values(const LayerKey& key) { return {key.source, key.region, key.layer}; }
void bump(const QSqlDatabase& db) { (void)query(db, QStringLiteral("UPDATE map_meta SET revision=revision+1 WHERE singleton=1")); }

QVector<MapMarker> read_markers(const QSqlDatabase& db, const QString& profile, const std::optional<LayerKey>& layer) {
    QString statement = QStringLiteral(
        "SELECT m.source,m.region,m.layer,m.id,m.category,m.label,m.notes,m.x,m.y,"
        "coalesce(p.status,'not_collected'),m.retired,m.origin FROM markers m "
        "LEFT JOIN user_progress p ON p.source=m.source AND p.marker_id=m.id AND p.profile_id=?");
    QVariantList values{profile};
    if (layer) { statement += QStringLiteral(" WHERE m.source=? AND m.region=? AND m.layer=?"); values.append(layer_values(*layer)); }
    statement += QStringLiteral(" ORDER BY m.source,m.id LIMIT 20001");
    auto q = query(db, statement, values);
    QVector<MapMarker> result;
    while (q.next()) {
        result.append({{q.value(0).toString(), q.value(1).toString(), q.value(2).toString()},
            q.value(3).toString(), q.value(4).toString(), q.value(5).toString(), q.value(6).toString(),
            q.value(7).toDouble(), q.value(8).toDouble(), read_status(q.value(9).toString()),
            q.value(10).toBool(), q.value(11).toString()});
    }
    if (result.size() > max_markers) { fail(QStringLiteral("The local database exceeds the supported 20,000 marker limit.")); }
    return result;
}

void write_progress(const QSqlDatabase& db, const QString& profile, const QString& source, const QString& id,
                    CollectionStatus status, const QString& command, const QString& origin, const QVariant& undo = {}) {
    auto old = query(db, QStringLiteral("SELECT status,revision FROM user_progress WHERE profile_id=? AND source=? AND marker_id=?"), {profile, source, id});
    QString before = QStringLiteral("not_collected");
    qint64 revision = 0;
    if (old.next()) { before = old.value(0).toString(); revision = old.value(1).toLongLong(); }
    old.finish();
    const auto after = stored_status(status);
    if (before == after) { return; }
    const auto time = now();
    (void)query(db, QStringLiteral(
        "INSERT INTO user_progress(profile_id,source,marker_id,status,revision,updated_at) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(profile_id,source,marker_id) DO UPDATE SET status=excluded.status,revision=excluded.revision,updated_at=excluded.updated_at"),
        {profile, source, id, after, revision + 1, time});
    (void)query(db, QStringLiteral(
        "INSERT INTO detection_history(profile_id,source,marker_id,before_status,after_status,before_revision,after_revision,command_id,origin,created_at,undo_of) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
        {profile, source, id, before, after, revision, revision + 1, command, origin, time, undo});
}

QString schema_description(const QSqlDatabase& db) {
    QString result;
    auto q = query(db, QStringLiteral("SELECT type,name,tbl_name,coalesce(sql,'') FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name"));
    while (q.next()) { for (int i = 0; i < 4; ++i) { result += q.value(i).toString() + QChar(31); } result += QChar(30); }
    return result;
}
} // namespace

void sql::create_map_schema(const QSqlDatabase& db) {
    const QStringList statements{
        QStringLiteral("CREATE TABLE map_regions(source TEXT NOT NULL,region TEXT NOT NULL,title TEXT NOT NULL,PRIMARY KEY(source,region))"),
        QStringLiteral("CREATE TABLE map_layers(source TEXT NOT NULL,region TEXT NOT NULL,layer TEXT NOT NULL,title TEXT NOT NULL,frame TEXT NOT NULL,width REAL NOT NULL CHECK(width>0 AND width<=1000000),height REAL NOT NULL CHECK(height>0 AND height<=1000000),provenance TEXT NOT NULL,license TEXT NOT NULL,data_version TEXT NOT NULL,background BLOB,background_credit TEXT NOT NULL DEFAULT '',background_license TEXT NOT NULL DEFAULT '',PRIMARY KEY(source,region,layer),FOREIGN KEY(source,region) REFERENCES map_regions(source,region))"),
        QStringLiteral("CREATE TABLE marker_categories(source TEXT NOT NULL,category TEXT NOT NULL,PRIMARY KEY(source,category))"),
        QStringLiteral("CREATE TABLE markers(source TEXT NOT NULL,id TEXT NOT NULL,region TEXT NOT NULL,layer TEXT NOT NULL,category TEXT NOT NULL,label TEXT NOT NULL,notes TEXT NOT NULL DEFAULT '',x REAL NOT NULL,y REAL NOT NULL,retired INTEGER NOT NULL CHECK(retired IN(0,1)),origin TEXT NOT NULL CHECK(origin IN('manual','import')),PRIMARY KEY(source,id),FOREIGN KEY(source,region,layer) REFERENCES map_layers(source,region,layer),FOREIGN KEY(source,category) REFERENCES marker_categories(source,category))"),
        QStringLiteral("CREATE INDEX markers_layer ON markers(source,region,layer)"),
        QStringLiteral("CREATE TABLE user_progress(profile_id TEXT NOT NULL REFERENCES profiles(id),source TEXT NOT NULL,marker_id TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN('not_collected','possibly_collected','collected','manually_confirmed','unknown')),revision INTEGER NOT NULL CHECK(revision>0),updated_at TEXT NOT NULL,PRIMARY KEY(profile_id,source,marker_id),FOREIGN KEY(source,marker_id) REFERENCES markers(source,id))"),
        QStringLiteral("CREATE TABLE detection_history(id INTEGER PRIMARY KEY AUTOINCREMENT,profile_id TEXT NOT NULL REFERENCES profiles(id),source TEXT NOT NULL,marker_id TEXT NOT NULL,before_status TEXT NOT NULL CHECK(before_status IN('not_collected','possibly_collected','collected','manually_confirmed','unknown')),after_status TEXT NOT NULL CHECK(after_status IN('not_collected','possibly_collected','collected','manually_confirmed','unknown')),before_revision INTEGER NOT NULL,after_revision INTEGER NOT NULL,command_id TEXT NOT NULL,origin TEXT NOT NULL CHECK(origin IN('manual','import','undo')),created_at TEXT NOT NULL,undo_of INTEGER UNIQUE REFERENCES detection_history(id),FOREIGN KEY(source,marker_id) REFERENCES markers(source,id))"),
        QStringLiteral("CREATE INDEX history_profile ON detection_history(profile_id,id)"),
        QStringLiteral("CREATE INDEX history_marker ON detection_history(profile_id,source,marker_id,id)"),
        QStringLiteral("CREATE TABLE data_versions(source TEXT NOT NULL,version TEXT NOT NULL,provenance TEXT NOT NULL,license TEXT NOT NULL,imported_at TEXT NOT NULL,PRIMARY KEY(source,version))"),
        QStringLiteral("CREATE TABLE map_meta(singleton INTEGER PRIMARY KEY CHECK(singleton=1),database_id TEXT NOT NULL,revision INTEGER NOT NULL)")
    };
    for (const auto& statement : statements) { (void)query(db, statement); }
    (void)query(db, QStringLiteral("INSERT INTO map_meta VALUES(1,?,0)"), {uuid()});
}

void sql::create_detection_schema(const QSqlDatabase& db) {
    // Preserve event IDs and self-referencing undo records while adding an origin.
    (void)query(db, QStringLiteral(
        "CREATE TABLE detection_history_v4(id INTEGER PRIMARY KEY AUTOINCREMENT,profile_id TEXT NOT NULL REFERENCES profiles(id),source TEXT NOT NULL,marker_id TEXT NOT NULL,before_status TEXT NOT NULL CHECK(before_status IN('not_collected','possibly_collected','collected','manually_confirmed','unknown')),after_status TEXT NOT NULL CHECK(after_status IN('not_collected','possibly_collected','collected','manually_confirmed','unknown')),before_revision INTEGER NOT NULL,after_revision INTEGER NOT NULL,command_id TEXT NOT NULL,origin TEXT NOT NULL CHECK(origin IN('manual','import','undo','detection')),created_at TEXT NOT NULL,undo_of INTEGER UNIQUE REFERENCES detection_history_v4(id),FOREIGN KEY(source,marker_id) REFERENCES markers(source,id))"));
    (void)query(db, QStringLiteral("INSERT INTO detection_history_v4 SELECT * FROM detection_history"));
    (void)query(db, QStringLiteral("DROP TABLE detection_history"));
    (void)query(db, QStringLiteral("ALTER TABLE detection_history_v4 RENAME TO detection_history"));
    (void)query(db, QStringLiteral("CREATE INDEX history_profile ON detection_history(profile_id,id)"));
    (void)query(db, QStringLiteral("CREATE INDEX history_marker ON detection_history(profile_id,source,marker_id,id)"));
    (void)query(db, QStringLiteral(
        "CREATE TABLE detection_receipts(profile_id TEXT NOT NULL REFERENCES profiles(id),event_id BLOB NOT NULL CHECK(length(event_id)=32),recording_hash BLOB NOT NULL CHECK(length(recording_hash)=32),source TEXT NOT NULL,marker_id TEXT NOT NULL,closed_score REAL NOT NULL CHECK(closed_score BETWEEN 0.96 AND 1),opened_score REAL NOT NULL CHECK(opened_score BETWEEN 0.96 AND 1),position_confidence REAL NOT NULL CHECK(position_confidence BETWEEN 0 AND 1),recorded_ms INTEGER NOT NULL,command_id TEXT NOT NULL,provenance TEXT NOT NULL,license TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(profile_id,event_id),FOREIGN KEY(source,marker_id) REFERENCES markers(source,id))"));
}
void sql::create_animation_schema(const QSqlDatabase& db) {
    // Animation cues have physical measurements, not template similarities.
    // Keep v4 template receipts and their >=.96 constraints intact.
    (void)query(db, QStringLiteral(
        "CREATE TABLE animation_receipts(profile_id TEXT NOT NULL REFERENCES profiles(id),event_id BLOB NOT NULL CHECK(length(event_id)=32),recording_hash BLOB NOT NULL CHECK(length(recording_hash)=32),source TEXT NOT NULL,marker_id TEXT NOT NULL,proof TEXT NOT NULL CHECK(length(proof) BETWEEN 2 AND 4096),position_confidence REAL NOT NULL CHECK(position_confidence BETWEEN 0 AND 1),recorded_ms INTEGER NOT NULL,command_id TEXT NOT NULL,provenance TEXT NOT NULL,license TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(profile_id,event_id),FOREIGN KEY(source,marker_id) REFERENCES markers(source,id))"));
}

QVector<MapLayer> Store::layers() const {
    auto q = query(db_, QStringLiteral("SELECT source,region,layer,title,frame,width,height,provenance,license,data_version FROM map_layers ORDER BY source,region,layer LIMIT 1025"));
    QVector<MapLayer> result;
    while (q.next()) {
        result.append({{q.value(0).toString(), q.value(1).toString(), q.value(2).toString()},
            q.value(3).toString(), q.value(4).toString(), q.value(5).toDouble(), q.value(6).toDouble(),
            q.value(7).toString(), q.value(8).toString(), q.value(9).toString()});
    }
    if (result.size() > max_map_layers) { fail(QStringLiteral("At most 1024 map layers are supported.")); }
    return result;
}

QVector<MapMarker> Store::markers(const QString& profile, const LayerKey& layer) const {
    (void)settings(profile);
    return read_markers(db_, profile, layer);
}

Dataset Store::export_layer(const QString& profile, const LayerKey& key) const {
    for (const auto& layer : layers()) {
        if (layer.key == key) { return {{layer}, markers(profile, key)}; }
    }
    fail(QStringLiteral("The selected map layer does not exist."));
}

ImportReview Store::review_import(const Dataset& data, const QString& profile, bool overwrite) const {
    validate_dataset(data); (void)settings(profile);
    QString initial_id; qint64 initial_revision = 0;
    {
        auto meta = query(db_, QStringLiteral("SELECT database_id,revision FROM map_meta WHERE singleton=1"));
        if (!meta.next()) { fail(QStringLiteral("Map revision metadata is missing.")); }
        initial_id = meta.value(0).toString(); initial_revision = meta.value(1).toLongLong();
    }
    ImportReview result;
    const auto existing_maps = layers();
    QHash<QString, MapLayer> known_maps;
    for (const auto& map : existing_maps) { known_maps.insert(layer_identity(map.key), map); }
    for (const auto& map : data.maps) {
        const auto old = known_maps.constFind(layer_identity(map.key));
        if (old == known_maps.cend()) { ++result.new_maps; }
        else if (old->frame != map.frame || old->width != map.width || old->height != map.height) {
            fail(QStringLiteral("An existing layer's coordinate frame/bounds cannot be changed. Use a new layer ID."));
        }
    }
    if (existing_maps.size() + result.new_maps > max_map_layers) { fail(QStringLiteral("Import would exceed 1024 map layers.")); }
    const auto old_records = read_markers(db_, profile, std::nullopt);
    QHash<QString, MapMarker> known;
    QHash<QString, int> counts;
    for (const auto& marker : old_records) { known.insert(marker_identity(marker), marker); ++counts[layer_identity(marker.map)]; }
    QSet<QString> has_progress;
    auto progress = query(db_, QStringLiteral("SELECT source,marker_id FROM user_progress WHERE profile_id=?"), {profile});
    while (progress.next()) { has_progress.insert(progress.value(0).toString() + QChar(31) + progress.value(1).toString()); }
    for (auto marker : data.markers) {
        const auto key = marker_identity(marker);
        const auto old = known.constFind(key);
        if (old == known.cend()) { ++result.new_markers; }
        else {
            --counts[layer_identity(old->map)];
            auto metadata = marker; metadata.status = old->status;
            if (metadata != *old) { ++result.updated_markers; }
        }
        ++counts[layer_identity(marker.map)];
        const auto before = old == known.cend() ? CollectionStatus::NotCollected : old->status;
        if ((overwrite || !has_progress.contains(key)) && before != marker.status) { ++result.progress_changes; }
    }
    if (old_records.size() + result.new_markers > max_markers) { fail(QStringLiteral("Import would exceed 20,000 local markers.")); }
    for (const int count : counts) { if (count > max_layer_markers) { fail(QStringLiteral("A layer cannot contain more than 5000 markers.")); } }
    auto meta = query(db_, QStringLiteral("SELECT database_id,revision FROM map_meta WHERE singleton=1"));
    if (!meta.next()) { fail(QStringLiteral("Map revision metadata is missing.")); }
    if (meta.value(0).toString() != initial_id || meta.value(1).toLongLong() != initial_revision) { fail(QStringLiteral("The database changed during preview. Review this import again.")); }
    result.token = QCryptographicHash::hash(encode_json(data) + profile.toUtf8() + (overwrite ? "1" : "0")
        + meta.value(0).toString().toUtf8() + QByteArray::number(meta.value(1).toLongLong()), QCryptographicHash::Sha256);
    return result;
}

void Store::apply_import(const Dataset& data, const QString& profile, bool overwrite, const QByteArray& token) {
    transaction(db_, [&] {
        if (review_import(data, profile, overwrite).token != token) { fail(QStringLiteral("The database changed after preview. Review this import again.")); }
        for (const auto& map : data.maps) {
            (void)query(db_, QStringLiteral("INSERT INTO map_regions VALUES(?,?,?) ON CONFLICT(source,region) DO NOTHING"), {map.key.source, map.key.region, map.key.region});
            (void)query(db_, QStringLiteral(
                "INSERT INTO map_layers(source,region,layer,title,frame,width,height,provenance,license,data_version) VALUES(?,?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(source,region,layer) DO UPDATE SET title=excluded.title,provenance=excluded.provenance,license=excluded.license,data_version=excluded.data_version"),
                {map.key.source, map.key.region, map.key.layer, map.title, map.frame, map.width, map.height, map.provenance, map.license, map.version});
            (void)query(db_, QStringLiteral("INSERT INTO data_versions VALUES(?,?,?,?,?) ON CONFLICT(source,version) DO UPDATE SET provenance=excluded.provenance,license=excluded.license,imported_at=excluded.imported_at"),
                {map.key.source, map.version, map.provenance, map.license, now()});
        }
        const auto command = uuid();
        for (const auto& marker : data.markers) {
            (void)query(db_, QStringLiteral("INSERT INTO marker_categories VALUES(?,?) ON CONFLICT DO NOTHING"), {marker.map.source, marker.category});
            (void)query(db_, QStringLiteral(
                "INSERT INTO markers(source,id,region,layer,category,label,notes,x,y,retired,origin) VALUES(?,?,?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(source,id) DO UPDATE SET region=excluded.region,layer=excluded.layer,category=excluded.category,label=excluded.label,notes=excluded.notes,x=excluded.x,y=excluded.y,retired=excluded.retired,origin=excluded.origin"),
                {marker.map.source, marker.id, marker.map.region, marker.map.layer, marker.category, marker.label,
                 nonnull(marker.notes), marker.x, marker.y, marker.retired, marker.origin});
            auto old = query(db_, QStringLiteral("SELECT 1 FROM user_progress WHERE profile_id=? AND source=? AND marker_id=?"), {profile, marker.map.source, marker.id});
            const bool present = old.next(); old.finish();
            if (overwrite || !present) { write_progress(db_, profile, marker.map.source, marker.id, marker.status, command, QStringLiteral("import")); }
        }
        bump(db_);
    });
}

void Store::save_layer(const MapLayer& layer) {
    const Dataset data{{layer}, {}};
    const auto profile = active_profile().id;
    apply_import(data, profile, false, review_import(data, profile, false).token);
}

void Store::save_marker(const QString& profile, const MapMarker& marker) {
    auto data = export_layer(profile, marker.map);
    data.markers = {marker};
    apply_import(data, profile, false, review_import(data, profile, false).token);
}

void Store::set_status(const QString& profile, const QVector<MapMarker>& markers, CollectionStatus status) {
    (void)settings(profile); (void)stored_status(status);
    if (markers.size() > max_layer_markers) { fail(QStringLiteral("Batch exceeds 5000 markers.")); }
    transaction(db_, [&] {
        const auto command = uuid();
        QSet<QString> seen;
        for (const auto& marker : markers) {
            const auto key = marker_identity(marker);
            if (seen.contains(key)) { continue; } seen.insert(key);
            auto exists = query(db_, QStringLiteral("SELECT 1 FROM markers WHERE source=? AND id=?"), {marker.map.source, marker.id});
            if (!exists.next()) { fail(QStringLiteral("A selected marker no longer exists.")); } exists.finish();
            write_progress(db_, profile, marker.map.source, marker.id, status, command, QStringLiteral("manual"));
        }
        bump(db_);
    });
}

QVector<DetectionResult> Store::apply_detections(const QString& profile, const RecordingAnalysis& analysis, const QString& expected_marker) {
    validate_analysis(analysis); (void)settings(profile);
    QVector<DetectionResult> results;
    transaction(db_, [&] {
        const auto dataset = export_layer(profile, analysis.map);
        const auto& layer = dataset.maps.first();
        if (analysis.frame != layer.frame) { fail(QStringLiteral("Recording uses a different coordinate frame. No progress was changed.")); }
        if (analysis.reference_bounds && (analysis.reference_bounds->left() < 0 || analysis.reference_bounds->top() < 0
            || analysis.reference_bounds->right() > layer.width || analysis.reference_bounds->bottom() > layer.height)) {
            fail(QStringLiteral("Calibrated reference exceeds local layer bounds. No progress was changed."));
        }
        const auto command = uuid();
        bool changed = false;
        auto current_markers = dataset.markers;
        for (const auto& event : analysis.openings) {
            auto receipt = query(db_, QStringLiteral("SELECT marker_id FROM detection_receipts WHERE profile_id=? AND event_id=? UNION ALL SELECT marker_id FROM animation_receipts WHERE profile_id=? AND event_id=?"), {profile, event.event_id, profile, event.event_id});
            if (receipt.next()) {
                results.append({event.opened_ms, DetectionOutcome::Duplicate, DetectionReason::PreviouslyProcessed, {receipt.value(0).toString()}});
                continue;
            }
            receipt.finish();
            auto result = match_opening(event, analysis, layer, current_markers);
            if (!expected_marker.isEmpty() && (result.outcome == DetectionOutcome::Applied || result.outcome == DetectionOutcome::AlreadyCollected)
                && result.candidates != QStringList{expected_marker}) {
                result.outcome = DetectionOutcome::Review; result.reason = DetectionReason::TargetMismatch;
            }
            if (result.outcome == DetectionOutcome::Applied || result.outcome == DetectionOutcome::AlreadyCollected) {
                const auto& marker_id = result.candidates.first();
                if (result.outcome == DetectionOutcome::Applied) {
                    write_progress(db_, profile, analysis.map.source, marker_id, CollectionStatus::Collected, command, QStringLiteral("detection"));
                    for (auto& marker : current_markers) { if (marker.id == marker_id) { marker.status = CollectionStatus::Collected; break; } }
                }
                if (event.animation) {
                    const auto& p = *event.animation;
                    const auto proof = QString::fromUtf8(QJsonDocument(QJsonObject{{"burst_ms", p.burst_ms}, {"dissolved_ms", p.dissolved_ms},
                        {"ring_coverage", p.ring_coverage}, {"gold_fraction", p.gold_fraction}, {"blue_rise", p.blue_rise},
                        {"interaction_frames", p.interaction_frames}, {"dissolve_frames", p.dissolve_frames}}).toJson(QJsonDocument::Compact));
                    (void)query(db_, QStringLiteral("INSERT INTO animation_receipts VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"),
                        {profile, event.event_id, analysis.fingerprint, analysis.map.source, marker_id, proof,
                         event.position->confidence, event.opened_ms, command, analysis.provenance, analysis.license, now()});
                } else {
                    (void)query(db_, QStringLiteral("INSERT INTO detection_receipts VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"),
                        {profile, event.event_id, analysis.fingerprint, analysis.map.source, marker_id,
                         event.closed_similarity, event.opened_similarity, event.position->confidence, event.opened_ms,
                         command, analysis.provenance, analysis.license, now()});
                }
                changed = true;
            }
            results.append(result);
        }
        if (changed) { bump(db_); }
    });
    return results;
}

void Store::set_retired(const QVector<MapMarker>& markers, bool retired) {
    transaction(db_, [&] {
        for (const auto& marker : markers) {
            auto q = query(db_, QStringLiteral("UPDATE markers SET retired=? WHERE source=? AND id=?"), {retired, marker.map.source, marker.id});
            if (q.numRowsAffected() != 1) { fail(QStringLiteral("Marker no longer exists.")); }
        }
        bump(db_);
    });
}

bool Store::can_undo(const QString& profile) const {
    auto q = query(db_, QStringLiteral("SELECT 1 FROM detection_history h WHERE profile_id=? AND origin!='undo' AND NOT EXISTS(SELECT 1 FROM detection_history u WHERE u.undo_of=h.id) LIMIT 1"), {profile});
    return q.next();
}

void Store::undo_last(const QString& profile) {
    (void)settings(profile);
    transaction(db_, [&] {
        QString command;
        {
            auto q = query(db_, QStringLiteral("SELECT command_id FROM detection_history h WHERE profile_id=? AND origin!='undo' AND NOT EXISTS(SELECT 1 FROM detection_history u WHERE u.undo_of=h.id) ORDER BY id DESC LIMIT 1"), {profile});
            if (!q.next()) { return; } command = q.value(0).toString();
        }
        struct Undo { qint64 id; QString source, marker, before, after; };
        QVector<Undo> changes;
        auto q = query(db_, QStringLiteral("SELECT id,source,marker_id,before_status,after_status FROM detection_history WHERE profile_id=? AND command_id=? ORDER BY id DESC"), {profile, command});
        while (q.next()) { changes.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(), q.value(4).toString()}); }
        q.finish();
        const auto undo_command = uuid();
        for (const auto& change : changes) {
            auto current = query(db_, QStringLiteral(
                "SELECT p.status,p.revision,h.after_revision FROM user_progress p JOIN detection_history h ON h.id=(SELECT max(id) FROM detection_history WHERE profile_id=p.profile_id AND source=p.source AND marker_id=p.marker_id) WHERE p.profile_id=? AND p.source=? AND p.marker_id=?"),
                {profile, change.source, change.marker});
            if (!current.next() || current.value(0).toString() != change.after || current.value(1) != current.value(2)) { fail(QStringLiteral("Progress changed outside its history; undo was not applied.")); }
            current.finish();
            write_progress(db_, profile, change.source, change.marker, read_status(change.before), undo_command, QStringLiteral("undo"), change.id);
        }
        bump(db_);
    });
}

QVector<HistoryEvent> Store::history(const QString& profile, int limit, qint64 before_id) const {
    (void)settings(profile);
    auto q = query(db_, QStringLiteral("SELECT h.id,m.label,h.source,h.marker_id,h.before_status,h.after_status,h.origin,h.created_at,h.command_id FROM detection_history h JOIN markers m ON m.source=h.source AND m.id=h.marker_id WHERE h.profile_id=? AND (?=0 OR h.id<?) ORDER BY h.id DESC LIMIT ?"), {profile, before_id, before_id, std::clamp(limit, 1, 2000)});
    QVector<HistoryEvent> result;
    while (q.next()) { result.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(), q.value(4).toString(), q.value(5).toString(), q.value(6).toString(), q.value(7).toString(), q.value(8).toString()}); }
    return result;
}

QByteArray Store::background(const LayerKey& layer) const {
    auto q = query(db_, QStringLiteral("SELECT background FROM map_layers WHERE source=? AND region=? AND layer=?"), layer_values(layer));
    if (!q.next()) { fail(QStringLiteral("Map layer is missing.")); }
    return q.value(0).toByteArray();
}

QVector<HistoryEvent> Store::history_after(const QString& profile, qint64 after_id, int limit) const {
    (void)settings(profile);
    if (after_id < 0) { fail(QStringLiteral("Invalid history cursor.")); }
    auto q = query(db_, QStringLiteral("SELECT h.id,m.label,h.source,h.marker_id,h.before_status,h.after_status,h.origin,h.created_at,h.command_id FROM detection_history h JOIN markers m ON m.source=h.source AND m.id=h.marker_id WHERE h.profile_id=? AND h.id>? ORDER BY h.id LIMIT ?"), {profile, after_id, std::clamp(limit, 1, 2000)});
    QVector<HistoryEvent> result;
    while (q.next()) { result.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(), q.value(4).toString(), q.value(5).toString(), q.value(6).toString(), q.value(7).toString(), q.value(8).toString()}); }
    return result;
}

QString Store::background_credit(const LayerKey& layer) const {
    auto q = query(db_, QStringLiteral("SELECT background_credit,background_license FROM map_layers WHERE source=? AND region=? AND layer=?"), layer_values(layer));
    if (!q.next()) { return {}; }
    return q.value(0).toString() + QStringLiteral(" · ") + q.value(1).toString();
}

void Store::set_background(const LayerKey& layer, const QByteArray& png, const QString& credit, const QString& license) {
    if (!png.isEmpty()) {
        (void)decode_background(png);
        if (credit.trimmed().isEmpty() || license.trimmed().isEmpty() || credit.size() > 512 || license.size() > 512) { fail(QStringLiteral("PNG source and usage rights are required (up to 512 characters each).")); }
    }
    transaction(db_, [&] {
        const auto previous = background(layer).size();
        const auto total = scalar(db_, QStringLiteral("SELECT coalesce(sum(length(background)),0) FROM map_layers")).toLongLong();
        if (total - previous + png.size() > 128 * 1024 * 1024) { fail(QStringLiteral("Background images exceed the 128 MiB local limit.")); }
        auto q = query(db_, QStringLiteral("UPDATE map_layers SET background=?,background_credit=?,background_license=? WHERE source=? AND region=? AND layer=?"),
            {png.isEmpty() ? QVariant{} : QVariant(png), nonnull(credit), nonnull(license), layer.source, layer.region, layer.layer});
        if (q.numRowsAffected() != 1) { fail(QStringLiteral("Map layer no longer exists.")); } bump(db_);
    });
}

void Store::backup(const QString& destination) const {
    const QFileInfo target(destination);
    if (target.exists()) { fail(QStringLiteral("Choose a new backup filename; existing files are never overwritten.")); }
    QTemporaryDir temporary(target.absolutePath() + QStringLiteral("/.teyvat-backup-XXXXXX"));
    if (!temporary.isValid()) { fail(QStringLiteral("Cannot create a backup in this directory.")); }
    const auto snapshot = temporary.filePath(QStringLiteral("snapshot.db"));
    (void)query(db_, QStringLiteral("VACUUM INTO ?"), {snapshot});
    if (!QFile::rename(snapshot, target.absoluteFilePath())) { fail(QStringLiteral("Cannot finalize the backup. Existing files were preserved.")); }
}

void Store::restore(const QString& source) {
    QTemporaryDir temporary(QFileInfo(path_).absolutePath() + QStringLiteral("/.teyvat-restore-XXXXXX"));
    if (!temporary.isValid()) { fail(QStringLiteral("Cannot create a restore staging directory.")); }
    const auto snapshot = temporary.filePath(QStringLiteral("snapshot.db"));
    const auto connection = uuid();
    int backup_version = 0;
    {
        auto candidate = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        candidate.setDatabaseName(QFileInfo(source).absoluteFilePath());
        candidate.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=3000"));
        try {
            if (!candidate.open()) { fail(QStringLiteral("Cannot open the backup read-only.")); }
            backup_version = scalar(candidate, QStringLiteral("PRAGMA user_version")).toInt();
            if (scalar(candidate, QStringLiteral("PRAGMA integrity_check")).toString() != QStringLiteral("ok")
                || scalar(candidate, QStringLiteral("PRAGMA application_id")).toInt() != application_id
                || (backup_version != schema_version && backup_version != 5)
                || (backup_version == schema_version && schema_description(candidate) != schema_description(db_))) {
                fail(QStringLiteral("Backup is damaged or has an incompatible schema."));
            }
            auto fk = query(candidate, QStringLiteral("PRAGMA foreign_key_check"));
            if (fk.next()) { fail(QStringLiteral("Backup has invalid references.")); } fk.finish();
            (void)query(candidate, QStringLiteral("VACUUM INTO ?"), {snapshot});
            candidate.close();
        } catch (...) { candidate.close(); candidate = QSqlDatabase(); QSqlDatabase::removeDatabase(connection); throw; }
    }
    QSqlDatabase::removeDatabase(connection);
    if (backup_version == 5) {
        Store upgraded(snapshot);
        if (schema_description(upgraded.db_) != schema_description(db_)) {
            fail(QStringLiteral("Backup is damaged or has an incompatible schema."));
        }
    }
    backup(path_ + QStringLiteral(".before-restore-%1.bak").arg(uuid()));
    (void)query(db_, QStringLiteral("ATTACH DATABASE ? AS restored"), {snapshot});
    (void)query(db_, QStringLiteral("PRAGMA foreign_keys=OFF"));
    const QStringList tables{QStringLiteral("animation_receipts"), QStringLiteral("detection_receipts"), QStringLiteral("detection_history"), QStringLiteral("user_progress"), QStringLiteral("markers"),
        QStringLiteral("marker_categories"), QStringLiteral("map_layers"), QStringLiteral("map_regions"), QStringLiteral("data_versions"),
        QStringLiteral("map_meta"), QStringLiteral("app_state"), QStringLiteral("settings"), QStringLiteral("profiles")};
    try {
        transaction(db_, [&] {
            for (const auto& table : tables) { (void)query(db_, QStringLiteral("DELETE FROM main.%1").arg(table)); }
            for (auto i = tables.crbegin(); i != tables.crend(); ++i) { (void)query(db_, QStringLiteral("INSERT INTO main.%1 SELECT * FROM restored.%1").arg(*i)); }
            auto fk = query(db_, QStringLiteral("PRAGMA foreign_key_check"));
            if (fk.next()) { fail(QStringLiteral("Restored references are invalid.")); } fk.finish();
            (void)settings(active_profile().id);
            if (scalar(db_, QStringLiteral("SELECT count(*) FROM markers")).toInt() > max_markers) { fail(QStringLiteral("Backup exceeds the local marker limit.")); }
            if (scalar(db_, QStringLiteral("SELECT coalesce(sum(length(background)),0) FROM map_layers")).toLongLong() > 128 * 1024 * 1024) { fail(QStringLiteral("Backup exceeds the background image limit.")); }
            for (const auto& layer : layers()) {
                validate_dataset(export_layer(active_profile().id, layer.key));
                const auto image = background(layer.key); if (!image.isEmpty()) { (void)decode_background(image); }
            }
            (void)query(db_, QStringLiteral("UPDATE map_meta SET database_id=?,revision=revision+1"), {uuid()});
        });
    } catch (...) {
        (void)query(db_, QStringLiteral("PRAGMA foreign_keys=ON"));
        (void)query(db_, QStringLiteral("DETACH DATABASE restored")); throw;
    }
    (void)query(db_, QStringLiteral("PRAGMA foreign_keys=ON"));
    (void)query(db_, QStringLiteral("DETACH DATABASE restored"));
}
} // namespace teyvat
